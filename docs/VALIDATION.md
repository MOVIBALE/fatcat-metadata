# Validation scope

The CI checks cover package installation, metadata composition, C++ behavior,
an out-of-tree C++ consumer, and a generated 3MF archive's structure and
read-back. They establish that the package can emit and structurally validate
the expected archive members. They do not launch a slicer or establish native
GUI acceptance, slicing success, or printer behavior.

## Automated checks

The `python-extension` job installs the built wheel outside the source checkout
on Ubuntu, macOS, and Windows. It checks the extension import, package-data
paths, all seven exact translation targets, native source catalog, real
profile paths, and built-in project composition for representative machines.

The `cpp` job builds and runs the C++ suite in Debug and Release on all three
platforms. Those jobs do not require a non-C locale. A separate Linux job
installs and generates `de_DE.UTF-8`, enables
`FATCAT_TEST_NON_C_LOCALE=ON`, and uses `ctest --no-tests=error` to run the
comma-decimal case. The `out-of-tree-consumer` job builds and runs a small
downstream CMake project. The `neroued-3mf` job writes a sample archive using
the optional `neroued-3mf` integration and reads it back to check archive
members and CRCs.

## Native GUI acceptance

Native slicer acceptance must be recorded separately from these CI results.
For each supported application used for GUI acceptance, retain the exact
application version and OS, the input 3MF hash, the import/open result, visible
printer/nozzle/material/plate selections, any warning or repair prompt, and
the saved or re-exported output hash. A CI-created and read-back-verified 3MF
is not evidence that a slicer GUI opened or accepted it.

Private or customer acceptance artifacts should remain in the authorized
acceptance record and should not be copied into this public source package.
