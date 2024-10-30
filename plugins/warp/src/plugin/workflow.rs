use crate::cache::cached_function_guid;
use crate::matcher::cached_function_matcher;
use binaryninja::backgroundtask::BackgroundTask;
use binaryninja::binaryview::{BinaryView, BinaryViewExt};
use binaryninja::llil;
use binaryninja::workflow::{Activity, AnalysisContext, Workflow};
use std::time::Instant;
use binaryninja::command::Command;

const MATCHER_ACTIVITY_NAME: &str = "analysis.plugins.warp.matcher";
// NOTE: runOnce is off because previously matched functions need info applied.
const MATCHER_ACTIVITY_CONFIG: &str = r#"{
    "name": "analysis.plugins.warp.matcher",
    "title" : "WARP Matcher",
    "description": "This analysis step applies WARP info to matched functions...",
    "eligibility": {
        "auto": {},
        "runOnce": true
    }
}"#;

const GUID_ACTIVITY_NAME: &str = "analysis.plugins.warp.guid";
const GUID_ACTIVITY_CONFIG: &str = r#"{
    "name": "analysis.plugins.warp.guid",
    "title" : "WARP GUID Generator",
    "description": "This analysis step generates the GUID for all analyzed functions...",
    "eligibility": {
        "auto": {},
        "runOnce": true
    }
}"#;

pub struct RunMatcher;

impl Command for RunMatcher {
    fn action(&self, view: &BinaryView) {
        let view = view.to_owned();
        log::info!("Re-running matcher for {:?}", view);
        // TODO: Check to see if the GUID cache is empty and ask the user if they want to regenerate the guids.
        std::thread::spawn(move || {
            let background_task = BackgroundTask::new("Matching on functions...", false).unwrap();
            let start = Instant::now();
            view.functions()
                .iter()
                .for_each(|function| cached_function_matcher(&function));
            log::info!("Function matching took {:?}", start.elapsed());
            background_task.finish();  
        });
    }

    fn valid(&self, _view: &BinaryView) -> bool {
        true
    }
}

pub fn insert_workflow() {
    let matcher_activity = |ctx: &AnalysisContext| {
        let view = ctx.view();
        let background_task = BackgroundTask::new("Matching on functions...", false).unwrap();
        let start = Instant::now();
        view.functions()
            .iter()
            .for_each(|function| cached_function_matcher(&function));
        log::info!("Function matching took {:?}", start.elapsed());
        background_task.finish();
    };

    let guid_activity = |ctx: &AnalysisContext| {
        let function = ctx.function();
        if let Some(llil) = unsafe { ctx.llil_function::<llil::NonSSA<llil::RegularNonSSA>>() } {
            cached_function_guid(&function, &llil);
        }
    };

    let function_meta_workflow = Workflow::new_from_copy("core.function.metaAnalysis");
    let guid_activity = Activity::new_with_action(GUID_ACTIVITY_CONFIG, guid_activity);
    function_meta_workflow
        .register_activity(&guid_activity)
        .unwrap();
    function_meta_workflow.insert("core.function.runFunctionRecognizers", [GUID_ACTIVITY_NAME]);
    function_meta_workflow.register().unwrap();

    let module_meta_workflow = Workflow::new_from_copy("core.module.metaAnalysis");
    let matcher_activity = Activity::new_with_action(MATCHER_ACTIVITY_CONFIG, matcher_activity);
    module_meta_workflow
        .register_activity(&matcher_activity)
        .unwrap();
    module_meta_workflow.insert("core.module.notifyCompletion", [MATCHER_ACTIVITY_NAME]);
    module_meta_workflow.register().unwrap();
}
