# Overglaze mark and Windows resources

The original mark is two offset rounded square rings: an image layer and a
glaze layer. It is independent of NVIDIA's identity. The palette is charcoal
`#101418`, pale green `#B6D88C`, muted green `#516747`, and warm white `#F2F1EB`.
`assets/branding/overglaze.svg` is the editable source, under the project's MIT
licence. Keep the rear ring visible and preserve the foreground's dark fill,
which masks the overlap. The geometry uses a 64 × 64 view box and remains legible
at 16 px. The product UI draws the same geometry without loading image files.

Generate the Windows icon inside the build tree:

```powershell
python source/python/generate_brand_icon.py `
  --source assets/branding/overglaze.svg --output build/branding/overglaze.ico
```

The renderer uses only Python's standard library. It reads the SVG geometry,
supersamples each frame, and writes 16, 20, 24, 32, 48, 64, 128 and 256 px DIBs
into one ICO. It makes no network request, uses no fonts and requires no image
library. A repeated run produces identical bytes and leaves an unchanged output
untouched. Generated ICO files belong in the build directory, not source control.

The desktop executable's resource template is
`source/native/controller/resources/application.rc.in`. Resource 101 is the
application icon. Resource 1 contains Windows VERSIONINFO, currently
`0.2.0-preview.3` (`0,2,0,3` numerically), marked as prerelease. Its internal and
original filenames identify `overglaze_viewer.exe`. Do not attach that resource
unchanged to a different executable.

For CMake integration, enable `RC`, use a custom command to generate the ICO
from the SVG and Python script, and configure the template with
`OVERGLAZE_ICON_PATH` and `OVERGLAZE_RESOURCE_HEADER` (forward-slash absolute
paths). Add the configured `.rc` to `overglaze_viewer`, with `OBJECT_DEPENDS`
on the generated ICO. The native window class must also load resource 101 for
both `hIcon` and `hIconSm`; embedding it alone does not set the window icon.
