# AIMP Companion Control 0.3.1 preview

Pair with [Companion module 1.0.0-beta.8](https://github.com/ninoleto/companion-module-ninoleto-aimp/releases/tag/v1.0.0-beta.8). Targets stable AIMP 5.40; includes x86 and x64 DLLs. This is an independent preview fork.

Extended focused-track actions now use AIMP's actual active playlist and focus index. Missing/invalid/group-only focus returns an error before reading or changing a track. Existing controls and legacy navigation are retained.

The attached installer is the original supplied, tested package, unchanged. Source publication adds documentation and licence material; it does not rebuild or repack the installer. Download and retain `THIRD-PARTY-NOTICES.txt` with it. Source, tests, SDK headers, vendored JSON header and licences are in this repository; generated output is excluded from Git.

## Install

1. Keep the previously working native 0.2.1 / module beta.3 packages and export a Companion configuration backup.
2. Download the AIMPPACK, `SHA256SUMS.txt` and `THIRD-PARTY-NOTICES.txt`. Check `Get-FileHash .\AimpCompanionControl-0.3.1-preview.aimppack -Algorithm SHA256` against the digest below and the checksum file.
3. Disable the AIMP connection in Companion and fully close AIMP. Open the AIMPPACK, complete installation, restart AIMP and confirm version 0.3.1.
4. Download the matching beta.8 TGZ from the linked module release and verify its checksum. In Companion 5, use **Modules → Import module package** and select the TGZ without extracting it. Select beta.8 on the existing connection and enable it; retain the working label, host, port and allowlist.
5. Test existing playback/rotaries, focus behavior, media/library controls, reconnect and shutdown. Use disposable playlists and copied media for mutation/tag tests. New preset captions require newly dragged presets; copied artwork is preserved.

```text
85d05860db83edce61053d3bf012bd15bcc454ec10bd29622716d8ab6bd3f33e  AimpCompanionControl-0.3.1-preview.aimppack
```

The AIMPPACK is for AIMP; the TGZ is for Companion. Fresh plugin settings use `127.0.0.1:19122`; existing installations should retain their working endpoint.

## Verification and known limits

Publication verified all 109 native archive file digests, installer members, versioned package hashes and the vendored JSON hash. The paired module's 102 tests, official checker and actual-host smoke against the unchanged TGZ passed (192 actions, 46 feedbacks, 503 presets, 624 variables, zero unexpected warnings). See [final review](https://github.com/ninoleto/AIMP-Companion-Control/blob/v0.3.1-preview/docs/FINAL-REVIEW.md) and [supplied native verification](https://github.com/ninoleto/AIMP-Companion-Control/blob/v0.3.1-preview/docs/VERIFICATION.txt).

The supplied native report records portable ASan/UBSan tests, SDK-double tests and pinned x86/x64 cross-builds. Those native tests/builds were not rerun during publication because this machine has no WSL/suitable C++ toolchain. Leak detection was disabled in the supplied native tests. PowerShell scripts were parsed but not run against AIMP.

- Complete Windows AIMP validation is outstanding, including actual focus/group interaction, SDK behavior, reload/unload, reconnect and end-to-end controls. Mock-host/SDK-double results do not establish Windows runtime acceptance. AIMP 6 beta is unvalidated.
- The separate **visualisation Start** runtime report remains unresolved.
- Explicit numeric targets remain subject to stale inventory/external reorder races; no atomic expected-GUID precondition exists. Refresh inventory and avoid concurrent reordering during cues.
- DLLs are unsigned. Microsoft Defender scanning and publisher signing were not performed; no Defender clearance is claimed.
- Rare failed SDK cancellation/reentrant unload may retain the plugin until AIMP exits; fully restart before replacing it.
- WebSocket control has no TLS/token authentication; use the intended trusted local network and configured IPv4 allowlist. Focused removal affects one track, empty queues may show no effect, read-only protection remains, and asynchronous acknowledgements mean queued/accepted. Provider/format/skin and SDK capability limits remain documented in the README/contracts.

To roll back, fully close AIMP, reinstall the retained native 0.2.1 package, restart AIMP and select module beta.3 on the existing Companion connection. Restore the Companion backup if needed.
