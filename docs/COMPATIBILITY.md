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
