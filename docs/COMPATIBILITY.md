# Recorded compatibility sources

Native profiles are retained unchanged with their actual version and provenance.
Catalogue availability does not imply every material type or build plate works.

OrcaSlicer 2.4.2 U1 0.4 mm composition uses that release's native machine with
the recorded Orca 2.2.4 process. For Textured PEI (including the source default),
the recorded `Snapmaker PLA Basic @U1` material retains its actual 65 °C plate
and 220 °C nozzle settings. Returned `process_source` and `material_source`
identify their respective versions.

The installed 2.4.2 catalogue includes machine-compatible `Snapmaker PLA @U1`,
but its native textured plate temperature is zero; it is usable only on plates
its actual profile supports. The declared `Snapmaker PLA` default supports
A250/A350. Neither profile is rewritten. Material selection checks the exact
machine, requested type and effective plate; incompatible explicit selections
are rejected.

Complete native catalogues also supply exact SnapmakerOrca nondefault-nozzle
and FlashStudio 0.25 mm materials. OrcaSlicer 2.4.2 U1 0.6 mm still has no matching
PLA source. Updating the native snapshots and reviewing compatibility records
is described in [MAINTAINERS.md](MAINTAINERS.md).

PrusaSlicer 3.0.0-alpha12 is a preview target with a separate typed project
format. Its sources come from native CLI preset resolution, not generated mesh
projects or renamed Orca profiles. The catalogue covers the application's 32
default-nozzle FFF selections, each with a native 0.20 mm process and Prusament
PLA. Arbitrary nozzles and materials require an explicit native configuration.
The SDK preserves native tool count, feeder slot capacity and typed values.
Native hardware/data availability is separate from GUI acceptance: only MK4S,
MK4S MMU3 and XL 5T representative outputs have been opened and sliced.
See [PRUSA.md](PRUSA.md) for unsupported operations and exact import semantics.
