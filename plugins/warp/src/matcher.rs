use binaryninja::architecture::Architecture as BNArchitecture;
use binaryninja::backgroundtask::BackgroundTask;
use binaryninja::binaryview::{BinaryView, BinaryViewExt};
use binaryninja::function::Function as BNFunction;
use binaryninja::platform::Platform;
use binaryninja::rc::Guard;
use binaryninja::rc::Ref as BNRef;
use dashmap::DashMap;
use std::cmp::Ordering;
use std::collections::{HashMap, HashSet};
use std::hash::{DefaultHasher, Hasher};
use std::path::PathBuf;
use std::sync::OnceLock;
use walkdir::{DirEntry, WalkDir};
use warp::r#type::class::TypeClass;
use warp::r#type::guid::TypeGUID;
use warp::r#type::Type;
use warp::signature::function::{Function, FunctionGUID};
use warp::signature::Data;

use crate::cache::{cached_call_site_constraints, cached_function_match, try_cached_function_guid};
use crate::convert::to_bn_type;
use crate::plugin::on_matched_function;

// TODO: Make this configurable.
pub const TRIVIAL_FUNCTION_DELTA_THRESHOLD: u64 = 20;

pub static PLAT_MATCHER_CACHE: OnceLock<DashMap<PlatformID, Matcher>> = OnceLock::new();

pub fn cached_function_matcher(function: &BNFunction) {
    let platform = function.platform();
    let platform_id = PlatformID::from(platform.as_ref());
    let matcher_cache = PLAT_MATCHER_CACHE.get_or_init(Default::default);
    match matcher_cache.get(&platform_id) {
        Some(matcher) => matcher.match_function(function),
        None => {
            let matcher = Matcher::from_platform(platform);
            matcher.match_function(function);
            matcher_cache.insert(platform_id, matcher);
        }
    }
}

// TODO: Maybe just clear individual platforms? This works well enough either way.
pub fn invalidate_function_matcher_cache() {
    let matcher_cache = PLAT_MATCHER_CACHE.get_or_init(Default::default);
    matcher_cache.clear();
}

pub struct Matcher {
    pub functions: DashMap<FunctionGUID, Vec<Function>>,
    pub types: DashMap<TypeGUID, Type>,
    pub named_types: DashMap<String, Type>,
}

impl Matcher {
    /// Create a matcher from the platforms signature subdirectory.
    pub fn from_platform(platform: BNRef<Platform>) -> Self {
        let platform_name = platform.name().to_string();
        let task = BackgroundTask::new(
            format!("Getting platform matcher data... {}", platform_name),
            false,
        )
        .unwrap();
        // Get core signatures for the given platform
        let install_dir = binaryninja::install_directory().unwrap();
        let core_dir = install_dir.parent().unwrap();
        #[cfg(target_os = "macos")]
        let root_core_sig_dir = core_dir.join("Resources").join("signatures");
        #[cfg(not(target_os = "macos"))]
        let root_core_sig_dir = core_dir.join("signatures");
        let plat_core_sig_dir = root_core_sig_dir.join(&platform_name);
        let mut data = get_data_from_dir(&plat_core_sig_dir);

        // Get user signatures for the given platform
        let user_dir = binaryninja::user_directory().unwrap();
        let root_user_sig_dir = user_dir.join("signatures");
        let plat_user_sig_dir = root_user_sig_dir.join(&platform_name);
        let user_data = get_data_from_dir(&plat_user_sig_dir);

        data.extend(user_data);

        // TODO: If a user signature has the same name as a core signature, remove the core signature.

        task.set_progress_text("Gathering matcher functions...");

        // Get functions for comprehensive matching.
        let functions = data
            .iter()
            .flat_map(|(_, data)| {
                data.functions.iter().fold(DashMap::new(), |map, func| {
                    #[allow(clippy::unwrap_or_default)]
                    map.entry(func.guid)
                        .or_insert_with(Vec::new)
                        .push(func.clone());
                    map
                })
            })
            .map(|(guid, mut funcs)| {
                funcs.sort_by_key(|f| f.symbol.name.to_owned());
                funcs.dedup_by_key(|f| f.symbol.name.to_owned());
                (guid, funcs)
            })
            .collect();

        task.set_progress_text("Gathering matcher types...");

        let types = data
            .iter()
            .flat_map(|(_, data)| {
                data.types.iter().fold(DashMap::new(), |map, comp_ty| {
                    map.insert(comp_ty.guid, comp_ty.ty.clone());
                    map
                })
            })
            .collect();

        task.set_progress_text("Gathering matcher named types...");

        // TODO: We store a duplicate lookup for named references.
        let named_types = data
            .iter()
            .flat_map(|(_, data)| {
                data.types.iter().fold(DashMap::new(), |map, comp_ty| {
                    if let Some(ty_name) = &comp_ty.ty.name {
                        map.insert(ty_name.to_owned(), comp_ty.ty.clone());
                    }
                    map
                })
            })
            .collect();

        task.finish();

        log::debug!("Loaded signatures: {:?}", data.keys());

        Self {
            functions,
            types,
            named_types,
        }
    }

