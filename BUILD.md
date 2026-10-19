# Build AIMP Companion Control 0.3.1 preview

For installation, download `AimpCompanionControl-0.3.1-preview.aimppack` from the [0.3.1 preview release](https://github.com/ninoleto/AIMP-Companion-Control/releases/tag/v0.3.1-preview), verify its checksum and retain the accompanying notices; no build tools are required. These instructions rebuild the native plugin into `dist/`, which is excluded from Git. The Companion module has its own source package and build process.

## Build environment

The build path is **Linux or WSL**, with Bash, Python 3, native GCC/G++, and the pinned LLVM-MinGW cross-compiler. The source includes SDK headers and the JSON dependency. No prebuilt upstream plugin DLL is reused.

Pinned toolchain: **LLVM-MinGW 20261006, UCRT, Ubuntu 22.04 x86_64 host**.

- Release: https://github.com/mstorsjo/llvm-mingw/releases/tag/20261006
- Archive: `llvm-mingw-20261006-ucrt-ubuntu-22.04-x86_64.tar.xz`
- SHA-256: `5f9c6ed95b2d4bdb2869a488c5fd5857fbdabcf288a0aa3eb1da43f6a08d8ab4`

The compiler is not bundled. Download it from that release, verify its hash, and extract it before building. The hash above was checked against the release metadata and downloaded archive.

## Commands

Run these in the extracted source directory inside Linux/WSL, replacing the toolchain directory with its actual location:

```bash
bash scripts/test.sh
AIMP_TOOLCHAIN=/absolute/path/to/llvm-mingw-20261006-ucrt-ubuntu-22.04-x86_64 bash scripts/build.sh
```

On Windows, open the WSL shell first; these are Bash commands, not PowerShell commands. Windows drive `D:` is normally available at `/mnt/d` in WSL.

The test script runs portable WebSocket/protocol/input tests and audio validation tests with **AddressSanitizer and UndefinedBehaviorSanitizer**. Audio tests cover ranges, rejected nonfinite/malformed values, clamped relative adjustments, and dynamic band indexes. LeakSanitizer is disabled because the build environment restricts its process inspection. These tests do not instantiate the Windows AIMP SDK.

The build script produces:

```text
dist/AimpCompanionControl-i686.dll
dist/AimpCompanionControl-x86_64.dll
dist/AimpCompanionControl-0.3.1-preview.aimppack
dist/SHA256SUMS.txt
```

Legacy controls and the audio, player, and playlist/queue extensions are compiled together. The C++17 runtime is linked statically against Windows UCRT; ASLR and NX are enabled. PE timestamps and package member dates are fixed for reproducibility. No packing, obfuscation, signing, background service, or updater is used. Toolchain licence notices are in `third_party/toolchain-licenses/`.

The x86 alias in `src/plugin.def` supplies AIMP's undecorated `AIMPPluginGetHeader` entry point; x64 exports it directly. The `.aimppack` includes both architectures and the language file.

## Release verification

Consult `docs/VERIFICATION.txt` for checks performed on the delivered files. Real Windows/AIMP acceptance testing remains necessary for the new audio controls, EQ, device switching, recording, modal dialogs, playlist/queue editing, reconnect, and plugin shutdown/reload.

The DLLs are unsigned, and no Defender acceptance is claimed. Scan the exact packaged files with current Defender and retain hashes/detection details if a problem appears. Suspected false positives can be submitted to https://www.microsoft.com/en-us/wdsi/filesubmission. Do not change antivirus settings to make a build pass. Publisher signing can identify a release but does not replace testing.

Follow `README.md` to install or roll back, preserving the previous working plugin/module pair while evaluating this preview.

The 0.3.1 build also links Windows oleaut32 for documented music-library VARIANT/BSTR handling. New portable validation and SDK-double tests are included in scripts/test.sh; none substitutes for execution inside AIMP.
