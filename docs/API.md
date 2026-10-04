# Calling Fat Cat metadata

Fat Cat owns native preset selection, project settings and slicer metadata.
Your generator supplies geometry, its real material palette and its process
choices. Neroued owns writing the 3MF. Neither library changes generated geometry.

## Recommended Python entry points

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

The result has one authoritative `project_settings` dictionary. Its
`effective_settings` and `metadata_defaults` describe that final result.
`process_source` and `material_source`, when present, identify actual native or
recorded compatibility sources. See [COMPATIBILITY.md](COMPATIBILITY.md).

For a user-provided project, call
`compose_project_settings(source_project, request)`. This request omits
`project_source`; it supplies the target and its source-preservation or material
selection policy. Existing unknown project fields are retained. `None` as the
source is equivalent to the one-argument native-source call.

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

The [complete independent generator](../cpp/examples/python_neroued_consumer/)
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

`assemble_project_template` and `assemble_machine_registry_template` remain
published historical interfaces. New native-source generators use the two
composers above. These compatibility factories are retained for existing callers.

Lumina placement warnings and merged-slot annotations are optional product
extensions. For example, `output_options.emit_lumina_merged_slots_json` is
opt-in and requires merged objects. Independent generators need no Lumina code.

## C++ consumption

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
