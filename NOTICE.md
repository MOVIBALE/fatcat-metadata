# Third-party notices

## Slicer profile snapshots

`compatibility/current-src/native-project-sources/` contains native project
profile data collected from the listed application releases. The original
upstream license texts are included under `licenses/third-party/slicer-profiles/`
and are installed in Python wheels at
`fatcat_metadata_data/licenses/third-party/slicer-profiles/`. The manifest
records the source repository, application release, immutable source commit,
license path, and source URL. Fat Cat translation contracts and compatibility
metadata are maintained separately; the upstream profiles remain under their
respective upstream terms.

PrusaSlicer 3 preset data is collected through its native configuration CLI,
which resolves the application's YAML presets. The exact typed JSON values are
stored as a shared base and JSON Patch differences. Its schema is exported by
the same binary. Upstream implementation code and application binaries are not
included. This snapshot remains subject to the upstream license below.

One retained complete project at
`native-project-sources/compatibility-projects/orca-2.2.4-snapmaker-u1-0.4.json`
records an OrcaSlicer 2.2.4 U1 process. It is compatibility data, not a 2.4.2
native profile snapshot. It retains its recorded process provenance and uses
the included OrcaSlicer AGPL-3.0 license text. The file was retained verbatim;
its content hash is recorded in the source descriptor.

| Application snapshot | Upstream repository | Included license text |
| --- | --- | --- |
| Bambu Studio 02.08.02.61 | [bambulab/BambuStudio](https://github.com/bambulab/BambuStudio) | [AGPL-3.0](licenses/third-party/slicer-profiles/bambu-studio/LICENSE) |
| OrcaSlicer 2.4.2 | [OrcaSlicer/OrcaSlicer](https://github.com/OrcaSlicer/OrcaSlicer) | [AGPL-3.0](licenses/third-party/slicer-profiles/orcaslicer/LICENSE.txt) |
| QIDI Studio 02.07.02.60 | [QIDITECH/QIDIStudio](https://github.com/QIDITECH/QIDIStudio) | [upstream license](licenses/third-party/slicer-profiles/qidi-studio/LICENSE) |
| ElegooSlicer 1.5.3.5 | [ElegooOfficial/ElegooSlicer](https://github.com/ElegooOfficial/ElegooSlicer) | [AGPL-3.0](licenses/third-party/slicer-profiles/elegooslicer/LICENSE.txt) |
| Anycubic Slicer Next 2.0.0.3 | [ANYCUBIC-3D/AnycubicSlicerNext](https://github.com/ANYCUBIC-3D/AnycubicSlicerNext) | [AGPL-3.0](licenses/third-party/slicer-profiles/anycubic-slicer-next/LICENSE.txt) |
| Flash Studio 1.7.18 | [FlashForge/Orca-Flashforge](https://github.com/FlashForge/Orca-Flashforge) | [AGPL-3.0](licenses/third-party/slicer-profiles/flash-studio/LICENSE.txt) |
| Snapmaker Orca 2.4.0 | [Snapmaker/OrcaSlicer](https://github.com/Snapmaker/OrcaSlicer) | [AGPL-3.0](licenses/third-party/slicer-profiles/snapmaker-orca/LICENSE.txt) |
| PrusaSlicer 3.0.0-alpha12 | [prusa3d/PrusaSlicer](https://github.com/prusa3d/PrusaSlicer) | [AGPL-3.0](licenses/third-party/slicer-profiles/prusaslicer/LICENSE) |

The QIDI repository's license file is preserved verbatim because its copyright
and preamble differ from the common AGPL text in the other profile repositories.
For Anycubic, no repository tag matching application version 2.0.0.3 was found;
the included license text is from the repository `main` commit recorded in the
manifest. The snapshot version describes the installed application source used
for the profiles, not the revision of that license file. Flash Studio 1.7.18 also
had no matching public source tag; its included license text is from the recorded
1.7.15 source revision. Both native snapshots come from the exact official app
bundles, while these license-source limitations remain explicit.

## Build dependencies

The C++ build uses these upstream projects when matching system packages are
not available. Their original license texts are included and installed with
the wheel under `fatcat_metadata_data/licenses/third-party/`.

| Dependency | Version | License | Upstream | Included license text |
| --- | --- | --- | --- | --- |
| nlohmann/json | 3.12.0 | MIT | [nlohmann/json](https://github.com/nlohmann/json) | [LICENSE.MIT](licenses/third-party/nlohmann-json/LICENSE.MIT) |
| TinyXML-2 | 10.0.0 | zlib | [leethomason/tinyxml2](https://github.com/leethomason/tinyxml2) | [LICENSE.txt](licenses/third-party/tinyxml2/LICENSE.txt) |
| pybind11 | 3.0.0 | BSD-3-Clause | [pybind/pybind11](https://github.com/pybind/pybind11) | [LICENSE](licenses/third-party/pybind11/LICENSE) |

Exact tags, resolved commits, and source URLs for the files above are recorded
in [`licenses/third-party/manifest.json`](licenses/third-party/manifest.json).
The Python build backend is not linked into or shipped as part of the runtime
library. The optional Neroued example and integration job use
`neroued-3mf==0.4.0`; that separate project retains its own license and terms,
and is not a runtime dependency of `fatcat-metadata`.
