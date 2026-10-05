# C++ library

The CMake project builds the `fatcat_metadata_core` C++17 library and, by
default, a pybind11 extension module and the native `fatcat` command. The public headers are under `include/`.
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

## Install a standalone SDK

The C++ installation is opt-in so Python wheels do not carry static libraries
or CMake development files:

```bash
cmake -S cpp -B build-sdk -DFATCAT_BUILD_PYTHON=OFF \
  -DFATCAT_BUILD_TESTS=OFF -DFATCAT_INSTALL_CPP=ON
cmake --build build-sdk --config Release --parallel
cmake --install build-sdk --config Release --prefix /tmp/fatcat-sdk
```

Consumers use `find_package(FatCatMetadata 0.1.0 CONFIG REQUIRED)` and link
`FatCatMetadata::Core`. `FatCatMetadata_DATA_DIR` locates the installed public
data root and follows the installation prefix when it is moved. If a matching
system JSON or TinyXML2 dependency was used to build the SDK, that dependency
must also be available to the consumer; bundled dependencies are installed.

The installed `bin/fatcat` queries targets and choices and composes JSON without
Python. Its default data directory follows the SDK prefix when moved. Start
with `bin/fatcat --help` and the [first-use guide](../docs/FIRST_USE.md).
Use [distribution preparation](../docs/DISTRIBUTIONS.md) to archive the SDK and
its source receipt. macOS builds require a deployment target of at least 13.3.

The same consumer example exercises this installed contract:

```bash
cmake -S cpp/examples/out_of_tree_consumer -B build-installed-consumer \
  -DFATCAT_CONSUME_INSTALLED=ON -DCMAKE_PREFIX_PATH=/tmp/fatcat-sdk
cmake --build build-installed-consumer --config Release --parallel
ctest --test-dir build-installed-consumer -C Release --output-on-failure
```

The consumer's `main.cpp`, `request.json` and `CMakeLists.txt` can be copied to
another directory for installed use; no Fat Cat source directory is required.
