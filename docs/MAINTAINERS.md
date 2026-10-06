# Maintaining native slicer sources

## Composition owners

`cpp/src/project_settings.cpp` validates selection and coordinates native
materials, slot projection and final summaries. Its private merge implementation
in `project_settings_merge.cpp` prepares all sources once and owns merged slot
arrays, transitions, differences and mappings. Both preserved-source and bound
hardware paths call that same implementation. `project_settings_overrides.cpp`
owns explicit scalar, colour and wipe-tower overrides. Shared project field
contracts remain private in `project_settings_internal.h`; none of these files
introduces an installed public API or a generator dependency.

The native source updater is `scripts/sync_native_project_sources.py`. It reads
the profile files installed by each supported macOS slicer release and checks
the exact `CFBundleShortVersionString` in its app bundle before it writes any
profile snapshot. Use `--dry-run` with the same application roots to validate and preview the
update without changing repository files. A normal successful run replaces the
generated source index and profile files; use a clean working tree and preserve
local changes before applying it. These refresh tools require macOS app bundles;
consuming the installed SDK works on Linux, macOS and Windows.

## Refresh the pinned application snapshots

The version, target filename and template-import order are maintained once in
[`supported-targets.json`](../compatibility/current-src/translations/supported-targets.json).
CMake generates the internal C++ registry from that manifest; the Python
extension and source updater use the same entries. When changing a supported
release, update its manifest entry and matching target contract together, then
refresh and review the native source data. The updater rejects a contract that
disagrees with the manifest. Native profile JSON remains unchanged upstream
source data, rather than being flattened into this registry.

Install these exact application releases under `/Applications` (or provide
their actual bundle paths) before running the updater:

| `slicer_id` | Required version |
| --- | --- |
| `BambuStudio` | `02.08.02.61` |
| `OrcaSlicer` | `2.4.2` |
| `QIDIStudio` | `02.07.02.60` |
| `ElegooSlicer` | `1.5.3.5` |
| `AnycubicSlicerNext` | `2.0.0.3` |
| `FlashStudio` | `1.7.18` |
| `SnapmakerOrca` | `2.4.0` |

Pass one app bundle for every `slicer_id`. The path must be the `.app` bundle
root, which contains `Contents/Info.plist` and
`Contents/Resources/profiles/`:

```bash
python3 scripts/sync_native_project_sources.py \
  --application-root 'BambuStudio=/Applications/BambuStudio.app' \
  --application-root 'OrcaSlicer=/Applications/OrcaSlicer.app' \
  --application-root 'QIDIStudio=/Applications/QIDIStudio.app' \
  --application-root 'ElegooSlicer=/Applications/ElegooSlicer.app' \
  --application-root 'AnycubicSlicerNext=/Applications/AnycubicSlicerNext.app' \
  --application-root 'FlashStudio=/Applications/Flash Studio.app' \
  --application-root 'SnapmakerOrca=/Applications/Snapmaker Orca.app'
```

The updater verifies each exact application version, copies machine/process/
material profile files and their exact inheritance/include closure, and writes
`compatibility/current-src/native-project-sources/source-index.json`. A missing
exact profile is recorded as unavailable. Do not create a substitute default
or rename a different preset to satisfy an identity. A machine's declared default
can contradict a material's explicit compatible_printers list. The updater keeps
that declaration and records its incompatibility, then discovers instantiated
materials in the same native catalogue whose inherited compatibility names the
exact machine. It copies their real inheritance/include closure unchanged.

The generated index keeps native source identities, ordered material defaults,
preset options and exact inheritance links. Display names and supported plate
policy remain in `native-source-map.json` and the generated canonical/target
bindings; do not duplicate them in source rows. A single material default uses
the ordered `default_filament_profile_names` array too. Runtime profile reads
are reused only within one composition/catalog call, so later calls reload
updated files and inheritance cycle/depth checks remain active.

OrcaSlicer 2.4.2 U1 0.4 mm has no native process. Its recorded complete Orca
2.2.4 project supplies the process. For Textured PEI, the native 2.4.2
`Snapmaker PLA @U1` profile has a zero plate temperature and cannot be selected;
the recorded 2.2.4 `Snapmaker PLA Basic @U1` supplies its real 65/220 °C settings.
The same plate resolution is used for explicit and omitted plate selections,
and retained materials are checked too. The result identifies each actual
`process_source` and `material_source`; historical settings must never be
relabeled as 2.4.2 native presets. Catalogue availability does not guarantee
that every material type or plate is supported. OrcaSlicer U1 0.6 mm has no PLA
candidate; SnapmakerOrca 2.4.0 has genuine Generic PLA sources for 0.2/0.6/0.8 mm.

The updater preserves the retained file and its source descriptor across
refreshes. Check its recorded hardware, process provenance, content hash, and
license before copying it. Remove the compatibility descriptor only when an
exact native process and compatible materials become available and the
replacement has been reviewed.

Review the generated source index, profile files, and
`unavailable-sources.json` before continuing. In particular, confirm the source
row still has the expected application version, machine profile, default print
profile, material choices, plate support, and accurate availability flags.

## Reconcile canonical and target bindings

