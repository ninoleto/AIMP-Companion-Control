# Publication review — 19 October 2026

Reviewed pair: native **0.3.1 preview** and Companion **1.0.0-beta.8**. Source was imported from `AIMP-Companion-0.3.1-beta8-full-source.zip` (SHA-256 `7d6a3c741e3698e7bbb01f1c5de78171e7a8dd2d5e86c558088cae7467732ada`). The supplied archive contains no Git history. Upstream revisions and attribution remain in each project's provenance file.

## Focused review and preparation

Read both READMEs, build instructions, provenance, licences and verification reports. Reviewed the WebSocket limits and access checks, serialized module transport and timeout handling, native focused-target resolution, dynamic versus explicit playlist selection, corresponding regression tests, dependency pins, package versions and distribution contents. This is a focused review, not an exhaustive security audit.

All implementation files, tests, SDK headers, dependency pins, lockfiles and existing notices are retained unchanged. Publication changes add download links, review documentation, exclusions for generated/temporary files, and the JSON library's complete MIT licence. The vendored JSON header has SPDX notices rather than the full licence claimed in the original provenance text; `third_party/json-LICENSE.MIT` supplies the exact upstream v3.11.2 text and the provenance text is corrected. Native release notices accompany the original installer as a separate asset, because the installer contains only DLLs and language files. Retain those notices with binary distributions.

`FILE-SHA256SUMS.txt` in each project is retained as the original archive inventory, not a checksum of the edited publication documentation. It includes generated files intentionally omitted from Git. The original extracted archive remains the reference for that inventory. Release `SHA256SUMS.txt` files describe the downloadable release assets.

## Checks rerun during publication

- Verified every original inventory entry before publication changes: 109 native files and 123 module files.
- Verified both supplied packages against their verification reports and their copies inside the source archive.
- Read every native installer member and verified both DLLs and both language-file copies against the archive's standalone files.
- Verified the vendored JSON header hash: `665fa14b8af3837966949e8eb0052d583e2ac105d3438baba9951785512cf921`.
- Verified packaged module `main.js`: `4efce140b20e5ac0b34e3fb01523eb7ad1894a1b82163560fb9fe32c097908fb`.
- Installed exact module lockfile dependencies with lifecycle scripts disabled. Node **22.23.3**: **102 tests passed**, with zero failures/skips; official `npm run check` passed. Initial restricted-sandbox attempts could not spawn subprocesses; both checks passed when rerun with the required process access.
- Ran `npm run test:host -- --features --advanced` against the **supplied unchanged TGZ**: 192 actions, 46 feedbacks, 503 presets, 624 variables, zero unexpected host warnings, and two separately expected diagnostic warnings. Saved stale removal presets, strict explicit/destination targets, simulated read-only errors, migration, property readback, information summaries and media/library controls passed.
- Parsed all four PowerShell diagnostics successfully; did not execute them against AIMP.
- Checked source for common private-key and GitHub/AWS token patterns; found none. This is a limited pattern check.

The native ASan/UBSan suite, SDK-double tests and pinned x86/x64 cross-build were **not rerun** on this Windows machine: WSL and a suitable C++ toolchain are unavailable. Their supplied results remain in [native verification](https://github.com/ninoleto/AIMP-Companion-Control/blob/v0.3.1-preview/docs/VERIFICATION.txt). The module's supplied evidence remains in [module verification](https://github.com/ninoleto/companion-module-ninoleto-aimp/blob/v1.0.0-beta.8/VERIFICATION.md). No package was rebuilt or repacked for publication.

## Remaining limitations and findings

- Execution inside Windows AIMP, actual focused/group UI interaction, reload/unload, reconnect, end-to-end Companion control, and the wider audio/output/recording/dialog/library behavior still need real installation testing. Mock hosts and SDK doubles do not establish Windows runtime acceptance.
- The separate **visualisation Start** runtime report remains unresolved. These versions do not change that behavior.
- Explicit targets still use cached GUID-to-numeric-index resolution. External reordering between inventory and execution remains a race; the native protocol has no atomic expected-GUID precondition. Refresh inventory and avoid concurrent reordering during cues.
- Microsoft Defender scanning and publisher signing were not performed. DLLs are unsigned. No Defender clearance or antivirus acceptance is claimed. AIMP 6 beta is not validated.
- LeakSanitizer was disabled in the supplied native checks because of process-inspection restrictions. PowerShell diagnostics were not run against AIMP.
- A rare failed SDK cancellation or reentrant unload may retain a module reference until AIMP exits; fully close/restart AIMP before replacing the plugin.
- The WebSocket service has no TLS or token authentication and is intended for a trusted local network with the configured IPv4 allowlist. Browser-origin connections are rejected; that is not authentication.
- Focused removal means one focused track, not every highlighted row. Read-only protection remains. Empty queue operations may have no visible effect. Async acknowledgements mean accepted/queued, not completed. Provider, format and skin support vary; the README and protocol contracts describe those limits.

## Original package identities

```text
85d05860db83edce61053d3bf012bd15bcc454ec10bd29622716d8ab6bd3f33e  AimpCompanionControl-0.3.1-preview.aimppack
6bb1e2d6cc6c0c5d789e12720f2b5768cd21526fab6f6260194c76d9de9a614e  ninoleto-aimp-1.0.0-beta.8.tgz
```

Keep the previously working native 0.2.1 / module beta.3 pair and a Companion configuration backup for rollback. Installation and rollback steps are in each README and prerelease.
