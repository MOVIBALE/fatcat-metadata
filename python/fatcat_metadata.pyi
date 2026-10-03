from typing import Any, NotRequired, Required, TypedDict, overload

_Object = dict[str, Any]

class _ProjectRequest(TypedDict, total=False):
    slicer_id: Required[str]
    application_version: Required[str]
    project_source: str
    machine_uid: str
    nozzle_uid: str
    build_plate_uid: str
    source_materials: list[_Object]
    native_print_profile_name: str
    native_filament_profile_names: list[str]
    material_uid: str
    material_uids: list[str]
    material_mode: str
    hardware_mode: str
    process_settings: _Object
    merge_sources: list[_Object]
    merge_default_project: _Object
    filament_slot_mode: str
    default_filament_source_slot: int
    filament_source_slots: list[int | _Object]
    preserve_source_material_settings: bool
    filament_colour: list[str]
    layer_height: str | float
    sparse_infill_density: str | float

class _ProjectComposition(TypedDict):
    project_settings: _Object
    effective_settings: _Object
    metadata_defaults: _Object
    process_source: NotRequired[_Object]
    material_source: NotRequired[_Object]

__version__: str
__fatcat_cpp_extension__: bool
__fatcat_project_settings__: bool

@overload
def compose_project_settings(request: _ProjectRequest | _Object) -> _ProjectComposition: ...
@overload
def compose_project_settings(project: _Object | None, request: _ProjectRequest | _Object) -> _ProjectComposition: ...
@overload
def compose_project_settings(request_json: str) -> str: ...
@overload
def compose_project_settings(project_json: str | None, request_json: str) -> str: ...
@overload
def compose_model_metadata(project: _Object, request: _Object) -> _Object: ...
@overload
def compose_model_metadata(project_json: str, request_json: str) -> str: ...

# Read-only data and advanced source/template operations retain JSON contracts.
def metadata_target(slicer_id: str) -> str: ...
def metadata_resource_specs(slicer_id: str) -> str: ...
def native_project_source_catalog(slicer_id: str) -> str: ...
def patch_wipe_tower(project_json: str, settings_json: str, dialect_json: str) -> str: ...
def source_material_settings_parts(request_json: str) -> str: ...
def extract_source_material_slots(project_json: str, model_settings_xml: str, request_json: str) -> str: ...
def read_project_layout(project_json: str, request_json: str) -> str: ...
def read_placement_warnings(placement_json: str | None) -> str: ...
def read_source_metadata(project_json: str, model_settings_xml: str, request_json: str,
                         source_model_xml: str | None = None, slice_info_xml: str | None = None,
                         placement_json: str | None = None) -> str: ...
def serialize_layer_config_ranges(data_json: str, fine_layer_height_mm: float) -> str: ...
def read_model_object_metadata(model_xml: str) -> str: ...
def validate_template_hardware(template_json: str, expected_project_json: str,
                               source_slicer: str, selected_slicer: str) -> None: ...
def template_import_parts(request_json: str) -> str: ...
def resolve_template_build_plate(request_json: str, default_build_plate_uid: str,
                                 model_plate_value: str | None, sidecar_bed_value: str | None,
                                 project_bed_value: str | None) -> str: ...
def detect_template_source(source_model_xml: str | None, slice_info_xml: str | None) -> str: ...
def import_template_metadata(project_settings_json: str, source_model_xml: str | None,
                              slice_info_xml: str | None, model_settings_xml: str | None,
                              plate_sidecar_json: str | None, request_json: str,
                              explicit_slicer_hint: bool) -> str: ...

# Published historical factories remain available for existing consumers.
def assemble_project_template(base_project_json: str, registry_machine_json: str | None,
                               options_json: str) -> str: ...
def assemble_machine_registry_template(source_settings_json: str, options_json: str) -> str: ...
