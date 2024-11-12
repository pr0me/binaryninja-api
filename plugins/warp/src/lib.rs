use binaryninja::architecture::{
    Architecture, ImplicitRegisterExtend, Register as BNRegister, RegisterInfo,
};
use binaryninja::basicblock::BasicBlock as BNBasicBlock;
use binaryninja::binaryview::BinaryViewExt;
use binaryninja::function::{Function as BNFunction, NativeBlock};
use binaryninja::llil;
use binaryninja::llil::{
    ExprInfo, FunctionMutability, InstrInfo, NonSSA, NonSSAVariant, Register, VisitorAction,
};
use binaryninja::rc::Ref as BNRef;
use warp::signature::basic_block::BasicBlockGUID;
use warp::signature::function::constraints::FunctionConstraints;
use warp::signature::function::{Function, FunctionGUID};

use crate::cache::{
    cached_adjacency_constraints, cached_call_site_constraints, cached_function_guid,
};
use crate::convert::{from_bn_symbol, from_bn_type};

pub mod cache;
pub mod convert;
mod matcher;
/// Only used when compiled for cdylib target.
mod plugin;

pub fn build_function<A: Architecture, M: FunctionMutability, V: NonSSAVariant>(
    func: &BNFunction,
    llil: &llil::Function<A, M, NonSSA<V>>,
) -> Function {
    let bn_fn_ty = func.function_type();
    Function {
        guid: cached_function_guid(func, llil),
        symbol: from_bn_symbol(&func.symbol()),
        ty: from_bn_type(&func.view(), &bn_fn_ty, 255),
        constraints: FunctionConstraints {
            // NOTE: Adding adjacent only works if analysis is complete.
            adjacent: cached_adjacency_constraints(func),
            call_sites: cached_call_site_constraints(func),
            // TODO: Add caller sites (when adjacent and call sites are minimal)
            // NOTE: Adding caller sites only works if analysis is complete.
            caller_sites: Default::default(),
        },
    }
}

/// Basic blocks sorted from high to low.
pub fn sorted_basic_blocks(func: &BNFunction) -> Vec<BNRef<BNBasicBlock<NativeBlock>>> {
    let mut basic_blocks = func
        .basic_blocks()
        .iter()
        .map(|bb| bb.clone())
        .collect::<Vec<_>>();
    basic_blocks.sort_by_key(|f| f.raw_start());
    basic_blocks
}

pub fn function_guid<A: Architecture, M: FunctionMutability, V: NonSSAVariant>(
    func: &BNFunction,
    llil: &llil::Function<A, M, NonSSA<V>>,
) -> FunctionGUID {
    let basic_blocks = sorted_basic_blocks(func);
    let basic_block_guids = basic_blocks
        .iter()
        .map(|bb| basic_block_guid(bb, llil))
        .collect::<Vec<_>>();
    FunctionGUID::from_basic_blocks(&basic_block_guids)
}

pub fn basic_block_guid<A: Architecture, M: FunctionMutability, V: NonSSAVariant>(
    basic_block: &BNBasicBlock<NativeBlock>,
    llil: &llil::Function<A, M, NonSSA<V>>,
) -> BasicBlockGUID {
    let func = basic_block.function();
    let view = func.view();
    let arch = func.arch();
    let max_instr_len = arch.max_instr_len();

    // NOPs and useless moves are blacklisted to allow for hot-patchable functions.
    let is_blacklisted_instr = |info: InstrInfo<A, M, NonSSA<V>>| {
        match info {
            InstrInfo::Nop(_) => true,
            InstrInfo::SetReg(op) => {
                match op.source_expr().info() {
                    ExprInfo::Reg(source_op) if op.dest_reg() == source_op.source_reg() => {
                        match op.dest_reg() {
                            Register::ArchReg(r) => {
                                // If this register has no implicit extend then we can safely assume it's a NOP.
                                // Ex. on x86_64 we don't want to remove `mov edi, edi` as it will zero the upper 32 bits.
                                // Ex. on x86 we do want to remove `mov edi, edi` as it will not have a side effect like above.
                                matches!(
                                    r.info().implicit_extend(),
                                    ImplicitRegisterExtend::NoExtend
                                )
                            }
                            Register::Temp(_) => false,
                        }
                    }
                    _ => false,
                }
            }
            _ => false,
        }
    };

    let basic_block_range = basic_block.raw_start()..basic_block.raw_end();
    let mut basic_block_bytes = Vec::with_capacity(basic_block_range.count());
    for instr_addr in basic_block.into_iter() {
        let mut instr_bytes = view.read_vec(instr_addr, max_instr_len);
        if let Some(instr_info) = arch.instruction_info(&instr_bytes, instr_addr) {
            instr_bytes.truncate(instr_info.len());
            if let Some(instr_llil) = llil.instruction_at(instr_addr) {
                // If instruction is blacklisted don't include the bytes.
                if !is_blacklisted_instr(instr_llil.info()) {
                    if instr_llil.visit_tree(&mut |_expr, expr_info| match expr_info {
                        ExprInfo::ConstPtr(op) if view.segment_at(op.value()).is_some() => {
                            // Constant Pointer must be in a segment for it to be relocatable.
                            VisitorAction::Halt
                        }
                        ExprInfo::ExternPtr(_) => VisitorAction::Halt,
                        ExprInfo::Const(op)
                            if !view.functions_at(op.value()).is_empty()
                                || view.data_variable_at_address(op.value()).is_some() =>
                        {
                            // Constant Pointer promotion for Constants that would be promoted at MLIL.
                            // If the value backs a symbol than we are eagerly masking for the sake of simplicity.
                            VisitorAction::Halt
                        }
                        _ => VisitorAction::Descend,
                    }) == VisitorAction::Halt
                    {
                        // Found a variant instruction, mask off entire instruction.
                        instr_bytes.fill(0);
                    }
                    // Add the instructions bytes to the basic blocks bytes
                    basic_block_bytes.extend(instr_bytes);
                }
            }
        }
    }

    BasicBlockGUID::from(basic_block_bytes.as_slice())
}

#[cfg(test)]
mod tests {
    use crate::cache::cached_function_guid;
    use binaryninja::binaryview::BinaryViewExt;
    use binaryninja::headless::Session;
    use std::path::PathBuf;
    use std::sync::OnceLock;

    static INIT: OnceLock<Session> = OnceLock::new();

    fn get_session<'a>() -> &'a Session {
        // TODO: This is not shared between other test modules, should still be fine (mutex in core now).
        INIT.get_or_init(|| Session::new())
    }

    #[test]
    fn insta_signatures() {
        let session = get_session();
        let out_dir = env!("OUT_DIR").parse::<PathBuf>().unwrap();
        for entry in std::fs::read_dir(out_dir).expect("Failed to read OUT_DIR") {
            let entry = entry.expect("Failed to read directory entry");
            let path = entry.path();
            if path.is_file() {
                if let Some(path_str) = path.to_str() {
                    if path_str.ends_with("library.o") {
                        if let Some(inital_bv) = session.load(path_str) {
                            let mut functions = inital_bv
                                .functions()
                                .iter()
                                .map(|f| cached_function_guid(&f, &f.low_level_il().unwrap()))
                                .collect::<Vec<_>>();
                            functions.sort_by_key(|guid| guid.guid);
                            insta::assert_debug_snapshot!(functions);
                        }
                    }
                }
            }
        }
    }
}
