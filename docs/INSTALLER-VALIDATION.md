# Windows Setup preview validation — 2026-10-07

The installer, update/uninstall checks and first-run model configuration are
implemented on `codex/windows-installer`, based on the integrated product UI.
The installed application in `DLSSLab/app` and all real game plugins were left
unchanged. No game was launched or NR evaluated for this work.

## Verified

- Release build passed. Provider, runtime and core source files were not changed;
  the controller host DLL remains byte-identical to the earlier UI package.
- Controller Python regression: 74 tests, 73 passed and one expected skip for
  the research track absent from the public repository.
- Application maintenance/model import: 49 checks. Covered unsupported models,
  existing different destinations, matching-file reuse, hard links, incomplete
  and orphaned transactions, malformed records and surviving game configurations.
  Synthetic model bytes were never loaded as a DLL.
- First-run configuration state: 17 checks, with isolated temporary preferences.
- Existing game manager: 5,053 checks; game-library UI: 93 checks over 92 ImGui
  frames; root location: 56 checks. Only temporary fixtures were modified.
- Six targeted native tests passed: synthetic overlay, application maintenance,
  model setup, viewer smoke, capture-viewer controls and color-contract controls.
  GPU availability was confirmed by Kevin; the shared test mutex was respected.
- Real Setup cycle: 21 checks passed on a disposable G: folder containing spaces
  and Chinese characters. Fresh installation, same-directory update, application
  process blocking, unresolved game-record blocking, completed uninstall and
  byte-identical retention of settings/captures/models/adapter sentinels all passed.
  The test registration was removed by the test's own uninstaller.
- Runtime-enabled payload has 43 explicitly listed files. Its ten Microsoft
  runtime DLLs are unmodified, validly signed and x64. No NVIDIA model is packaged.

The real cycle found and resolved an uninstall self-block: Inno's original
uninstaller waits for a temporary worker that starts the checker. The checker
now exempts only the exact owned uninstaller on its process ancestry chain;
other processes mapped from the application directory continue to block.

The build script accepts Inno's own encoded compiler version rather than assuming
the official compiler has useful PE version metadata. The compiler was obtained
from the official 6.7.3 release and its publisher signature was checked. Packaged
file hashes are also embedded into the Inno file entries.

## Window review and handoff

The isolated packaged application was opened on the normal desktop. Its first-run
layout, skip action, Settings opening, reopening configuration and native model
file dialog were inspected. Computer Use was stopped by Kevin's physical Escape
key while the file dialog was open. After Kevin requested manual window testing,
no further computer input was sent.

Kevin's remaining manual checks:

1. Run Setup and choose a fresh local folder. Verify shortcut and first launch.
2. Select a supported model in the first-run screen. Verify import success, enter
   Game Library, and verify Settings reports the model. Unsupported files must
   show an error without replacing an existing model.
3. Close the application, rerun Setup and check the model/settings remain. Remove
   game plugins and their external launch options before uninstalling the app;
   imported models and user data should remain afterward.

The preview Setup is not Authenticode-signed. Its checksum/build receipt identify
the bytes but are not a publisher certificate. It is a local preview, not an
uploaded GitHub release or a new game compatibility certification. The earlier
hard-boundary concurrency review finding remains a separate upstream task.

Functional test output stays under `data/` and is excluded from Git. Installer
tests use only generated fixtures; the final installer and ZIP are the
deliverables, not those fixture directories.
