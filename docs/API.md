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
