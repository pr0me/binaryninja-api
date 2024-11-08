use std::collections::{HashMap, HashSet};
use std::fs::File;
use std::io::Read;
use std::path::{Path, PathBuf};

use ar::Archive;
use clap::{arg, Parser};
use rayon::prelude::*;

use binaryninja::binaryview::{BinaryView, BinaryViewExt};
use binaryninja::function::Function as BNFunction;
use binaryninja::rc::Guard as BNGuard;
use serde_json::json;
use walkdir::WalkDir;
use warp::r#type::ComputedType;
use warp::signature::function::constraints::FunctionConstraint;
use warp::signature::function::{Function, FunctionGUID};
use warp::signature::Data;
use warp_ninja::convert::from_bn_type;

#[derive(Parser, Debug)]
#[command(about, long_about)]
/// A simple CLI utility to generate WARP signature files headlessly using Binary Ninja.
///
/// NOTE: This requires a headless compatible Binary Ninja, make sure it's in your path.
struct Args {
    /// Path to create signatures from, this can be:
    /// - A binary (that can be opened with Binary Ninja)
    /// - A directory (all files will be merged)
    /// - An archive (with ext: a, lib, rlib)
    /// - A BNDB
    /// - A Signature file (sbin)
    #[arg(index = 1, verbatim_doc_comment)]
    path: PathBuf,

    /// The signature output file
    ///
    /// NOTE: If not specified the output will be the input path with the sbin extension
    /// as an example `mylib.a` will output `mylib.sbin`.
    #[arg(index = 2)]
    output: Option<PathBuf>,

    /// Should we overwrite output file
    ///
    /// NOTE: If the file exists we will exit early to prevent wasted effort.
    #[arg(short, long)]
    overwrite: Option<bool>,

    /// The external debug information file to use
    #[arg(short, long)]
    debug_info: Option<PathBuf>,
}

fn main() {
    let args = Args::parse();
    env_logger::Builder::from_env(env_logger::Env::default().default_filter_or("info")).init();

    // If no output file was given, just prepend binary with extension sbin
    let output_file = args.output.unwrap_or(args.path.with_extension("sbin"));

    if output_file.exists() && !args.overwrite.unwrap_or(false) {
        log::info!("Output file already exists, skipping... {:?}", output_file);
        return;
    }

    log::debug!("Starting Binary Ninja session...");
    let _headless_session = binaryninja::headless::Session::new();

    log::info!("Creating functions for {:?}...", args.path);
    let start = std::time::Instant::now();
    let data = data_from_file(&args.path)
        .expect("Failed to read data, check your license and Binary Ninja version!");
    log::info!("Functions created in {:?}", start.elapsed());

    // TODO: Add a way to override the symbol type to make it a different function symbol.
    // TODO: Right now the consumers must dictate that.
    // TODO: The binja_warp consumer sets this to library function fwiw

    if !data.functions.is_empty() {
        std::fs::write(&output_file, data.to_bytes()).expect("Failed to write functions to file");
        log::info!(
            "{} functions written to {:?}...",
            data.functions.len(),
            output_file
        );
    } else {
        log::warn!("No functions found for binary {:?}...", args.path);
    }
}

fn data_from_view(view: &BinaryView) -> Data {
    let mut data = Data::default();
    let is_function_named = |f: &BNGuard<BNFunction>| {
        !f.symbol().short_name().as_str().contains("sub_") || f.has_user_annotations()
    };

    data.functions = view
        .functions()
        .iter()
        .filter(is_function_named)
        .filter_map(|f| {
            let llil = f.low_level_il().ok()?;
            Some(warp_ninja::cache::cached_function(&f, &llil))
        })
        .collect::<Vec<_>>();
    data.types.extend(view.types().iter().map(|ty| {
        let ref_ty = ty.type_object().to_owned();
        ComputedType::new(from_bn_type(view, &ref_ty, u8::MAX))
    }));
    data
}

fn data_from_archive<R: Read>(mut archive: Archive<R>) -> Option<Data> {
    // TODO: I feel like this is a hack...
    let temp_dir = tempdir::TempDir::new("tmp_archive").ok()?;
    // Iterate through the entries in the ar file and make a temp dir with them
    let mut entry_files: HashSet<PathBuf> = HashSet::new();
    while let Some(entry) = archive.next_entry() {
        match entry {
            Ok(mut entry) => {
                let name = String::from_utf8_lossy(entry.header().identifier()).to_string();
                // Write entry data to a temp directory
                let output_path = temp_dir.path().join(&name);
                if !entry_files.contains(&output_path) {
                    let mut output_file =
                        File::create(&output_path).expect("Failed to create entry file");
                    std::io::copy(&mut entry, &mut output_file).expect("Failed to read entry data");
                    entry_files.insert(output_path);
                } else {
                    log::debug!("Skipping already inserted entry: {}", name);
                }
            }
            Err(e) => {
                log::error!("Failed to read archive entry: {}", e);
            }
        }
    }

    // Create the data.
    let entry_data = entry_files
        .into_par_iter()
        .filter_map(|path| {
            log::debug!("Creating data for ENTRY {:?}...", path);
            data_from_file(&path)
        })
        .collect::<Vec<_>>();

    let mut archive_data = Data::merge(&entry_data);
    // Archives can resolve like this, its assumed that the symbols are weak.
    resolve_guids(&mut archive_data.functions);
    Some(archive_data)
}