    pub fn add_type_to_view<A: BNArchitecture>(&self, view: &BinaryView, arch: &A, ty: &Type) {
        fn inner_add_type_to_view<A: BNArchitecture>(
            matcher: &Matcher,
            view: &BinaryView,
            arch: &A,
            visited_refs: &mut HashSet<String>,
            ty: &Type,
        ) {
            let ty_id_str = TypeGUID::from(ty).to_string();
            if view.get_type_by_id(&ty_id_str).is_some() {
                // Type already added.
                return;
            }
            // Type not already added to the view.
            // Verify all nested types are added before adding type.
            match ty.class.as_ref() {
                TypeClass::Pointer(c) => {
                    inner_add_type_to_view(matcher, view, arch, visited_refs, &c.child_type)
                }
                TypeClass::Array(c) => {
                    inner_add_type_to_view(matcher, view, arch, visited_refs, &c.member_type)
                }
                TypeClass::Structure(c) => {
                    for member in &c.members {
                        inner_add_type_to_view(matcher, view, arch, visited_refs, &member.ty)
                    }
                }
                TypeClass::Enumeration(c) => {
                    inner_add_type_to_view(matcher, view, arch, visited_refs, &c.member_type)
                }
                TypeClass::Union(c) => {
                    for member in &c.members {
                        inner_add_type_to_view(matcher, view, arch, visited_refs, &member.ty)
                    }
                }
                TypeClass::Function(c) => {
                    for out_member in &c.out_members {
                        inner_add_type_to_view(matcher, view, arch, visited_refs, &out_member.ty)
                    }
                    for in_member in &c.in_members {
                        inner_add_type_to_view(matcher, view, arch, visited_refs, &in_member.ty)
                    }
                }
                TypeClass::Referrer(c) => {
                    // Check to see if the referrer has been added to the view.
                    let mut resolved = false;
                    if let Some(ref_guid) = c.guid {
                        // NOTE: We do not need to check for cyclic reference here because
                        // NOTE: GUID references are unable to be referenced by themselves.
                        if view.get_type_by_id(ref_guid.to_string()).is_none() {
                            // Add the referrer to the view if it is in the Matcher types
                            if let Some(ref_ty) = matcher.types.get(&ref_guid) {
                                inner_add_type_to_view(matcher, view, arch, visited_refs, &ref_ty);
                                resolved = true;
                            }
                        }
                    }

                    if let Some(ref_name) = &c.name {
                        // Only try and resolve by name if not already visiting.
                        if !resolved
                            && visited_refs.insert(ref_name.to_string())
                            && view.get_type_by_name(ref_name).is_none()
                        {
                            // Add the ref to the view if it is in the Matcher types
                            if let Some(ref_ty) = matcher.named_types.get(ref_name) {
                                inner_add_type_to_view(matcher, view, arch, visited_refs, &ref_ty);
                            }
                            // No longer visiting type.
                            visited_refs.remove(ref_name);
                        }
                    }

                    // All nested types _should_ be added now, we can add this type.
                    let ty_name = ty.name.to_owned().unwrap_or_else(|| ty_id_str.clone());
                    view.define_auto_type_with_id(ty_name, ty_id_str, &to_bn_type(arch, ty));
                }
                _ => {}
            }
        }
        inner_add_type_to_view(self, view, arch, &mut HashSet::new(), ty)
    }

    pub fn match_function(&self, function: &BNFunction) {
        // Call this the first time you matched on the function.
        let on_new_match = |matched: &Function| {
            // We also want to resolve the types here.
            if let TypeClass::Function(c) = matched.ty.class.as_ref() {
                // Recursively go through the function type and resolve referrers
                let view = function.view();
                let arch = function.arch();
                for out_member in &c.out_members {
                    self.add_type_to_view(&view, &arch, &out_member.ty);
                }
                for in_member in &c.in_members {
                    self.add_type_to_view(&view, &arch, &in_member.ty);
                }
            }
        };

        if let Some(matched_function) = cached_function_match(function, || {
            // We have yet to match on this function.
            // TODO: Expand this check to be less broad.
            let function_delta = function.highest_address() - function.lowest_address();
            let is_function_trivial = { function_delta < TRIVIAL_FUNCTION_DELTA_THRESHOLD };
            let warp_func_guid = try_cached_function_guid(function)?;
            match self.functions.get(&warp_func_guid) {
                Some(matched) if matched.len() == 1 && !is_function_trivial => {
                    on_new_match(&matched[0]);
                    Some(matched[0].to_owned())
                }
                Some(matched) => {
                    let matched_on = self.match_function_from_constraints(function, &matched)?;
                    on_new_match(matched_on);
                    Some(matched_on.to_owned())
                }
                None => None,
            }
        }) {
            on_matched_function(function, &matched_function);
        }
    }

