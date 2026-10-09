# Windows installer

`Overglaze-Setup-<version>.exe` installs the desktop application and the files
used to prepare game plugins. Choose a writable local folder; another drive,
including G:, is supported. The default is a folder under your user profile.
Installation is per-user and creates a Start menu entry, an optional desktop
shortcut and the normal Windows uninstall entry.

## First launch

The initial configuration screen lets you select your own `nvngx_dlssnr.dll`.
Overglaze verifies the file against its reviewed versions before importing it
(an unrecognized model only with *允许使用未识别的模型* on; see [MODEL.md](MODEL.md)).
The model is never included in the installer. You can continue without a model
to browse captures and register games, then return to Settings to import it.
Installing a game plugin still requires the model and the game-specific checks.

The application installer does not install plugins into your games. Open Game
Library, add a path, review its result, and use the selected game's actions.

## Updates

Run the newer installer. Keep the existing installation directory: game records,
capture output and late-loading launch options can reference it. The installer
checks for processes using the installation and asks you to close them yourself.
It does not terminate games or restart Windows.

Updates replace the packaged program files and documentation. They retain:

- `data/`: settings, game registration, captures and recovery records;
- `app/models/`: your model;
- `app/adapters/` and `app/adapters-retired/`: game-specific packages.

The desktop application's version and a game's installed plugin version are
separate. After an application update, Game Library reports which games have an
update available. Close the selected game and update it there. A successful
application installation does not certify a game or its image quality.

## Uninstall

First remove each game plugin through Game Library and remove any Steam launch
option pointing to `overglaze_launch.exe`. The app's uninstaller checks existing
records and refuses to remove the program while installed or unresolved game
plugins depend on it. It reports the affected entries; it never tries to remove
unknown game files itself.

The uninstaller removes packaged program files, shortcuts and its Windows
uninstall entry. Models, generated adapter packages, settings, captures and
recovery records remain in the chosen folder. Delete them separately only when
you no longer need them. Steam launch options are user-managed; a local scan
cannot establish that every external shortcut or launcher reference was removed.

## Runtime and network

The installer includes the signed Microsoft Visual C++ v14 x64 redistributable
DLLs beside the desktop programs. It does not install a global runtime, change
driver settings or fetch additional components. The NVIDIA model remains
user-supplied. The installer and application work offline; new releases are
downloaded manually.

Windows may show an unknown-publisher prompt for an unsigned preview build.
File hashes allow you to check the distributed bytes, but are not a code-signing
certificate. See the validation record for the actual signing and test status.

## Build

Create a portable payload using `Package-Overglaze.ps1` with `-RuntimeDirectory`
set to the toolchain's x64 `Microsoft.VC143.CRT` redistributable folder. The
packager accepts only the listed Microsoft-signed x64 runtime DLLs and includes
their hashes in the release manifest. It does not copy SDK or debug runtimes.

Compile that payload with `source/powershell/Build-Installer.ps1`, supplying the
path to Inno Setup's `ISCC.exe` and a new output directory. The script verifies
the payload before compiling and writes a Setup checksum and build receipt.
No release is uploaded automatically. Installation-cycle tests use disposable
folders and do not replace an existing Overglaze application.
