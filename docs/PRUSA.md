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

These are software observations, not physical print validation. The catalogue's
other hardware selections have not each been sliced in the GUI. Unsupported:
SLA, Prusa 2.x, multiple configuration containers, merged source tuning/slot
compaction, layer-range components, and native multi-colour filament records.
Material remapping for combined multi-tool/feeder hardware is not yet supported.
Unsupported component inputs are rejected rather than dropped. The SDK's
single model and explicit object/part metadata use actual writer IDs and
zero-based material indices. Source object-specific settings are not copied
to newly described objects. Use a reviewed generator mapping when that is needed.

本次没有验证实体打印。暂不支持项见上；目录存在不代表逐机型验过。新增支持发生在
FatCat SDK，Lumina 的切片选择界面与合并生成流程没有因此自动获得 Prusa 支持。

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
