# Maintaining native slicer sources

The native source updater is `scripts/sync_native_project_sources.py`. It reads
the profile files installed by each supported macOS slicer release and checks
the exact `CFBundleShortVersionString` in its app bundle before it writes any
profile snapshot. Run it only from a clean working tree after backing up or
committing changes: there is no dry-run option, and a successful run replaces
the generated source index and profile files.

## Refresh the pinned application snapshots

Install these exact application releases under `/Applications` (or provide
their actual bundle paths) before running the updater:

| `slicer_id` | Required version |
| --- | --- |
| `BambuStudio` | `02.08.02.61` |
| `OrcaSlicer` | `2.4.2` |
| `QIDIStudio` | `02.07.02.60` |
| `ElegooSlicer` | `1.5.3.5` |
| `AnycubicSlicerNext` | `2.0.0.2` |
| `FlashStudio` | `1.7.15` |
| `SnapmakerOrca` | `2.3.6` |

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
or rename a different preset to satisfy an identity. For example, OrcaSlicer
2.4.2 has a Snapmaker U1 0.4 mm machine and material source, but its process
preset is not uniquely available. Its source row records one retained complete
Orca 2.2.4 project in `compatibility-projects/`. The generic built-in composer
uses that historical process with the selected 2.4.2 machine and materials.
The native process availability stays unavailable, and the result reports the
historical `process_source`; it must never be relabeled a 2.4.2 native process.
Snapmaker Orca 2.3.6 has its own separate U1 0.4 mm source.

The updater preserves the retained file and its source descriptor across
refreshes. Check its recorded hardware, process provenance, content hash, and
license before copying it. Remove the compatibility descriptor only when an
exact native process becomes available and the replacement has been reviewed.

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
python3 scripts/sync_native_machine_bindings.py
```

This deterministically updates `translations/canonical.json`, the seven target
JSON files, and `native-project-sources/unavailable-sources.json`. Review those
diffs together. Hardware volume, nozzle arrays, target default process, plate
bindings, and unavailable source reasons must all agree with the pinned native
profiles. Preserve explicit unavailable combinations instead of inventing a
fallback.

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
recorded native defaults; unavailable defaults are errors. The explicit JSON
composer accepts `merge_default_project` for these absent transition pairs and
checks its hardware identity. Neither entry infers a substitute machine.

`assemble_project_template` and `assemble_machine_registry_template` retain
their published C++/Python signatures for explicit historical callers. Their
minimal/registry factories are compatibility behavior and are not used by the
current native-source export path. Template assembly no longer selects policy
from a printer brand or display name. Lumina's unused wrappers were removed;
current consumers call the project/settings and metadata composers directly.
