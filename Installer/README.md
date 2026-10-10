# GamEngine Setup (Windows x64)

The release installer is a small native Windows application in `Installer/Bootstrapper`. It contains no editor payload or .NET runtime. At installation time it reads the latest published release of [B4rtekk1/GamEngine](https://github.com/B4rtekk1/GamEngine/releases), downloads `GamEngineEditor.zip`, and checks its size and GitHub's SHA-256 asset digest before extraction.

## Build a local release

On a Windows machine with Visual Studio 2022 C++ tools, CMake, the Vulkan SDK, and Slang:

```powershell
./Installer/Build-Release.ps1 -Version 0.1.0
```

The script configures a distribution Release build, builds `PackageEditor` into a clean directory, checks essential files, and writes `Installer/dist/GamEngineEditor.zip`, `manifest.json`, and `GamEngineSetup.exe` with SHA-256 checksum files. Upload `GamEngineEditor.zip` and `GamEngineSetup.exe` as assets on the matching GitHub release. The installer reads the SHA-256 digest that GitHub computes for the uploaded ZIP. The `out` and `dist` directories are ignored by Git.

If a code-signing certificate is available, pass `-CertificateThumbprint <thumbprint>`. The script signs the final setup executable. An unsigned build is suitable for local testing; do not distribute it as a trusted update.

## Install and update

The default location is `%LOCALAPPDATA%\Programs\GamEngine`. The installer downloads the latest release, validates the ZIP, extracts into a staging directory, moves the completed directory under `Versions/<version>`, then atomically changes `current.json`. The installed setup executable also launches the active `Editor.exe` from shortcuts. Previous versions remain in place for rollback. Running setup again in the same location fetches the latest release. To switch back to the previous version, run the installed `GamEngineSetup.exe --rollback`.

The setup window includes folder selection, shortcut preferences, download progress, a link to the project page, and the full Apache License 2.0 text directly on the license step. Installation is available only after the user accepts the license. On completion, the user can choose whether to launch the editor. The license and application icon are embedded in the small setup executable.

The setup registers an entry under the current user's Installed apps list. The uninstall command starts a temporary copy of setup, waits for the original to close, then removes the version directories, known shortcuts, and registry entry. Unknown files in the install directory are preserved. A log for background failures is written to `%TEMP%\GamEngineSetupError.log`.

This release path checks GitHub when setup is run. It does not perform unattended background update checks. HTTPS plus GitHub's asset digest verifies transport and asset integrity, but a separately signed manifest would provide stronger publisher authentication. Installation is per-user and does not elevate into Program Files.

## Verify

Run `GamEngineSetup.exe --verify-release <temporary-zip-path>` to download and verify the latest release without installing it.

Before a public release, test first launch, update, and uninstall on a clean Windows machine without Visual Studio, the Vulkan SDK, .NET, or Windows App SDK. The editor still needs a Vulkan-capable driver. C++ script compilation can require a separate toolchain.
