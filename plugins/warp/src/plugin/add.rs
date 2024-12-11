use crate::cache::{cached_function, cached_type_references};
use crate::matcher::invalidate_function_matcher_cache;
use binaryninja::binaryview::BinaryView;
use binaryninja::command::FunctionCommand;
use binaryninja::function::Function;
use std::io::Write;
use std::thread;

pub struct AddFunctionSignature;

impl FunctionCommand for AddFunctionSignature {
    fn action(&self, view: &BinaryView, func: &Function) {
        let view = view.to_owned();
        let func = func.to_owned();
        thread::spawn(move || {
            let Ok(llil) = func.low_level_il() else {
                log::error!("Could not get low level IL for function.");
                return;
            };

            let Some(save_file) = binaryninja::interaction::get_save_filename_input(
                "Use Signature File",
                "*.sbin",
                "user.sbin",
            ) else {
                return;
            };

            let mut data = warp::signature::Data::default();
            if let Ok(file_bytes) = std::fs::read(&save_file) {
                // If the file we are adding the function to already has data we should preserve it!
                log::info!("Signature file already exists, preserving data...");
                let Some(file_data) = warp::signature::Data::from_bytes(&file_bytes) else {
                    log::error!("Could not get data from signature file: {:?}", save_file);
                    return;
                };
                data = file_data;
            };

            // Now add our function to the data.
            data.functions.push(cached_function(&func, &llil));

            if let Some(ref_ty_cache) = cached_type_references(&view) {
                let referenced_types = ref_ty_cache
                    .cache
                    .iter()
                    .filter_map(|t| t.to_owned())
                    .collect::<Vec<_>>();

                data.types.extend(referenced_types);
            }

            if let Ok(mut file) = std::fs::File::create(&save_file) {
                match file.write_all(&data.to_bytes()) {
                    Ok(_) => {
                        log::info!("Signature file saved successfully.");
                        // Force rebuild platform matcher.
                        invalidate_function_matcher_cache();
                    }
                    Err(e) => log::error!("Failed to write data to signature file: {:?}", e),
                }
            } else {
                log::error!("Could not create signature file: {:?}", save_file);
            }
        });
    }

    fn valid(&self, _view: &BinaryView, _func: &Function) -> bool {
        true
    }
}
