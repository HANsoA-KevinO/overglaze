# Portable preview

Extract the entire ZIP to a local drive and open `Open-Overglaze.cmd`, or open
`app/overglaze_viewer.exe` directly. The launcher only opens the desktop program.
It does not install anything into a game. Keep the `app` folder and the rest of
the extracted package together.

The NVIDIA model is not included. Place your own unmodified `nvngx_dlssnr.dll`
in `app/models/` as described in [MODEL.md](MODEL.md). The desktop program explains
the model status before installation. NR remains off each time a game starts.
Read [POLICY.md](../POLICY.md) before using any game installation feature.

The program creates its data folder inside the portable root when needed. Keep
the extracted folder writable. A ZIP is a distribution format, not a place to
run the program from. No driver, registry, security setting or Windows service is
changed by extracting or opening this package.

The binaries use the Microsoft Visual C++ v14 x64 runtime. If Windows reports
that `MSVCP140.dll`, `VCRUNTIME140.dll` or `VCRUNTIME140_1.dll` is missing, install
the x64 runtime using [Microsoft's official instructions](https://learn.microsoft.com/en-us/cpp/windows/latest-supported-vc-redist).
Its version must be at least as recent as the MSVC tools used to build the package.
Do not download individual DLLs from third-party sites. Overglaze does not install
or update the runtime, drivers or security settings for you.

Before moving or deleting a used Overglaze folder, uninstall its game entries
and remove Steam launch options that reference it. Existing game installations
can depend on paths into that folder. Extract a new preview into a separate
folder; do not overwrite an old installation or copy its runtime data blindly.

## Build a package locally

After building the public controller track (see [CONTRIBUTING.md](../CONTRIBUTING.md)):

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File source/powershell/Package-Overglaze.ps1 `
  -BuildDirectory .\build -OutputDirectory .\data\packages `
  -Version 0.2.0-preview.1
```

This makes an unpacked `Overglaze-0.2.0-preview.1-win64` folder, the matching ZIP,
and a `.zip.sha256` checksum. It does not create a Git tag, release or upload.
Existing outputs are refused. Use a new output directory for another attempt;
a failed attempt may leave its partial output for inspection. The script never
deletes output, game files or user data.

Packaging uses an explicit allowlist of eleven built programs/DLLs, the public
licence texts, and named documents. It ignores model files, game data, screenshots,
local root overrides, logs, debug symbols and any other unlisted build output.
Every input and output ancestor is checked for reparse points. Input payloads
are bounded to 512 MiB. Run the build and packaging from a stable checkout with
no concurrent writers.

`app/release-manifest.json` records the requested preview version, the checkout's
Git commit and dirty state observed at packaging time, and the SHA-256 and size
of every payload file. It excludes itself; the adjacent ZIP checksum covers the
whole archive, including the manifest. It contains no build directory, user
name, remote URL or local game paths. These hashes detect changed files; they
are not a signature or a statement of official NVIDIA support.

The manifest accurately reports a dirty source tree but cannot prove that a
supplied executable was built from that tree. It states this limitation. Review
the current source, rebuild, run the relevant tests and inspect the package
before distributing it. Changing `-Version` labels the package; it does not
rewrite the executable's embedded version. Keep those versions in sync.

ZIP entries have sorted names and a fixed timestamp (2000-01-01 UTC). Identical
inputs, checkout state, version and PowerShell/.NET compression implementation
produce identical archive bytes. A different compiler or compression runtime
is not promised to produce byte-identical binaries or archives.