`compatibility/current-src/native-source-map.json` defines the selected
machine/nozzle identities and source preset names. Update it only when the
upstream application actually changes its identities or supported plate set;
do not derive a new machine from a similar printer profile. After reviewing the
source index, run:

```bash
python3 scripts/sync_native_machine_bindings.py --dry-run
python3 scripts/sync_native_machine_bindings.py
```

This deterministically updates `translations/canonical.json`, the seven target
JSON files, and `native-project-sources/unavailable-sources.json`. Review those
diffs together. Hardware volume, nozzle arrays, target default process, plate
bindings, and unavailable source reasons must all agree with the pinned native
profiles. Preserve explicit unavailable combinations instead of inventing a
fallback.

## Refresh explicit process contracts

`scripts/sync_process_contract.py` extracts only the SDK's declared process
subset from reviewed `PrintConfig.cpp` common/FFF definitions. It excludes SLA,
uses each field's enum list (including explicit reused lists), and keeps native
scalar/array serialization. It does not infer feature equivalents. Inspect
preprocessor conditions manually: this limited importer is not a C++ compiler.

```bash
python3 scripts/sync_process_contract.py --slicer OrcaSlicer \
  --print-config /path/to/exact-release/PrintConfig.cpp \
  --definition-version 2.4.2 \
  --source-url https://github.com/OrcaSlicer/OrcaSlicer/blob/v2.4.2/src/libslic3r/PrintConfig.cpp \
  --dry-run
```

Remove `--dry-run` after reviewing the definitions. When the exact public source
is unavailable, supply the recorded baseline version, an exact native
`--export-settings` output via `--native-defaults`, and its `--application-root`.
The app bundle version must match the target. Both definition and export hashes
are recorded; `exact_application_definitions` remains false for an older source.
The export confirms field presence, defaults and serialization, not all possible
enum values. Review those in the native GUI and retain this limitation in docs.
Do not relabel older definitions as current ones. Native `renamed_from` records
resolve preset renames; ambiguous/missing identities remain unavailable.

## Verify and record the update

Run the commands from the repository root:

```bash
python3 -m py_compile scripts/sync_native_project_sources.py \
  scripts/sync_native_machine_bindings.py
cmake -S cpp -B /tmp/fatcat-cpp-check \
  -DFATCAT_BUILD_PYTHON=OFF -DFATCAT_BUILD_TESTS=ON
cmake --build /tmp/fatcat-cpp-check --parallel
ctest --test-dir /tmp/fatcat-cpp-check --output-on-failure
python3 -m pip install --target /tmp/fatcat-python-check .
PYTHONPATH=/tmp/fatcat-python-check python3 -m unittest discover \
  -s cpp/tests -p 'test_*.py' -v
```

The regular C++ suite registers only the locale-independent checks and the C
locale case. The comma-decimal case is opt-in because `de_DE.UTF-8` must exist
on the test host. On Ubuntu, enable it only after generating that locale:

```bash
sudo apt-get update
sudo apt-get install -y locales
sudo locale-gen de_DE.UTF-8
cmake -S cpp -B /tmp/fatcat-cpp-locale-check \
  -DFATCAT_BUILD_PYTHON=OFF -DFATCAT_BUILD_TESTS=ON \
  -DFATCAT_TEST_NON_C_LOCALE=ON
cmake --build /tmp/fatcat-cpp-locale-check --parallel
ctest --test-dir /tmp/fatcat-cpp-locale-check --output-on-failure \
  --no-tests=error -R '^fatcat_locale_test_non_c$'
```

The Python test suite exercises every packaged target using real machine,
process, plate, and material source paths. Record the installed slicer version,
the app bundle's source commit or vendor release, changed identities, and any
unavailable exact profiles in the change description. Review third-party
license source revisions in `licenses/third-party/manifest.json` whenever an
upstream profile source changes.

## Public data root and sparse merged selections

C++ callers pass a public packaged data root to
`compose_builtin_project_settings`, `compose_project_settings_from_data`, and
`compose_model_metadata_from_data`. Fat Cat selects the exact target and
canonical data internally. Existing explicit JSON entry points remain available.
The out-of-tree example builds both a native project and the U1 compatibility
case without reading a target filename or a prebuilt project.

For a merged selection, `source_materials` can describe the ordered complete
palette while `merge_sources[].slots` describes only real source rows. Missing
rows are synthesized using the same native-source path and exact machine/nozzle
UIDs. Actual source temperatures, flow, process, unknown fields, and transition
values remain authoritative. Transition pairs absent from every source require
recorded native defaults; unavailable defaults are errors. The material carrier
retains the source's hardware values and missing-field state, so an omitted
source plate or bed geometry is not replaced by a material-default field.
The explicit JSON
composer accepts `merge_default_project` for these absent transition pairs and
checks its hardware identity. Neither entry infers a substitute machine.

`assemble_project_template` and `assemble_machine_registry_template` retain
their published C++/Python signatures for explicit historical callers. Their
minimal/registry factories are compatibility behavior and are not used by the
current native-source export path. Template assembly no longer selects policy
from a printer brand or display name. Lumina's unused wrappers were removed;
current consumers call the project/settings and metadata composers directly.
