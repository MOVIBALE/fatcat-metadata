# Fat Cat metadata

Fat Cat metadata is a C++17 library with a Python extension for composing
slicer-specific 3MF metadata and project settings. The checked-in target data
supports the exact application versions listed in [NOTICE.md](NOTICE.md).
Requests for an unknown slicer or application version fail instead of silently
choosing another target.

The Python package includes the C++ extension, target translation data, native
project profile snapshots, and a small optional adapter for the Neroued 3MF
writer. The core package does not require Neroued.

OrcaSlicer 2.4.2 U1 0.4 mm composition uses that release's native machine with
the recorded Orca 2.2.4 process. For Textured PEI (including the source default),
the recorded `Snapmaker PLA Basic @U1` material retains its actual 65 °C plate
and 220 °C nozzle settings. Returned `process_source` and `material_source`
identify their respective versions. The installed 2.4.2 catalogue includes
machine-compatible `Snapmaker PLA @U1`, but its native textured plate temperature
is zero; it is usable only on plates its actual profile supports. The declared
`Snapmaker PLA` default supports A250/A350. Neither profile is rewritten.
Material selection checks the exact machine, requested type and effective plate;
explicit incompatible selections are rejected. Complete native catalogues also
supply the exact SnapmakerOrca nondefault nozzle and FlashStudio 0.25 mm materials.
OrcaSlicer 2.4.2 U1 0.6 mm still has no matching PLA source.

## Install and use

Python 3.12–3.14 and a C++17 toolchain are required. From a source checkout:

```bash
python -m pip install .
```

The public extension module is `fatcat_metadata`. Its main entry points are
`compose_project_settings`, `compose_model_metadata`, and `patch_wipe_tower`.
See [`cpp/tests/test_binding.py`](cpp/tests/test_binding.py) for small calls and
[`cpp/examples/python_neroued_consumer/`](cpp/examples/python_neroued_consumer/)
for a complete writer example.

## Build and verify

Build and run the standalone C++ tests:

```bash
cmake -S cpp -B build -DFATCAT_BUILD_PYTHON=OFF -DFATCAT_BUILD_TESTS=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

Build the installed Python extension and run its smoke tests:

```bash
python -m pip install .
python -m unittest discover -s cpp/tests -p 'test_*.py' -v
```

The out-of-tree C++ consumer in `cpp/examples/out_of_tree_consumer` shows how
to add this project with `add_subdirectory`. The optional Python example writes
a 3MF with Neroued 0.4.0; install that package separately before running it:

```bash
python -m pip install 'neroued-3mf==0.4.0'
python cpp/examples/python_neroued_consumer/write_example.py \
  --output /tmp/fatcat-consumer-example.3mf
```

## Source data and licensing

The profile snapshots and supported-version list are described in
[NOTICE.md](NOTICE.md). Original Fat Cat code is licensed under
AGPL-3.0-only; see [LICENSE](LICENSE) and [COPYRIGHT.md](COPYRIGHT.md).
Third-party profile data and optional dependencies remain under their own
licenses.
