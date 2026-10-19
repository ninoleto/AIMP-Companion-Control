# AIMP Companion Control — 0.3.1 preview

Native Windows plugin for **ninoleto: AIMP Companion Control**, with Companion module **1.0.0-beta.8**. Includes ready-to-install x86 and x64 binaries. Targets the stable AIMP 5.40 SDK and preserves the original connection identity, protocol, settings, and existing control actions.

Version 0.3.1 corrects focused-target safety in the 0.3.0 feature set. Extended track actions read AIMP’s actual focused track and return `NO_FOCUSED_TRACK` when no track is focused, instead of falling back to the first track. Extended focused-playlist actions require a current active playlist. Legacy navigation remains unchanged. Source checks and SDK-double tests do not establish execution inside Windows AIMP.

## Install the update

1. Keep your previous working plugin/module packages. Save a Companion configuration backup.
2. Disable the AIMP connection in Companion and fully close AIMP.
3. Download **`AimpCompanionControl-0.3.1-preview.aimppack`**, `SHA256SUMS.txt` and `THIRD-PARTY-NOTICES.txt` from the [0.3.1 preview release](https://github.com/ninoleto/AIMP-Companion-Control/releases/tag/v0.3.1-preview). Verify the installer with `Get-FileHash .\AimpCompanionControl-0.3.1-preview.aimppack -Algorithm SHA256`, retain the notices, then open the installer and restart AIMP. Confirm plugin version 0.3.1.
4. Download **`ninoleto-aimp-1.0.0-beta.8.tgz`** from the [matching module release](https://github.com/ninoleto/companion-module-ninoleto-aimp/releases/tag/v1.0.0-beta.8) and verify its release checksum. In Companion, import it without extracting it. Select beta.8 on the existing connection and enable it. Keep the working host and port.
5. Existing buttons remain. New presets become available after capability discovery; a previously copied button is not automatically redesigned.

The AIMPPACK goes into AIMP; the TGZ goes into Companion. No compilation is required. Both architectures are included, and the x86 EQ getter workaround from 0.2.1 is retained.

## Controls

| Area | Available controls |
| --- | --- |
| Playback | Play/pause/stop, next/previous, volume/mute, seeking, shuffle/repeat, focus navigation, direct file/URL playback, A/B loop, end-of-track/playlist behavior |
| Audio | Pitch, tempo, speed, balance, chorus, echo, flanger, reverb, enhancer, bass, EQ enable/auto/preamp/all reported bands, reset, preset apply/save/rename/delete |
| Transitions/output | Automatic/manual fades and cross-mixing, inter-track pause, reported output-device selection, internet-radio capture and single-track mode |
| Artwork/lyrics | Cached album artwork for Companion image feedback; plain/timed lyrics with current, previous and next lines and track identity guards |
| Playlists/tracks | Create/import/rename/close, add/sort/clear, ratings and automatic-play flags, move/copy between playlists, selection, group expansion/selection/merging, targeted rescans, allowlisted view/format/grouping properties |
| Smart-playlist sources | Inspect/create folder or playlist sources, configure source options, request synchronization; individual factories/providers determine availability |
| Queue | Inspect/add/remove/reorder/clear/suspend and resume |
| Music library | Discover storage providers and fields, typed search/filter/sort, asynchronous bounded queries, page/cursor browsing, cached-result playback and playlist/queue actions; see the provider contract for additional storage operations and requirements |
| Tags | Read/edit 29 documented metadata fields through AIMP's tag editor, including ReplayGain tag values; file writes require confirmation and matching file identity; merged tags can normalize differences between formats |
| Application | Existing dialogs/bookmarks/visualizations, playlist cleanup, notifications, window commands, registered-action lookup/execution by exact ID, close request, loaded/buffering state |
| Skins | Installed skin list and metadata, select/default/reapply, hue and intensity with relative adjustment |

Presets retain Companion 5 native styling, size 20 and shrink-to-fit text, consistent vector icons, and actual rotary events. Capability discovery hides unsupported actions instead of inventing successful results.

## Scope and limits

This is a remote control plugin, not a replacement AIMP player, converter, tag editor, DSP host, or plugin installer. The SDK also has interfaces for writing new decoders, visualizers, providers and custom UI; those are not remote-control commands.

The audited stable SDK has no documented direct setters for voice removal, playback normalization/ReplayGain configuration, silence-removal settings, arbitrary third-party DSP/VST parameters, scheduler jobs, converter jobs, or live output sample-rate changes. Opening the relevant dialog is distinct from controlling all its settings. `app/action/get` can check an exact known action ID; the SDK does not enumerate every built-in action ID. No private INI rewriting, guessed IDs, memory patching, or keyboard injection is used.

Physical media deletion and installing/uninstalling AIMP add-ons are outside this bridge's operating-control scope. Playlist removals affect entries. Tag writes really modify files, so the corresponding action defaults its confirmation option to off. EQ preset deletion is explicit: the SDK has no built-in/user marker. Saving a preset stores band gains, not preamp/enabled state.

Pitch range is −10 to +10 in unspecified AIMP pitch units; tempo 0.80–1.50×, speed 0.50–1.50×, EQ gains −15 to +15 dB. Enhancer uses 1–4 because the SDK header/help disagree on the maximum. Provider capabilities, stream type, file format and skin layout can limit individual operations. AIMP 6 is not a validated target.

Detailed evidence and wire contracts are in `docs/CAPABILITY-MATRIX-0.3.0.md` and the other `docs/*CONTRACT*` / `docs/*COMMANDS*` / `docs/*PROTOCOL*` files. Asynchronous acknowledgements mean accepted or queued, not that playback, file scanning or UI changes have completed.

## Connection settings

Keep your working settings when updating. Fresh installations default to **127.0.0.1:19122**, with a loopback-only allowlist.

| Where Companion runs | AIMP listen interface / allowlist | Companion host |
|---|---|---|
| Same Windows computer as AIMP | Localhost; allow `127.0.0.1` | `127.0.0.1` |
| Another computer or separate container | AIMP computer's LAN interface; allow the Companion backend's source IPv4 address | AIMP computer's LAN IPv4 address |

The browser's address does not identify where the Companion backend runs. AIMP's **Companion Control (preview)** settings show whether the listener started. A bind failure usually means another process already owns the selected port.

This is a WebSocket-only endpoint for a trusted local network, without TLS or token authentication. The plugin does not alter firewall settings. Browser-origin WebSocket connections and old HTTP modules are not supported.

Optional read-only check, from this source folder in PowerShell:

```powershell
.\scripts\Test-Connection.ps1 -Address 127.0.0.1 -Port 19122
.\scripts\Test-Features.ps1 -Address 127.0.0.1 -Port 19122
```

Use the AIMP computer's LAN address when running that check elsewhere. It reads status and playlists without starting playback or changing settings.

## Verification and rollback

See `docs/VERIFICATION.txt` for the exact build, portable tests, mock-host checks and limitations. The new code has not run inside Windows AIMP in this environment. Retain the working pair until you have tested the new functions on your machine.

First check playback and the existing rotary controls, then artwork/lyrics, library browsing, reconnect, and AIMP shutdown. Test playlist changes on a disposable playlist and tag writing on copied media files. In rare failed SDK cancellation or reentrant unload cases, a module reference is retained until AIMP exits to prevent callbacks into unloaded code; fully restart AIMP before replacing the DLL.

To roll back, fully close AIMP, reinstall your retained working native package, then select its working Companion module on the existing connection. Restore your saved Companion configuration if you need to undo added buttons. Host, port and the INI settings file are unchanged.

The DLLs are ordinary unsigned builds without executable packing, obfuscation, an updater or background service. Defender acceptance is not guaranteed. Preserve the detection name and file hash if a build is flagged; do not disable protection.

Source provenance and licences: `UPSTREAM.txt`, `LICENSE`, SDK notices and `third_party/`. Build instructions: `BUILD.md`. Publication checks and remaining findings: [docs/FINAL-REVIEW.md](docs/FINAL-REVIEW.md). Tested packages are release assets; build output is excluded from Git.
