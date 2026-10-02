# Python consumer example

This example creates a 20 mm cube and writes a 3MF using Fat Cat's metadata
composer and the public Neroued 0.4.0 writer API:

1. `builtin_project_request.json` selects the exact FatCat-bundled Bambu Studio
   02.08.02.61 A1 mini 0.4 mm source and textured PEI plate. The one-argument
   `compose_project_settings` entry resolves its native machine, process, and
   material profile inheritance and returns the effective project settings.
2. The script queues the cube as a production build item. The part ID comes
   from `add_mesh_object`; the example assigns the next integer as a distinct
   metadata assembly ID.
3. `compose_model_metadata` builds the package metadata description and
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

To use a custom source JSON, pass the explicit project fixture and its request:

```bash
python cpp/examples/python_neroued_consumer/write_example.py \
  --project-json cpp/examples/python_neroued_consumer/project.json \
  --project-request-json cpp/examples/python_neroued_consumer/project_request.json \
  --output /tmp/fatcat-consumer-example.3mf
```
