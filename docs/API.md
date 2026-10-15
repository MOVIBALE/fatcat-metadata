# Calling Fat Cat metadata

Fat Cat owns native preset selection, project settings and slicer metadata.
Your generator supplies geometry, its real material palette and its process
choices. Neroued owns writing the 3MF. Neither library changes generated geometry.

## Recommended Python entry points

Discover choices before composing; queries return dictionaries and use the same
native source index and compatibility rules as composition:

```python
import fatcat_metadata as fatcat

targets = fatcat.list_targets()["targets"]
machines = fatcat.list_machines("BambuStudio", "02.08.02.61")["sources"]
choices = fatcat.list_project_options({
    "slicer_id": "BambuStudio", "application_version": "02.08.02.61",
    "machine_uid": "bambu-lab:a1-mini", "nozzle_uid": "nozzle:0.4mm",
    "build_plate_uid": "plate:textured-pei",
})
materials = [item for item in choices["filament_profiles"] if item["available"]]
```

`list_targets` returns exact installed software/version identities.
`list_machines` accepts an exact version or the unique installed version by
omission. `list_project_options` requires software/version/machine/nozzle; a
plate is optional. It returns `build_plates`, `print_profiles`,
`filament_profiles`, ordered default names, source availability and optional
`compatibility_source`. Choices contain no raw native profile paths. Material
names are suitable for `native_filament_profile_names`; their types describe the
actual inherited preset. Unavailable choices carry `unavailable_reason`.
Availability is a candidate-level source/selection check, not GUI or printer
validation. Final composition retains its full validation. See
[first use](FIRST_USE.md) for the installed command and complete example.

