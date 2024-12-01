use crate::cache::{cached_function, cached_type_references};
use crate::matcher::invalidate_function_matcher_cache;
use binaryninja::binaryview::{BinaryView, BinaryViewExt};
use binaryninja::command::Command;
use binaryninja::function::Function;
use binaryninja::rc::Guard;
use rayon::prelude::*;
use std::io::Write;
use std::sync::atomic::AtomicUsize;
use std::sync::atomic::Ordering::Relaxed;
use std::thread;
use std::time::Instant;

pub struct CreateSignatureFile;

// TODO: Prompt the user to add the newly created signature file to the signature blacklist (so that it doesn't keep getting applied)

impl Command for CreateSignatureFile {
    fn action(&self, view: &BinaryView) {
        let is_function_named = |f: &Guard<Function>| {
            !f.symbol().short_name().as_str().contains("sub_") || f.has_user_annotations()
        };

        let mut signature_dir = binaryninja::user_directory().unwrap().join("signatures/");
        if let Some(default_plat) = view.default_platform() {
            // If there is a default platform, put the signature in there.
            signature_dir.push(default_plat.name().to_string());
        }
        let view = view.to_owned();
        thread::spawn(move || {
            let total_functions = view.functions().len();
            let done_functions = AtomicUsize::default();
            let background_task = binaryninja::backgroundtask::BackgroundTask::new(
                format!("Generating signatures... ({}/{})", 0, total_functions),
                true,
            )
            .unwrap();

            let start = Instant::now();

            let mut data = warp::signature::Data::default();
            data.functions.par_extend(
                view.functions()
                    .par_iter()
                    .inspect(|_| {
                        done_functions.fetch_add(1, Relaxed);
                        background_task.set_progress_text(format!("Generating signatures... ({}/{})", done_functions.load(Relaxed), total_functions))
                    })
                    .filter(is_function_named)
                    .filter(|f| !f.analysis_skipped())
                    .filter_map(|func| {
                        let llil = func.low_level_il().ok()?;
                        Some(cached_function(&func, &llil))
                    }),
            );

            if let Some(ref_ty_cache) = cached_type_references(&view) {
                let referenced_types = ref_ty_cache
                    .cache
                    .iter()
                    .filter_map(|t| t.to_owned())
                    .collect::<Vec<_>>();

                data.types.extend(referenced_types);
            }

            log::info!("Signature generation took {:?}", start.elapsed());

            if let Some(sig_file_name) = binaryninja::interaction::get_text_line_input(
                "Signature File",
                "Create Signature File",
            ) {
                let save_file = signature_dir.join(sig_file_name + ".sbin");
                log::info!("Saving to signatures to {:?}...", &save_file);
                // TODO: Should we overwrite? Prompt user.
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
            }

            background_task.finish();
        });
    }

    fn valid(&self, _view: &BinaryView) -> bool {
        true
    }
}
