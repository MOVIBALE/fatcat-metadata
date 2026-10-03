# Python consumer example

This example creates a 20 mm cube and writes a 3MF using Fat Cat's metadata
composer and the public Neroued 0.4.0 writer API:

1. `builtin_project_request.json` selects the exact FatCat-bundled Bambu Studio
   02.08.02.61 A1 mini 0.4 mm source and textured PEI plate. The one-argument
   `compose_project_settings` dictionary entry resolves its native machine, process, and
   material profile inheritance and returns `project_settings` as a dictionary.
2. The generator centers the cube on the selected project's rectangular
   `printable_area`, including its native origin offset, and places its bottom
   at Z=0. It validates the cube against that area and `printable_height`.
   Geometry placement is a generator responsibility; native presets stay intact.
   The default uses the public core assembly API: `add_mesh_object` creates
   the part, `add_component_object` returns the real assembly ID, and the
   placement is assigned to its build item. All material groups remain in the
   root model resources.
3. `compose_model_metadata` consumes that final dictionary and builds the package metadata description and
   `fatcat_metadata_neroued` applies it before neroued writes the 3MF once.

The default example uses the bundled native source without `project.json`. It
has one PLA slot, no image resources, and omits the optional wipe-tower
placement input, so no wipe-placement sidecar is emitted. The native profiles
provide the slicer's machine, process, and material settings; Lumina product
choices such as layer height and infill policy are not added by this entry.

Install `fatcat-metadata` from the repository root and install the optional
writer dependency separately:

```bash
python -m pip install . 'neroued-3mf==0.4.0'
```

Then run the built-in source example:

```bash
python cpp/examples/python_neroued_consumer/write_example.py \
  --output /tmp/fatcat-consumer-example.3mf
```

To generate the Orca 2.4.2 U1 sample with its selected native bed geometry:

```bash
python cpp/examples/python_neroued_consumer/write_example.py \
  --project-request-json cpp/examples/python_neroued_consumer/orca_u1_project_request.json \
  --output /tmp/fatcat-u1-example.3mf
```

The retained U1 process and material settings are labelled with their actual
2.2.4 provenance (`process_source` and `material_source`);
placement uses the composed 2.4.2 hardware's printable area. Run
`verify_3mf.py PATH PROJECT_REQUEST_JSON` after either export to check the
serialized cube geometry with component and production transforms applied,
plus material name, colour, and slot against the same selected request.
The builder reads that palette too; custom sources without an explicit palette
use the composed project's first material and colour.

To use a custom source JSON, pass the explicit project fixture and its request:

```bash
python cpp/examples/python_neroued_consumer/write_example.py \
  --project-json cpp/examples/python_neroued_consumer/project.json \
  --project-request-json cpp/examples/python_neroued_consumer/project_request.json \
  --bed-center 90 90 \
  --output /tmp/fatcat-consumer-example.3mf
```

The minimal custom fixture has no printable-area geometry, so its caller supplies
the bed center explicitly. For complete native projects, this option may override
the default center but must keep the cube strictly inside the declared rectangle.
The example supports rectangular beds and rejects unsupported shapes or a cube
that exceeds the selected bed or height; it does not arrange models in a slicer.

The optional `--production` switch writes an external production model and
requires a writer that preserves its material groups, such as the B7 writer
used by Lumina. Original Neroued 0.4.0 omits material groups in both production
modes; its supported default here is the core assembly layout. Both layouts
are checked for real material references, colours, slot indices, UUIDs and
transformed geometry. No generated ZIP member is rewritten.
