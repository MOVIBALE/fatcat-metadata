from typing import Any, NotRequired, Required, TypedDict, overload

_Object = dict[str, Any]


class _SourceMaterial(TypedDict, total=False):
    name: str
    colour: str
    material_type: str | None


class _MergeSlot(TypedDict):
    source_slot_id: int
    source_slot_index: NotRequired[int]
    slot_name: str
    material_id: str
    preview_color: str


class _MergeSource(TypedDict):
    source_id: str
    slots: list[_MergeSlot]
    # The first source uses the project's positional argument instead.
    project_settings: NotRequired[_Object]


class _ProjectRequest(TypedDict, total=False):
    slicer_id: Required[str]
    application_version: Required[str]
    project_source: str
    machine_uid: str
    nozzle_uid: str
    build_plate_uid: str
    source_materials: list[_SourceMaterial]
    native_print_profile_name: str
    native_filament_profile_names: list[str]
    material_uid: str
    material_uids: list[str]
    material_mode: str
    hardware_mode: str
    process_settings: _Object
    merge_sources: list[_MergeSource]
    merge_default_project: _Object
    filament_slot_mode: str
    default_filament_source_slot: int
    filament_source_slots: list[int | None]
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


class _ModelPart(TypedDict):
    part_id: int
    name: str
    material_index: int
    source_object_id: int
    source_volume_id: int
    matrix: str
    source_offset_x: str
    source_offset_y: str
    source_offset_z: str


class _MergedPart(TypedDict):
    part_id: int
    source_part_id: int


class _MergedModel(TypedDict):
    model_index: int
    assembly_id: int
    source_assembly_id: int
    instance_id: str
    identify_id: str
    source_model_settings_xml: str
    face_count: int
    parts: list[_MergedPart]
    source_slot_output_indexes: list[int]
    source_root_metadata: dict[str, str]
    transform: list[float]
    offset_mm: list[float]
    source_model_index: NotRequired[int]
    default_slot_output_index: NotRequired[int]
    source_layer_config_ranges_xml: NotRequired[str]
    source_slots: NotRequired[list[_Object]]


class _Plate(TypedDict):
    plater_id: str
    plater_name: str
    locked: bool
    bed_type: NotRequired[str]
    filament_map_mode: NotRequired[str]


class _OutputOptions(TypedDict, total=False):
    emit_lumina_merged_slots_json: bool


class _ComponentInputs(TypedDict, total=False):
    plate_summary: _Object
    wipe_tower_placement: _Object
    layer_config_ranges: _Object | None


class _ModelRequest(TypedDict, total=False):
    slicer_id: Required[str]
    application_version: Required[str]
    plate: Required[_Plate]
    component_inputs: Required[_ComponentInputs]
    resource_roles: list[str]
    output_options: _OutputOptions
    slice_uuid: str
    source_slice_info_xml: str
    # Supply these fields for a single model, or objects for merged models.
    assembly_id: int
    instance_id: str
    identify_id: str
    source_file: str
    parts: list[_ModelPart]
    active_material_count: int
    objects: list[_MergedModel]


class _MetadataPart(TypedDict):
    role: str
    path: str
    media_type: str
    content: NotRequired[str]
    resource_role: NotRequired[str]


class _Relationship(TypedDict):
    source: str
    id: str
    type: str
    target: str


class _MetadataDescription(TypedDict):
    parts: list[_MetadataPart]
    relationships: list[_Relationship]
    content_types: list[_Object]
    root_model: _Object
    settings_parts: dict[str, str]
    project_bed_type: str
    model_settings_bed_type: str
    build_items: NotRequired[list[_Object]]


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
def compose_model_metadata(project: _Object, request: _ModelRequest | _Object) -> _MetadataDescription: ...
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