fn data_from_directory(dir: PathBuf) -> Option<Data> {
    let files = WalkDir::new(dir)
        .into_iter()
        .filter_map(|e| {
            let path = e.ok()?.into_path();
            if path.is_file() {
                Some(path)
            } else {
                None
            }
        })
        .collect::<Vec<_>>();

    let unmerged_data = files
        .into_par_iter()
        .filter_map(|path| {
            log::debug!("Creating data for FILE {:?}...", path);
            data_from_file(&path)
        })
        .collect::<Vec<_>>();

    if !unmerged_data.is_empty() {
        Some(Data::merge(&unmerged_data))
    } else {
        None
    }
}

fn resolve_guids(functions: &mut [Function]) {
    let guid_map: HashMap<String, FunctionGUID> = functions
        .iter()
        .map(|f| (f.symbol.name.to_owned(), f.guid))
        .collect();

    let resolve_constraint = |mut constraint: FunctionConstraint| {
        // If we don't have a guid for the constraint grab it from the symbol name
        if constraint.guid.is_none() {
            if let Some(symbol) = &constraint.symbol {
                constraint.guid = guid_map.get(&symbol.name).copied();
            }
        }
        constraint
    };

    functions.iter_mut().for_each(|f| {
        f.constraints.call_sites = f
            .constraints
            .call_sites
            .iter()
            .cloned()
            .map(resolve_constraint)
            .collect();
        f.constraints.adjacent = f
            .constraints
            .adjacent
            .iter()
            .cloned()
            .map(resolve_constraint)
            .collect();
    });
}

// TODO: Pass settings.
fn data_from_file(path: &Path) -> Option<Data> {
    // TODO: Add external debug info files.
    // TODO: Support IDB's through debug info
    let settings_json = json!({
        "analysis.linearSweep.autorun": false,
        "analysis.signatureMatcher.autorun": false,
        // We don't need these
        "analysis.warp.matcher": false,
        "analysis.warp.guid": false,
    });

    match path.extension() {
        Some(ext) if ext == "a" || ext == "lib" || ext == "rlib" => {
            let archive_file = File::open(path).expect("Failed to open archive file");
            let archive = Archive::new(archive_file);
            data_from_archive(archive)
        }
        Some(ext) if ext == "sbin" => {
            let contents = std::fs::read(path).ok()?;
            Data::from_bytes(&contents)
        }
        _ if path.is_dir() => data_from_directory(path.into()),
        _ => {
            let path_str = path.to_str().unwrap();
            let view =
                binaryninja::load_with_options(path_str, true, Some(settings_json.to_string()))?;
            let data = data_from_view(&view);
            view.file().close();
            Some(data)
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use warp::r#type::guid::TypeGUID;
    use warp_ninja::convert::from_bn_type;

    #[test]
    fn test_data_from_file() {
        env_logger::init();
        // TODO: Store oracles here to get more out of this test.
        let out_dir = env!("OUT_DIR").parse::<PathBuf>().unwrap();
        let _headless_session = binaryninja::headless::Session::new();
        for entry in std::fs::read_dir(out_dir).expect("Failed to read OUT_DIR") {
            let entry = entry.expect("Failed to read directory entry");
            let path = entry.path();
            if path.is_file() {
                let result = data_from_file(&path);
                assert!(result.is_some());
            }
        }
    }

    #[test]
    fn check_for_leaks() {
        let session = binaryninja::headless::Session::new();
        let out_dir = env!("OUT_DIR").parse::<PathBuf>().unwrap();
        for entry in std::fs::read_dir(out_dir).expect("Failed to read OUT_DIR") {
            let entry = entry.expect("Failed to read directory entry");
            let path = entry.path();
            if path.is_file() {
                if let Some(inital_bv) = session.load(path.to_str().unwrap()) {
                    let result = data_from_file(&path);
                    assert!(result.is_some());
                    // Hold on to a reference to the core to prevent view getting dropped in worker thread.
                    let core_ref = inital_bv
                        .functions()
                        .iter()
                        .next()
                        .map(|f| f.unresolved_stack_adjustment_graph());
                    // Drop the file and view.
                    inital_bv.file().close();
                    std::mem::drop(inital_bv);
                    let initial_memory_info = binaryninja::memory_info();
                    if let Some(second_bv) = session.load(path.to_str().unwrap()) {
                        let result = data_from_file(&path);
                        assert!(result.is_some());
                        // Hold on to a reference to the core to prevent view getting dropped in worker thread.
                        let core_ref = second_bv
                            .functions()
                            .iter()
                            .next()
                            .map(|f| f.unresolved_stack_adjustment_graph());
                        // Drop the file and view.
                        second_bv.file().close();
                        std::mem::drop(second_bv);
                        let final_memory_info = binaryninja::memory_info();
                        for info in initial_memory_info {
                            let initial_count = info.1;
                            if let Some(&final_count) = final_memory_info.get(&info.0) {
                                assert!(
                                    final_count <= initial_count,
                                    "{}: final objects {} vs initial objects {}",
                                    info.0,
                                    final_count,
                                    initial_count
                                );
                            }
                        }
                    }
                }
            }
        }
    }
}
