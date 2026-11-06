# November 2026 slicer refresh / 2026 年 11 月切片器更新

This source update adds Bambu H3C and refreshes six pinned application snapshots.
It is not yet a new PyPI release. Application update checks and native project
acceptance are separate results; a failed updater is not recorded as “latest.”

本次源码更新增加 H3C，并更新六个目标版本，尚未发布到 PyPI。
软件更新查询、原生配置导入、实际切片分别记录，不能互相替代。

## Versions and update channels

| Application | Previous SDK target | Refreshed target | In-application observation on 2026-11-06 |
| --- | --- | --- | --- |
| Bambu Studio | 02.08.02.61 | **02.09.01.70** | Help → Check for updates reported the newest version. |
| OrcaSlicer | 2.4.2 | **2.4.2 retained** | Updater offered **2.5.0-rc**. This is a release candidate, not a replacement for the stable target. |
| QIDI Studio | 02.07.02.60 | **02.07.02.73** | Help → Check for updates reported the newest version. |
| ElegooSlicer | 1.5.3.5 | **1.5.3.5 retained** | Native request timed out. A separate request to the same vendor update endpoint returned 1.5.3.5. |
| Anycubic Slicer Next | 2.0.0.3 | **2.0.0.6** | Installed 2.0.0.2 offered 2.0.0.6. Earlier CDN TLS failures cleared on November 7; the native updater downloaded the complete 139.22 MiB official macOS installer. Installed version, code signature, bundled profiles and exported defaults were checked. |
| Flash Studio | 1.7.18 | **1.7.19** | Installed Apple's Rosetta and updated the installed app from the official 1.7.19 macOS package. Help → Check for updates failed with an SSL connection error; this is not recorded as “latest.” Native slicing succeeded. |
| Snapmaker Orca | 2.4.0 | **2.4.1** | Native update request failed with curl timeout 28. The official 2.4.1 release was downloaded and checked independently. |
| PrusaSlicer preview | 3.0.0-alpha12 | **3.0.0-alpha13** | Opened the app's Software releases link. The separate preset update offered PrusaResearch 1.0.11 → 1.0.21; the refreshed SDK catalogue already contains bundled 1.0.21. |

