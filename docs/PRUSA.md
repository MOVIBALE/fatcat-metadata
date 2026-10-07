# PrusaSlicer 3 native projects / Prusa 3 原生工程

FatCat supports the exact **3.0.0-alpha12 preview**. It composes Prusa's native
project JSON; opening the result with **File → Open Project** loads printer,
process and material configuration. Importing geometry into an existing project
is a different operation and may retain that project's settings.

FatCat 支持准确的 3.0.0-alpha12 预览版。用“打开工程”加载完整设置；导入几何模型
不是完整工程验收。2.x 与其他 3.x 版本不在这份兼容声明内。

## Complete installed example / 完整示例

Install the FatCat wheel and the optional Neroued writer described in
[FIRST_USE.md](FIRST_USE.md), then run outside the source checkout:

```bash
fatcat catalog --slicer PrusaSlicer
fatcat fields --slicer PrusaSlicer
python -m fatcat_metadata_examples.prusa --machine prusa:mk4s --output mk4s.3mf
python -m fatcat_metadata_examples.prusa --machine prusa:mk4s-mmu3-32bit --output mmu3.3mf
python -m fatcat_metadata_examples.prusa --machine prusa:xl-5t --output xl.3mf
```

The examples set 0.20 mm layers, three perimeters, 35% infill, 60 mm/s external
perimeters, 237 °C nozzle temperatures and 0.91 extrusion multipliers. MMU/XL
examples assign four coloured parts and retain five native material slots.
Your generator owns meshes, object IDs and placement. FatCat generates
`Metadata/PrusaSlicer3_project.json`; Neroued writes the archive once.

示例修改层高、围墙、填充、速度、温度与流量。MMU3 是一个喷头加五个换料槽；XL 5T
是五个独立工具。库保留两者不同的结构，不按颜色数量虚构喷头。

## Public composition / 公共调用入口

```python
import fatcat_metadata as fatcat

result = fatcat.compose_project_settings({
    "project_source": "fatcat_native",
    "slicer_id": "PrusaSlicer",
    "application_version": "3.0.0-alpha12",
    "machine_uid": "prusa:mk4s",
    "nozzle_uid": "nozzle:0.4mm",
    "source_materials": [{
        "name": "Generator red PLA", "colour": "#E63946", "material_type": "PLA",
        "native_settings": {
            "temperature": 237, "first_layer_temperature": 237,
            "extrusion_multiplier": 0.91,
        },
    }],
    "process_settings": {"wall_loops": 3, "sparse_infill_density": "35%"},
})
project = result["project_settings"]
```

Use `list_project_options` to discover the bundled native process, materials,
default sheet and process contract. `material_settings_contract` lists accepted
per-slot native filament fields. The bundled catalogue covers 32 default
0.4 mm FFF hardware selections; its native process is 0.20 mm and its material
is Prusament PLA. Availability does not prove every physical printer or feature.

For other nozzles, presets, sheets or materials, use Prusa's `--save config.json`
after selecting native profiles, or read `Metadata/PrusaSlicer3_project.json`
from a saved alpha12 project. Pass that dictionary to
`compose_project_settings(source, request)` and omit `project_source`. FatCat
accepts a native `--save` config or one native FFF project container. It retains
unknown source fields. Native preset names and IDs remain unchanged; generator
material names belong to its own writer base-material group.

其他喷嘴、板型和材料使用原生配置导出或用户工程。不会把 PLA 改名假装成 PETG，也不会
拿相近机型替代。保留原生预设身份，生成器自己的材料名称由写包工具保存。

## Parameter semantics / 参数含义

- Native scalar fields are validated against alpha12's exported schema.
  Typed percentages accept a number, `"35%"`, or `{ "value": 35, "is_percent": true }`
  as appropriate for the field. Conflicting aliases raise errors.
- Reviewed aliases include `wall_loops → perimeters`,
  `sparse_infill_density → fill_density`, and `wall_generator → perimeter_generator`.
  `zig-zag → zigzag` and `monotonicline → monotoniclines` are field-specific.
- Tool-scoped overrides are broadcast to the existing physical tool rows;
  imported per-tool values are retained when no override is supplied.
  `effective_settings` summarizes the first tool for layout; the native project
  retains all tool rows.
- `source_materials[i].native_settings` changes scalar native filament fields
  for that slot. Material arrays, tool assignments and unused hardware slots
  remain intact. More colours than native capacity raise an error.
- The public catalogue reports `filament_slot_policy=preserve_native_capacity`.
  Consumers select `preserve` even for a smaller logical palette; removing native
  MMU slots or XL tools would change physical hardware. Explicit `compact` requests
  remain unsupported. `PLA Basic` and `PETG Basic` are reviewed type labels for
  native PLA and PETG; they do not change the selected material preset or tuning.
- An explicit sheet UID must match `prusa-sheet:<native sheet type>` in the
  selected config. Omission retains the actual native default.
