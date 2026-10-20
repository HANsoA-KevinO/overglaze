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

Update from the app (below), or run the newer installer yourself. Keep the
existing installation directory: game records, capture output and late-loading
launch options can reference it. The installer checks for processes using the
installation and asks you to close them yourself. It does not terminate games or
restart Windows.

Updates replace the packaged program files and documentation. They retain:

- `data/`: settings, game registration, captures and recovery records;
- `app/models/`: your model;
- `app/adapters/` and `app/adapters-retired/`: game-specific packages.

The desktop application's version and a game's installed plugin version are
separate. After an application update, Game Library reports which games have an
update available. Close the selected game and update it there. A successful
application installation does not certify a game or its image quality.

### Updating from the app

With *启动时检查更新* on (Settings, the default), the desktop program checks the
project's GitHub releases when it starts, at most once every 24 hours; *检查更新*
in Settings checks at once. Only public GitHub release data is read (see
[Runtime and network](#runtime-and-network)). When a newer release exists, a
notice at the top of the window shows its version and release notes, with
*更新*, *稍后* and *打开下载页*.

*更新* is offered only to a copy installed by this Setup. The program counts as
installed when all of these hold: it runs from `<folder>\app\`,
`data\settings\application-install.json` (written by the Setup) names this
folder, `app\unins000.exe` exists, and the Setup's Windows uninstall entry points
to this folder. Anything else, a portable ZIP copy included, only shows the new
version and *打开下载页*; it is never installed automatically.

1. The program refuses, with the reason, while a registered game or another
   Overglaze program is running, while a game plugin operation is unfinished,
   or when the program's disk has too little free space. These are the same
   checks the installer makes.
2. It downloads the Setup into `data\updates\<version>\` (with a progress bar
   and *取消*) and checks its size and SHA-256 against the signed manifest. It
   removes older update folders it created itself; nothing else is touched.
3. It starts the Setup and exits:
   `"<Setup>" /SILENT /SUPPRESSMSGBOXES /NORESTART /DIR="<program folder>" /WAITPID=<pid> /RELAUNCH`.
   `/WAITPID` makes the Setup wait up to 60 seconds for the program to exit
   before its usual checks; `/RELAUNCH` reopens the program after a successful
   installation. A refused or failed installation does not reopen it.
4. After the relaunch the program shows *已更新到 <version>* once and, when
   installed games report a plugin update, how many; update those in Game
   Library. Plugins are never updated automatically. If the installation did
   not complete, the next start says so.

Every installable update carries `overglaze-update.json`, signed with the
project's release key, and `overglaze-update.json.sig`. The program has the
release public key built in and installs nothing whose signature does not
verify; the Setup's SHA-256 alone is not trusted, because it comes from the same
place as the Setup. A release without these two files is offered as a manual
download only. The release public key is
`assets/release-key/overglaze-release-p256.public.blob` (ECDSA P-256, SHA-256
fingerprint `92148a47da4ce5ba8cf755a0e3bce14bbbfdd9fb79c3de1e1972fcaa13892e3c`).

`overglaze_games self-update check` runs the same check from the command line
and prints the result; `overglaze_games self-update download` also downloads the
Setup into `data\updates\<version>\` and verifies it, without starting it.

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
user-supplied. The installer works offline.

The desktop program connects to the network only to check for updates: HTTPS
requests to GitHub for the project's public release list and, for an update,
its manifest, signature and Setup. It sends the standard HTTPS headers and
`User-Agent: Overglaze/<version>`, nothing about you, your games or your files,
and no cookies or credentials. Turn *启动时检查更新* off in Settings to stop the
automatic check; nothing connects unless you then press *检查更新*. The in-game
components (the plugin files in a game folder, and the panel) never connect to
the network.

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

## Publishing a release

Sign the self-update manifest after building the Setup and the portable ZIP:

```powershell
powershell -ExecutionPolicy Bypass -File source\powershell\Sign-Release.ps1 `
    -SetupPath <out>\Overglaze-Setup-<version>-win64.exe -ZipPath <out>\Overglaze-<version>-win64.zip `
    -Version <version> -Channel preview -SourceCommit <40-hex commit> -NotesFile <notes.txt> `
    -KeyPath <release private key>
```

`-KeyPath` may instead come from the environment variable
`OVERGLAZE_RELEASE_KEY`. The private key (a CNG `EccPrivateBlob`) is kept
outside every git work tree; the script refuses a key inside one and never
prints or copies it. `-NotesFile` is the UTF-8 plain-text release note shown in
the update notice (`notes_zh`). The script computes the sizes and SHA-256 of
both files, writes `overglaze-update.json` and `overglaze-update.json.sig` next
to the Setup, and verifies the signature with the release public key
(`assets/release-key/overglaze-release-p256.public.blob`) before it finishes;
nothing is written if it does not verify.

Attach both files to the GitHub release, next to the Setup and the ZIP, under
exactly these names. The release tag must be `v<version>`, matching the
manifest's version. A release without them is offered only as a manual download.
