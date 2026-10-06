# Process settings across slicers / 跨切片软件工艺参数

FatCat translates reviewed equivalents and validates explicit overrides against
the selected target's supported subset. It does not translate every feature in
every slicer or promise identical toolpaths between different engines.

FatCat 会转换已核对的等价写法，并按目标软件校验显式输入。它不承诺转换所有专属功能，
也不承诺不同切片引擎产生相同走线。

## Discover the contract / 查询支持范围

```python
import fatcat_metadata as fatcat

options = fatcat.list_project_options({
    "slicer_id": "BambuStudio",
    "application_version": "02.08.02.61",
    "machine_uid": "bambu-lab:a1-mini",
    "nozzle_uid": "nozzle:0.4mm",
})
contract = options["process_settings_contract"]
print(contract["fields"]["sparse_infill_pattern"])
print(contract["aliases"])
```

The same contract is returned by `fatcat choices` and the C++
`native_project_options` entry point. `fields` lists 38 supported native fields,
their types, numeric bounds, native array shape and accepted enum values.
`aliases`, `value_aliases` and `unsupported_fields` explain the normalization.
Inspect `source` for the definition revision and whether it matches the exact
application release. This subset is distinct from the application's full schema.

同一份契约也可通过 CLI 和 C++ 查询。目前有 38 个工艺字段；可查询类型、范围、数组格式、
枚举值、别名与明确不支持项。`source` 记录依据的源码版本及其是否与应用版本完全对应。
这里列出的是 SDK 支持范围，不是切片软件的全部设置。

## Compose once / 一次合成

Add `process_settings` to a normal native or user-project request:

```python
request["process_settings"] = {
    "layer_height": 0.16,
    "first_layer_height": 0.24,
    "wall_loops": 3,
    "sparse_infill_density": "25%",
    "sparse_infill_pattern": "rectilinear",
    "brim_type": "none",
    "outer_wall_speed": 73,
    "inner_wall_speed": 121,
    "sparse_infill_speed": 137,
    "travel_speed": 213,
    "enable_support": False,
}
settings = fatcat.compose_project_settings(request)
```

- `first_layer_height` and legacy `initial_layer_height` resolve to the FFF
  native field `initial_layer_print_height`. The SLA field of the same name is
  not written. Conflicting aliases in one object are errors.
- `rectilinear` and Bambu/QIDI `zig-zag` name the same native `ipRectilinear`
  pattern. `zigzag` is a distinct pattern and is **not** substituted.
- `brim_type="none"` resolves to native `no_brim`.
- Scalars are serialized as native text. Numeric density `25` becomes `25%`.
  A scalar speed applies uniformly to the target's existing native array
  entries (including flow-mode entries) when that target uses arrays; scalar
  targets retain a single value. Per-entry array inputs are not supported.
- Top-level process fields remain accepted and override nested values for
  the same resolved field. Unknown nested keys, nulls, arrays, unsupported enum
  values, invalid types and out-of-range values are errors rather than ignored.
- `print_speed` is rejected: it has no unambiguous native overall-speed field.
  Select specific wall/infill/support speeds instead. An Orca-only value such
  as `quartercubic` is rejected for Bambu with its accepted values listed.

首层只需输入一次；直线填充、无 Brim 等已知等价项会自动规范化。速度按原生单值或数组格式
写入；数组输入暂不支持。顶层显式值覆盖嵌套值，同一个对象中别名冲突会报错。
未知字段、空值、类型或范围错误、目标不支持的枚举值都会明确报错；没有含义明确的“整体速度”
映射，因此应填写具体速度项。

## Scope and verification / 范围与验证

Validation applies to new explicit overrides. Unknown fields already present in
a user project remain preserved. A project can still contain incompatible
pre-existing settings or interacting options; native slicing remains necessary.
Machine, nozzle, material, plate, firmware and physical-print constraints are
not reduced to this process-field contract.

Current definitions match the exact Bambu, Orca, QIDI, Elegoo and Snapmaker
releases. Flash 1.7.18 and Anycubic macOS 2.0.0.3 had no matching public source
revision when checked on 2026-10-06. Their contracts use the recorded public
definition baseline plus exact application defaults for field presence and
shape; representative native GUI exports are checked separately. New upstream
enum choices are not automatically added or claimed to be exhaustively verified.
Anycubic's Linux 2.0.0.5 build is not the packaged 2.0.0.3 snapshot.

校验针对新输入，用户工程中原有的未知字段继续保留；相互影响的设置仍需原生切片验证。
Flash/Anycubic 没有找到与当前安装包对应的公开源码，所以契约明确标注了较旧的源码依据，
同时核对当前应用导出的字段与数组格式。新版专属枚举不自动加入，也不宣称所有枚举都已逐项实测。

## Representative native slicing / 代表性原生切片

On 2026-10-06, the independent cube example was opened and sliced in all seven
macOS applications below. Inputs included a 0.16 mm layer, 0.24 mm first layer,
25% rectilinear infill, three walls, and wall/infill/travel speeds of
73/121/137/213 mm/s. These are software observations, not physical-print results
or coverage of every combination.

| Application | Version | Machine, 0.4 mm nozzle | Native slice preview |
| --- | --- | --- | --- |
| Bambu Studio | 02.08.02.61 | A1 mini | Completed |
| OrcaSlicer | 2.4.2 | P1S | Completed |
| QIDI Studio | 02.07.02.60 | Q2 | Completed |
| ElegooSlicer | 1.5.3.5 | Centauri Carbon 2 | Completed |
| Anycubic Slicer Next | 2.0.0.3 | Kobra S1 | Completed |
| Flash Studio | 1.7.18 | AD5X | Completed |
| Snapmaker Orca | 2.4.0 | U1 | Completed |

Anycubic and QIDI displayed a global Cool Plate selection while the current
plate used Textured PEI. Their actual exported G-code recorded
`curr_bed_type = Textured PEI Plate`, with 55 °C (Anycubic) and 60 °C (QIDI).
The per-plate selection was adopted. This does not establish other plate or
material combinations.

七款软件均真实打开并生成切片预览，未发送打印任务。Anycubic/QIDI 的全局热床显示与
当前打印板不同；核对实际 G-code 后确认所选纹理 PEI 板被采用。此表只记录上述样本，
不代表所有机型组合都已验证。