Official release sources: [Bambu](https://github.com/bambulab/BambuStudio/releases/tag/v02.09.01.70),
[QIDI](https://github.com/QIDITECH/QIDIStudio/releases/tag/v2.07.02.73),
[Snapmaker](https://github.com/Snapmaker/OrcaSlicer/releases/tag/v2.4.1),
[Prusa](https://github.com/prusa3d/PrusaSlicer/releases/tag/version_3.0.0-alpha13),
[Flash downloads](https://www.flashforge.com/pages/download-software),
[Anycubic downloads](https://www.anycubic.com/zh/slicerNextDownload).
Exact profile source revisions and license provenance are recorded in
[`licenses/third-party/manifest.json`](../licenses/third-party/manifest.json).

## Native data and composition

- H3C uses the native 0.2 / 0.4 / 0.6 / 0.8 mm machine and process profiles.
  Its physical extruder count, nine changeable nozzles, material variants and
  process variants remain separate axes. Adding material rows does not duplicate
  hardware rows.
- Bambu 2.9 material rules include the new native fields and drying groups.
  Native profile values, inheritance and user process overrides are preserved.
  Mixed PLA/TPU presets can carry different lengths for native drying-device
  lists. Composition, source-slot reorder and source-preserving merge use each
  resolved preset's field boundaries instead of imposing equal lengths. This
  information remains internal; exported projects contain native fields only.
  Merge also respects target-declared material fields such as
  `fins_extrude_safe_temp`, rather than treating them as global process settings.
- Material roles and preset boundaries now use a typed per-composition context,
  without adding runtime keys to target JSON. Native and edited uniform arrays
  share slot-range projection in composition, reorder and merge. Regression
  coverage includes native/custom sources in either order, compaction, repeated
  and default slots, missing-palette completion, target-bound temperature groups,
  explicit non-material roles and same-slot conflicts. Unknown uneven arrays
  report an actionable shape error instead of silently remaining unmapped.
- Anycubic 2.0.0.6 uses bundled profile version 1.3.2610.08. The refresh imports
  415 profile files for 35 machine/nozzle identities and records 659 observed
  fields. Public source definitions remain the recorded unmatched-main baseline;
  exact 2.0.0.6 defaults supply current field shapes. Slice headers use the
  build timestamp observed in a native GUI-saved project.
- Flash 1.7.19 profiles come from the official macOS package. Exact defaults were
  exported by the official **Windows 1.7.19 executable** using `--export-settings`.
  Its public source definitions remain the available 1.7.15 baseline; the contract
  records that distinction instead of claiming newer source code exists.
- Prusa schema, resolved presets, project identity and source provenance now
  follow the selected target. The catalogue contains 32 default-nozzle FFF
  selections, 222 scalar process fields and 65 scalar material fields.
- Partial native refreshes retain unselected slicers, Prusa routing and the
  reviewed Orca U1 compatibility source. Upstream profile bytes are copied
  unchanged; UTF-8 BOMs and duplicate keys are interpreted like the native parser.

## Actual GUI acceptance

Each row below used a FatCat-generated project opened as a project in the stated
native application, performed slicing, and saved a separate native project copy.
No physical printing was performed.

| Application / machine | Sample | Observed result |
| --- | --- | --- |
| Bambu 02.09.01.70 / H3C 0.4 mm | Nine PLA colours in stacked layers | Correct H3C hardware and nine material rows; 18 layers, 8 changes, 2.33 g; slicing succeeded. |
| Bambu 02.09.01.70 / H3C 0.4 mm | Eight PLA slots plus TPU for AMS in slot 8 | GUI slicing succeeded: 18 layers, 8 changes, 3.35 g. Native save retained PLA/TPU identity, 220/230 °C temperature groups, 0.2 mm layers and three walls. |
| QIDI 02.07.02.73 / Q2 0.4 mm | 20 mm cube, PLA, Cool Plate | 100 layers, 4.17 g, 15m42s; saved native settings retained layer height 0.2, wall loops 3 and outer-wall speed 47. |
| Snapmaker 2.4.1 / U1 0.4 mm | 20 mm cube, PLA, textured PEI | 100 layers, 4.45 g, 9m21s; saved native settings retained layer height 0.2, wall loops 3 and both native outer-wall speed variants at 47. |
| Prusa alpha13 / MK4S 0.4 HF | Installed Prusa example, 20 × 20 × 3 mm | Native project presets loaded; 15 layers, 1.08 g, 2m56s; three perimeters and 35% infill visible in the sliced preview. |
| Flash 1.7.19 / AD5X 0.4 mm | 20 mm cube, teal PLA, textured PEI selection | 100 layers, 4.20 g, model time 15m1s plus 59s preparation; saved native settings retained layer height 0.2, wall loops 3 and outer-wall speed 47. |
| Anycubic 2.0.0.6 / Kobra X 0.4 mm | Complete showcase cube with 19 custom process settings | 124 layers, 4.66 g, 19m58s. All 19 normalized overrides survived native save. GUI-exported G-code confirms textured PEI, 60 °C bed, 0.16/0.24 mm layers, 25% gyroid and outer-wall speed 73. |

Anycubic's global selector displays Cool Plate while the active plate retains
Textured PEI. The native project preserves the per-plate selection; exported
G-code uses `curr_bed_type = Textured PEI Plate` and `M140 S60`, not the Cool
Plate preset's 35 °C. Its isolated command-line slicing attempt crashed before
slicing; the successful acceptance above uses the GUI and its exported G-code.

The Flash GUI run exposed a separate example omission: `write_example.py` did
not supply the target-required slice UUID, while `showcase.py` already did.
Both now use the same generator helper for the UUID and requested plate facts.
The complete Flash example is generated and checked in the writer integration
job; no SDK validation is bypassed and no archive is rewritten.

The earlier alpha12 MMU3 and XL 5T checks remain historical evidence, not alpha13
acceptance. Unchanged targets were not all re-sliced in this refresh.

## Scope and delivery

H3C PLA + explicitly selected Bambu PETG HF composes successfully. Mixed PLA +
TPU for AMS composition, slot reorder and source-preserving merge are covered
by regressions; the nine-slot mixed project was also sliced in Bambu's GUI.
Known preset boundaries are used only when the full source array matches them.
Edited native projects with ordinary per-slot vectors retain the existing
projection rules; this does not claim arbitrary custom ragged-array support.

No claim covers every hardware/material combination or physical print quality.
No Lumina release or dependency update is included in this SDK change. PR review,
merge and a new PyPI release remain separate delivery steps.

自动检查覆盖全部打包目标及新增 H3C 参数轴；真实 GUI 结果只覆盖上表样本。
Flash、H3C 混用 TPU 和 Anycubic 新版的代表性 GUI 验收已完成；实际输出和截图
均已保留。未进行实体打印，本次也未修改 Lumina 或发布 PyPI。

Validation after the material-context refactor: 85 Python tests and seven C++
test executables passed on macOS
ARM64. Indexed native profile paths were checked. First-party edits pass
`git diff --check`; vendor profiles retain upstream CRLF/trailing whitespace
instead of being reformatted during import.

After the Flash example fix, all 10 consumer geometry/material checks passed,
and actual archive generation/verification passed for the Bambu default, the
Flash complete example and the shared Flash showcase. The preceding source
checkpoint passed all 36 GitHub checks (push and pull-request runs).

The refactor reproduces the previously GUI-sliced H3C mixed-material project's
settings exactly (zero changed fields). A fresh Anycubic showcase archive also
passes serialized geometry/material verification. These are output comparisons
and archive checks, not a claim of another GUI run.
