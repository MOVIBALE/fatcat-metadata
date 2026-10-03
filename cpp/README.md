# C++ library

The CMake project builds the `fatcat_metadata_core` C++17 library and, by
default, a pybind11 extension module. The public headers are under `include/`.
Slicer translation contracts and native source profiles are in the sibling
`compatibility/` directory and are packaged with the Python wheel.

## Build the C++ library and tests

From the repository root:

```bash
cmake -S cpp -B build -DFATCAT_BUILD_PYTHON=OFF -DFATCAT_BUILD_TESTS=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

The CMake project fetches pinned nlohmann/json and TinyXML-2 releases only when
matching system packages are unavailable. Their licenses are listed in the
repository-level [NOTICE.md](../NOTICE.md).

## Python extension

The root `pyproject.toml` builds and packages the extension with
scikit-build-core. Use `python -m pip install .` from the repository root. The
extension is imported as `fatcat_metadata`; Python-facing examples and tests
are in the root README and `tests/`.

## Out-of-tree consumer

`examples/out_of_tree_consumer` demonstrates consuming the core as a CMake
subdirectory. It builds against the public headers and packaged compatibility
data without relying on internal build targets. It receives one public data
root; the library resolves canonical/target files and native sources. It builds
a Bambu A1 project and the Orca 2.4.2 U1 project whose retained process is from
Orca 2.2.4. No prebuilt project fixture is needed.

```bash
cmake -S cpp/examples/out_of_tree_consumer -B build-consumer
cmake --build build-consumer --parallel
./build-consumer/fatcat_metadata_out_of_tree_consumer
```

Applications can pass their public packaged data root as the executable's first
argument. The public root APIs are declared in `fatcat/native_project_source.h`;
the existing explicit canonical/target JSON signatures remain supported.