- Bambu `auto_brim` has no reviewed exact Prusa equivalent. It raises an error;
  select Prusa's `no_brim`, `outer_only`, `inner_only`, or `outer_and_inner`
  intentionally. Other unreviewed features are not silently approximated.

没有核对等价关系的专属设置会明确报错。参数类型校验不等于所有参数组合都可切片；
切片引擎仍负责组合约束和实际走线。

## Verified scope and limits / 已验范围与限制

| Native GUI sample | Observed result |
| --- | --- |
| MK4S 0.4 HF, single material | Opened and sliced: 15 layers, 2 min 56 sec. GUI-exported G-code uses 237 °C and 0.91 extrusion multiplier. |
| MK4S MMU3 0.4, four colours | Opened and sliced: 60 layers, 14 min 47 sec; four colour bands and wipe tower visible. |
| XL 5T 0.4, four colours | Opened and sliced: 60 layers, 15 min 4 sec; native multi-tool configuration and coloured tower retained. |

Lumina integration was also opened and sliced in the alpha12 GUI: MK4S MMU3
single image (18 min 5 sec), two-image merge (21 min 50 sec), two-piece puzzle
(19 min 6 sec), XL 5T image (9 min 45 sec), and a user-template image
(17 min 15 sec). Four tool colours and native towers were visible. The
user-template GUI-exported G-code retained 237 °C and 0.91 extrusion multipliers.
No settings were changed in the slicer to bypass warnings.

These are software observations, not physical print validation. The catalogue's
other hardware selections have not each been sliced in the GUI. Unsupported:
SLA, Prusa 2.x, multiple configuration containers, native hardware slot
compaction, and native multi-colour filament records.
Material remapping for combined multi-tool/feeder hardware is not yet supported.
Unsupported component inputs are rejected rather than dropped.

本次同时验了 Lumina 实际生成的单件、批量、拼图、XL 多工具及用户模板，并导出
原生 Gcode 核对温度和流量。没有验证实体打印；目录存在不代表逐机型验过。

## Geometry and merged projects / 几何与合并工程

`metadata_defaults.geometry_layout=core` requests standard 3MF component objects
in the root model. Alpha12's native volume settings look up root-model IDs;
Production-extension external mesh IDs do not retain that mapping. The generator
still owns meshes, transforms and actual object/part IDs. FatCat owns native
object/volume configuration and zero-based palette to one-based extruder mapping.

`merge_sources` retains native filament preset identities and all filament tuning.
Logical slots must agree on material identity, colour and native values. Sources
must share physical hardware and printer settings. Object-scoped process
differences become object overrides; project-wide differences and nonuniform
per-tool values that cannot become one object setting raise errors. A wiping
matrix requires source values for every selected material transition. The final
bed owns tower width and position.

Model inputs with `source_model_settings_xml` pass the **native JSON string**
(the shared argument name is retained for compatibility), `source_assembly_id`,
each part's `source_part_id`, `source_slot_output_indexes` and `output_slot_count`.
FatCat preserves source object/volume settings, height ranges and layer profiles
while remapping IDs/extruders; the writer supplies new instance transforms.
New objects without a source describe their own parts and material indices.

The optional `layer_config_ranges` component accepts ordered nonoverlapping
`min_z`/`max_z` ranges with `layer_height_mm`, `infill_density_percent` and
`use_default_extruder=true`. These become native typed object ranges.
`wipe_tower_placement` remains an optional generator sidecar. The layout summary
uses native purge, ramming, spacing and brim values; `tower_cyclic_toolchanges`
also accounts for the return to the first material between layers. The slicer
computes actual tower geometry, so layout estimation is not a slicing guarantee.

标准 3MF 组件布局让原生体积设置能找到真实 ID。合并保留各源耗材调校、局部设置和
层高区间；无法表示的工程级差异明确拒绝。原生换料槽和工具数量保持不变。
塔的摆放估算读取原生冲刷、卸料和边缘值，最终走线仍由切片器计算。

## Source maintenance / 来源维护

`scripts/sync_prusa_sources.py` calls `--query-printer-models`,
`--query-print-tool-filament-profiles`, `--save` and `--export-config-schema` on
the exact binary, with an isolated temporary data directory. It creates no mesh
and no project per machine. One shared typed configuration plus deterministic
JSON Patch differences preserves exact native values without reimplementing
Prusa's YAML preset evaluator. The same refresh updates contracts and packaged
source routing; the old profile updater preserves this independent catalogue.

The release, source commit, binary hash and schema hash are recorded. The
upstream license is preserved verbatim in the wheel and C++ data installation.
See [MAINTAINERS.md](MAINTAINERS.md) and [NOTICE.md](../NOTICE.md).

Official references: [alpha12 release](https://github.com/prusa3d/PrusaSlicer/releases/tag/version_3.0.0-alpha12),
[native import semantics](https://github.com/prusa3d/PrusaSlicer/blob/version_3.0.0-alpha12/doc/3mf-import-matrix.md).