    pub fn match_function_from_constraints<'a>(
        &self,
        function: &BNFunction,
        matched_functions: &'a [Function],
    ) -> Option<&'a Function> {
        // TODO: To prevent invoking adjacent constraint function analysis, we must call call_site constraints specifically.
        let call_sites = cached_call_site_constraints(function);

        // NOTE: We are only matching with call_sites for now, as adjacency requires we run after all analysis has completed.
        if call_sites.is_empty() {
            return None;
        }

        // Check call site guids
        let mut highest_guid_count = 0;
        let mut matched_guid_func = None;
        let call_site_guids = call_sites
            .iter()
            .filter_map(|c| c.guid)
            .collect::<HashSet<_>>();
        for matched in matched_functions {
            let matched_call_site_guids = matched
                .constraints
                .call_sites
                .iter()
                .filter_map(|c| c.guid)
                .collect::<HashSet<_>>();
            let common_guid_count = call_site_guids
                .intersection(&matched_call_site_guids)
                .count();
            match common_guid_count.cmp(&highest_guid_count) {
                Ordering::Equal => {
                    // Multiple matches with same count, don't match on ONE of them.
                    matched_guid_func = None;
                }
                Ordering::Greater => {
                    highest_guid_count = common_guid_count;
                    matched_guid_func = Some(matched);
                }
                Ordering::Less => {}
            }
        }

        // Check call site symbol names
        let mut highest_symbol_count = 0;
        let mut matched_symbol_func = None;
        let call_site_symbol_names = call_sites
            .into_iter()
            .filter_map(|c| Some(c.symbol?.name))
            .collect::<HashSet<_>>();
        for matched in matched_functions {
            let matched_call_site_symbol_names = matched
                .constraints
                .call_sites
                .iter()
                .filter_map(|c| Some(c.symbol.to_owned()?.name))
                .collect::<HashSet<_>>();
            let common_symbol_count = call_site_symbol_names
                .intersection(&matched_call_site_symbol_names)
                .count();
            match common_symbol_count.cmp(&highest_symbol_count) {
                Ordering::Equal => {
                    // Multiple matches with same count, don't match on ONE of them.
                    matched_symbol_func = None;
                }
                Ordering::Greater => {
                    highest_symbol_count = common_symbol_count;
                    matched_symbol_func = Some(matched);
                }
                Ordering::Less => {}
            }
        }

        match highest_guid_count.cmp(&highest_symbol_count) {
            Ordering::Less => matched_symbol_func,
            Ordering::Greater => matched_guid_func,
            Ordering::Equal => {
                // If the two highest our the same we can use it.
                let ty_is_same = matched_guid_func?.ty == matched_symbol_func?.ty;
                let sym_is_same = matched_guid_func?.symbol == matched_symbol_func?.symbol;
                if ty_is_same && sym_is_same {
                    matched_guid_func
                } else {
                    // We matched equally on two different functions
                    None
                }
            }
        }
    }
}

fn get_data_from_dir(dir: &PathBuf) -> HashMap<PathBuf, Data> {
    let data_from_entry = |entry: DirEntry| {
        let path = entry.path();
        let contents = std::fs::read(path).ok()?;
        Data::from_bytes(&contents)
    };

    WalkDir::new(dir)
        .into_iter()
        .filter_map(|e| e.ok())
        .filter(|e| e.file_type().is_file())
        .filter_map(|e| Some((e.clone().into_path(), data_from_entry(e)?)))
        .collect()
}

/// A unique platform ID, used for caching.
#[derive(Copy, Clone, Debug, Hash, PartialEq, Eq, PartialOrd, Ord)]
pub struct PlatformID(u64);

impl From<&Platform> for PlatformID {
    fn from(value: &Platform) -> Self {
        let mut hasher = DefaultHasher::new();
        hasher.write(value.name().to_bytes());
        Self(hasher.finish())
    }
}

impl From<BNRef<Platform>> for PlatformID {
    fn from(value: BNRef<Platform>) -> Self {
        Self::from(value.as_ref())
    }
}

impl From<Guard<'_, Platform>> for PlatformID {
    fn from(value: Guard<'_, Platform>) -> Self {
        Self::from(value.as_ref())
    }
}