Use dictionaries with the two composers. They call the same C++ implementation
as the published JSON-string API. The wheel installs
`fatcat_metadata-stubs/__init__.pyi` for editor signatures and type checking.
This is a complete stub-only package; it needs no `py.typed` marker and does
not change the runtime extension's import name or location. See the
[mypy packaging guide](https://mypy.readthedocs.io/en/stable/installed_packages.html#creating-pep-561-compatible-packages).
The signatures describe nested palette entries, merged-source slot mappings,
model parts, plate fields and the returned metadata parts/relationships.
The private `TypedDict` names describe dictionary shapes for editors; they are
not runtime classes to instantiate or import. Ordinary dictionaries, including
advanced JSON fields, remain supported.

```python
import fatcat_metadata

selection = {
    "project_source": "fatcat_native",
    "slicer_id": "BambuStudio",
    "application_version": "02.08.02.61",
    "machine_uid": "bambu-lab:a1-mini",
    "nozzle_uid": "nozzle:0.4mm",
    "build_plate_uid": "plate:textured-pei",
    "source_materials": [
        {"name": "PLA", "material_type": "PLA", "colour": "#123456"}
    ],
}
composed = fatcat_metadata.compose_project_settings(selection)
project = composed["project_settings"]
```

`slicer_id` and `application_version` select an exact supported target. Native
composition requires the machine/nozzle identity and the real ordered palette.
An omitted plate uses the native source default. Explicit process/material
preset names can be selected with `native_print_profile_name` and
`native_filament_profile_names`. Unknown or incompatible selections raise
`ValueError`; they do not select a similar printer or invent material settings.

### Material selection before composition

In 0.1.1, `resolve_project_materials(selection)` returns `ready`, the ordered
`materials`, shared-compatible `build_plates` and actionable `issues`. It uses
the same resolver as composition; it neither writes a 3MF nor changes process
values. A profile's `available` means it matches the machine and material type;
`supported_build_plate_uids` describes its plate compatibility. Each returned
plate's `compatible` covers the entire requested palette.

```python
plan = fatcat_metadata.resolve_project_materials(selection)
if not plan["ready"]:
    # Present the candidates to the caller; do not silently pick another brand.
    print(plan["issues"], plan["materials"], plan["build_plates"])
    selection["source_materials"][0]["native_filament_profile_name"] = chosen_name
    selection["build_plate_uid"] = chosen_plate_uid
    plan = fatcat_metadata.resolve_project_materials(selection)
if plan["ready"]:
    composed = fatcat_metadata.compose_project_settings(selection)
```

`native_filament_profile_name` selects an exact preset for one source material.
The existing whole-palette `native_filament_profile_names` and material UID
interfaces remain supported; do not combine the selection forms. Missing source
types use the explicitly selected preset's type before the machine default.
Explicitly provided or inferred types remain validated. Composition of an
unresolved request raises `ProjectValidationError`, a `ValueError` subclass;
its `detail` contains the same selection plan. The issue codes distinguish
ambiguity, plate incompatibility, unavailable material and invalid selection.
Other invalid inputs still raise `ValueError`.

The C++ SDK exposes `resolve_project_materials(request_json, data_root)` and the
same diagnostic JSON through `ProjectSettingsError::diagnostic_json()`. The
installed CLI exposes `fatcat resolve-materials --request REQUEST.json`.

0.1.1 的材料预检与最终组装共用规则。界面可根据 `issues` 显示原生预设候选与整组
材料共同兼容的板型；用户明确选择后再次预检，再用原来的工艺参数组装。库不会静默
换品牌、换材料或换板型。旧版整组预设名和 UID 接口保持兼容。

The result has one authoritative `project_settings` dictionary. Its
`effective_settings` and `metadata_defaults` describe that final result.
`process_source` and `material_source`, when present, identify actual native or
recorded compatibility sources. See [COMPATIBILITY.md](COMPATIBILITY.md).

PrusaSlicer 3 uses this same entry point and metadata description, with native
typed JSON as `project_settings`. Its flattened `effective_settings` is a
geometry/layout summary; it is not a project to serialize. Per-slot
`source_materials[i].native_settings` applies reviewed native filament values.
Native preset names and IDs are retained; a generator's material `name` labels
its writer-owned base material rather than renaming a Prusa preset.
See [PRUSA.md](PRUSA.md) for the core geometry layout, native capacity policy,
source-preserving merge inputs, layer ranges and remaining limits.

Explicit process overrides share one target-specific contract across native and
user-project composition. Query `list_project_options(request)` or `fatcat choices`
for `process_settings_contract`; see [PROCESS_SETTINGS.md](PROCESS_SETTINGS.md)
for aliases, accepted values, scalar/array serialization and error behavior.
Unknown nested process keys now raise errors. Existing unknown source-project
fields remain preserved.
`list_native_fields(slicer_id, application_version="")` queries versioned native
evidence, process coverage and unknowns without requiring a machine. This returns
a dictionary; the CLI `fatcat fields` and C++ `native_field_inventory` return the
same JSON contract. See [PROCESS_SETTINGS.md](PROCESS_SETTINGS.md) for status
definitions and the distinction between native field evidence and SDK support.

For a user-provided project, call
`compose_project_settings(source_project, request)`. This request omits
`project_source`; it supplies the target and its source-preservation or material
selection policy. Existing unknown project fields are retained. `None` as the
source is equivalent to the one-argument native-source call.

Set `project_preset_name` to a non-empty string to give the exported process
preset its own project identity. This works through the same composer for
built-in sources, imported projects and merges. For Bambu-compatible projects,
it sets `print_settings_id` while retaining the original `default_print_profile`;
only the process entry in `inherits_group` is detached, leaving material and
printer inheritance intact. The project already contains resolved process values;
the slicer can then load a separate project preset instead of editing a system
preset with the same name. For Prusa 3, it changes the native print preset's
display name and retains its source ID and conditions. All process values,
material/hardware identities and unknown source settings are retained. Omit the
field to retain the source's existing preset name.

`project_preset_name` 为非空字符串时，FatCat 为导出的工艺预设赋予独立项目名称。
内置来源、导入工程和合并共用同一合成入口。Bambu 兼容工程修改 print_settings_id，
保留原生 default_print_profile，仅解除工艺继承，保留材料／机器继承和已展开的工艺值；
Prusa 3 修改显示名称，保留来源 ID 和适用条件。
工艺参数、材料／硬件身份及未知来源设置不变；省略该字段则保留原有预设名称。

Bambu projects also receive any missing native flush-volume vector, matrix and
nozzle multipliers. FatCat expands the recorded slicer defaults to the actual
material and physical-nozzle counts, avoiding dependence on the slicer's previous
project. Existing nozzle multipliers retain their prefix; missing nozzle entries
receive native defaults. Mixed old/new source merges prefer explicit transition
values and use defaults only when no source supplies a value. Complete source
matrices are retained; this does not calculate purging
volumes from colours or validate physical print results.

Bambu 工程缺少原生冲刷向量、矩阵或喷嘴倍率时，FatCat 按实际材料数和物理喷嘴数
展开已记录的切片软件默认值，避免受切片软件上一个工程的状态影响。已有喷嘴倍率保留，
仅补缺少的喷嘴项；新旧工程合并优先采用明确的冲刷值，仅在没有来源值时使用默认。
已有完整矩阵保留；
这里不按颜色预测冲刷量，也不代表实体打印已验证。

Pass the final project and real model facts to
`compose_model_metadata(project, model_request)`. The model request includes
the exact target, real object/assembly IDs, ordered parts/material indices,
plate and `component_inputs`. The result describes `root_model`, `parts`,
`content_types` and `relationships` for the writer. Binary resources such as
thumbnails remain caller-owned bytes.

For a single model, the caller supplies `assembly_id`, `instance_id`,
`identify_id`, `source_file` and `parts`. Each part describes its real writer
`part_id`, zero-based `material_index`, name, source IDs, matrix and offsets.
`plate` supplies `plater_id`, `plater_name` and `locked`; `component_inputs`
can be empty. For merged models, supply `objects` instead of the single-model
fields; each object's `model_index` follows the final one-based build order,
with source XML, part/slot mappings and actual placement.

The [minimal example](../cpp/examples/python_neroued_consumer/minimal.py)
shows the three integration steps without the complete CLI's options. The
[complete independent generator](../cpp/examples/python_neroued_consumer/)
shows both requests and registers the description with
`fatcat_metadata_neroued`, then writes the builder once.

## Existing JSON and advanced interfaces

The same compositor names still accept JSON strings and return JSON strings.
Their project result retains its existing `project_settings_json` field.
Choose one style for a call; mixed dictionary/string arguments are unsupported.
The dictionary project result uses `project_settings` instead, with no duplicate
serialized field. JSON-compatible values are required; non-finite numbers are
rejected by the dictionary serializer.

Read-only `metadata_target`, `native_project_source_catalog` and
`metadata_resource_specs` return packaged target/catalogue JSON. Source reading,
template import, slot extraction, layout reading and `patch_wipe_tower` are
advanced JSON/XML operations; their existing signatures are listed in the
installed stub and public headers.

For build provenance, read `__version__`, `__source_revision__` and
`__source_dirty__` from the installed extension. The revision records the build
checkout and the flag is `True` for local changes, `False` for clean Git sources,
or `None` when unavailable. Git-free source archives report revision `"unknown"`
unless their packaging workflow supplies build provenance as described in
[DISTRIBUTIONS.md](DISTRIBUTIONS.md).
These identify the library build; `metadata_target(slicer_id)` and the returned
`process_source`/`material_source` identify the selected configuration sources.

`assemble_project_template` and `assemble_machine_registry_template` remain
published historical interfaces. New native-source generators use the two
composers above. These compatibility factories are retained for existing callers.

Lumina placement warnings and merged-slot annotations are optional product
extensions. For example, `output_options.emit_lumina_merged_slots_json` is
opt-in and requires merged objects. Independent generators need no Lumina code.

## C++ consumption

The CLI and C++ consumers share `metadata_target_catalog(data_root)` and
`native_project_options(request_json, data_root)` in
`fatcat/native_project_source.h`. The existing machine catalogue and both
composers remain unchanged. CLI examples and stdout/exit-code contracts are
documented in [FIRST_USE.md](FIRST_USE.md).

Public headers preserve their existing JSON-string contracts. Use
`compose_builtin_project_settings`, `compose_project_settings_from_data` and
`compose_model_metadata_from_data` with one public data root; Fat Cat resolves
the native and target file layout internally.

For an installed SDK:

```cmake
find_package(FatCatMetadata 0.1.0 CONFIG REQUIRED)
target_link_libraries(my_generator PRIVATE FatCatMetadata::Core)
# FatCatMetadata_DATA_DIR is the installed public data root.
```

Build/install commands and a consumer that supports both installed and source
use are in [cpp/README.md](../cpp/README.md). Python wheels contain the extension,
adapter, stub and data; the standalone C++ SDK is a separate opt-in installation.

## Native tower wall styles

The tower dialect may declare `wall_type` with a native `key`, logical-to-native
`values` (`rib` and/or `rectangle`), and an optional `source_url`. The layout
reader exposes these supported logical styles as `tower_wall_types`.
`patch_wipe_tower` accepts optional `wall_type` only when the dialect declares
that style. Changing the style preserves tower enablement and purge volumes;
the existing process-difference index records the native field change.

塔方言可声明 wall_type，包含原生字段 key、rib／rectangle 到原生值的 values
映射及可选 source_url。布局读取器通过 tower_wall_types 返回支持的形状；
patch_wipe_tower 仅接受目标已声明的 wall_type。形状选择保留擦料塔启用与
冲刷量，原有工艺差异索引记录对应原生字段的变化。
