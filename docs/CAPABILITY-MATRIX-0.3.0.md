# AIMP 0.3.0 capability audit

Audit date: 19 October 2026. Target: the documented AIMP 5.40 SDK. This matrix describes the bridge's source and documented SDK boundaries; it is not a Windows runtime certification. Runtime capability results remain authoritative for the installed AIMP version, output device, media type, and optional provider.

The existing 0.2.1 release is a separate rollback copy. The 0.3.0 changes extend the established player protocol rather than replacing its transport controls.

## Sources

- Official SDK distribution: https://aimp.ru/?cat=sdk&do=download&os=desktop
- The downloaded SDK's `Sources/Cpp` and `Sources/Delphi` declarations were compared for newly used interfaces. Primary behavioral evidence is its extracted English and Russian CHM reference.
- Research copy: `aimp-sdk-official/v5.40/Sources`, `aimp-sdk-official/help-en`, and `aimp-sdk-official/help-ru` in the development workspace. The page filenames below identify the primary evidence precisely.
- `CAPABILITIES-RESEARCH.md` records the earlier audit, numeric range discrepancies, and the 0.2.1 equalizer ABI correction.

## Remote controls implemented in this release

| Family | Bridge coverage | Primary SDK evidence and limits |
|---|---|---|
| Transport and status | Play, pause, resume, stop, next/previous, file/URL playback, position/duration, volume/mute, current/focused track and playlist, loaded/buffering state | `IAIMPServicePlayer.html`, `apiPlayer.h`, `AIMP_MSG_PROPERTY_LOADED.html`, `AIMP_MSG_PROPERTY_PLAYER_BUFFERING.html`. Accepted playback can precede actual playback. |
| DSP and playback tuning | Balance, chorus, echo, enhancer, flanger, reverb, pitch, speed, tempo, bass; absolute values, atomic relative changes, neutral resets | Corresponding `AIMP_MSG_PROPERTY_*.html` and `apiMessages.h`. Pitch units are not specified; enhancer uses the intersection of conflicting published ranges. Unsupported properties are omitted or marked unavailable. |
| Equalizer | On/off/toggle, automatic preset selection, dynamic band count/frequencies, band gains, preamp, flatten/reset, preset list/apply/save/rename/delete | `IAIMPEqualizerBands.html`, `IAIMPEqualizerPreset.html`, `IAIMPServicePlayerEqualizer.html`, `IAIMPServicePlayerEqualizerPresets.html`. Gains/preamp are −15…+15 dB. Presets contain bands and name; preset preamp storage is not documented. |
| Playback modes | Shuffle, repeat, auto-jump, single-track playlist repeat, end-of-track/end-of-playlist behavior, cross-mixing | `apiMessages.h` and individual property pages. Read actual state after changes. |
| Transition timing | Automatic/manual transition enable, automatic/manual crossfade/fade-in/fade-out, automatic pause between tracks | `IAIMPServicePlayer.html` property list. Timing values are integer milliseconds; zero disables an effect. |
| A–B repeat | Read stage/range, advance A/B stage, set A/B/clear | `AIMP_MSG_PROPERTY_PARTREPEAT.html`, `AIMP_MSG_PROPERTY_PARTREPEAT_RANGE.html`. Ranges are seconds, not milliseconds. |
| Output and radio capture | Enumerate/select available output devices; recording and single-track recording switches | `IAIMPServicePlayer.html`, `AIMP_MSG_PROPERTY_RADIOCAP.html`, `AIMP_MSG_PROPERTY_RADIOCAP_SINGLE_TRACK.html`. Recording follows AIMP's existing stream-recording configuration. |
| Playlist inventory and editing | Enumerate/browse/play/resume/select/create/import/rename/close, add files/URLs, clear/remove entries, sort/shuffle/reverse, sort by template, refresh tags | `IAIMPPlaylist.html`, `IAIMPServicePlaylistManager.html`, `apiPlaylists.h`. Entry removal uses no physical-file-delete flag. Asynchronous additions report acceptance. |
| Track and playlist organization | Track enabled/selected/rating/move/copy; bulk selection; playlist group inspection/rename/merge; documented grouping/formatting/view properties and indices | `IAIMPPlaylistItem.html`, `IAIMPPlaylistGroup.html`, `IAIMPPlaylistProperties.html`. Playlist rating differs from file-tag rating. |
| Playlist sources | Inspect/create/update/synchronize documented folder and playlist-file/URL preimages | `IAIMPPlaylistPreimage.html`, `IAIMPPlaylistPreimageFolders.html`, `IAIMPServicePlaylistManager2.html`. Available factories are discovered at runtime. |
| Active-playlist cleanup | Remove missing, unselected, selected, disabled, or duplicate entries; synchronize with source | `AIMP_MSG_CMD_PLS_DELETE_NON_EXISTS`, `...NON_SELECTED`, `...SELECTED`, `...SWITCHEDOFF`, `...DUPLICATES`, `...RELOAD_FROM_PREIMAGE`. Confirmation required; no disk-deletion command is used. |
| Playback queue | Read/add/remove/clear/move, suspend/resume | `IAIMPPlaylistQueue.html`, `IAIMPPlaylistQueue2.html`. This edits AIMP's explicit queue; it does not replace its internal next-track selection engine. |
| Metadata and file tags | Player/track metadata; enumerated editable tag fields on a selected existing local/network-share file | `IAIMPFileInfo.html`, `IAIMPServiceFileTagEditor.html`, `IAIMPFileTagEditor.html`. Writes require confirmation and expected file identity; URLs/virtual streams are rejected. |
| Album artwork | Async retrieval, bounded resized PNG result, per-track cache/state and explicit absence/errors | `IAIMPServiceAlbumArt.html`, `TAIMPServiceAlbumArtReceiveProc.html`, `IAIMPImageContainer.html`, `IAIMPImage.html`. Provider availability and track artwork determine results. |
| Lyrics | Async plain/synchronized lyrics, current/previous/next line, bounded text/range output | `IAIMPServiceLyrics.html`, `TAIMPServiceLyricsReceiveProc.html`, `IAIMPLyrics.html`. Timing is milliseconds. Provider results may be absent. |
| Music library | Storage discovery, storage/schema selection, bounded asynchronous queries with search/filter/sort, grouping focus path, cached-row play/add, existing-playlist-row queue operations; optional add-files, tag refresh, rating, confirmed single-row library removal, add/report dialogs | `IAIMPServiceMusicLibrary.html`, `IAIMPMLDataProvider.html`, `IAIMPMLDataProvider2.html`, `IAIMPMLDataStorageCommand*.html`, `apiMusicLibrary.h`. Each storage command is capability-gated; deferred writes report operation status. Library removal explicitly passes `Physically=FALSE`. |
| UI and windows | Documented settings/About/plugins/scheduler/DSP dialogs, file/folder/playlist dialogs, search, quick info/editor, bookmarks, known player windows, notification display/clear, application quit | `apiMessages.h`, `AIMP_MSG_PROPERTY_HWND.html`. UI commands are acknowledged before modal execution. Window operations are restricted to AIMP's own process. |
| Visualizations | Start/stop, next/previous, fullscreen | Documented `AIMP_MSG_CMD_VISUAL_*` and `AIMP_MSG_PROPERTY_VISUAL_FULLSCREEN`. The visualizations service itself has no methods. |
| Installed skins | List installed names and title/author/description; current skin; select installed/default skin; reapply; hue/intensity get/set/atomic adjust | `IAIMPServiceSkinsManager.html`, `IAIMPSkinInfo.html`, `apiSkins.h`. Hue 0…255, intensity 0…100; reapply after color changes. A hue/intensity value of zero means the override is not set. |
| Known action IDs | Inspect/execute an explicitly supplied ID when AIMP resolves and enables it | `IAIMPServiceActionManager.html`, `IAIMPAction.html`, `IAIMPActionEvent.html`. The SDK does not expose enumeration or a stable complete catalog of IDs. |

## Exact new management contracts

All requests retain the existing `action` envelope and ordinary error handling.

| Request | Required fields | Result and safeguards |
|---|---|---|
| `equalizer/preset/rename` | `name`, `new_name`, `confirm:true` | Exact existing name; valid new name; case-insensitive destination uniqueness; no overwrite. Returns updated EQ inventory and `renamed_preset`. |
| `equalizer/preset/delete` | `name`, `confirm:true` | Exact existing name; rejects ambiguous duplicates; returns updated inventory and `deleted_preset`. |
| `app/command` | `command`, `confirm:true` for cleanup/source sync | `cleanup_missing`, `cleanup_non_selected`, `cleanup_selected`, `cleanup_disabled`, `cleanup_duplicates`, `sync_preimage`. Captures the active playlist GUID and refuses to run if the active playlist changes before queued execution. |
| `skins/get` | None | Refreshes installed inventory and reads actual current skin/color. |
| `skins/set` | `name` | Exact installed filename; empty string selects AIMP's default. Arbitrary paths are rejected. |
| `skins/reapply` | None | Reapplies the current skin. |
| `skins/color/set` | `parameter`, `value` | Parameter `hue` or `intensity`, integer value within the published range. |
| `skins/color/adjust` | `parameter`, `delta` | Integer relative amount, native read/adjust/clamp/write/reapply/read. |

The equalizer SDK does **not** identify factory presets versus user-created presets. Rename/delete therefore require explicit confirmation for the selected name and can affect a factory preset. The bridge does not silently choose a preset, overwrite an existing name, or claim a deleted preset can always be restored. `preset_builtin_distinction:false` makes this limitation explicit. Preset readback failures are reported even after a successful SDK mutation.

Cleanup removes playlist references, never underlying audio files. `sync_preimage` can change playlist membership to match its configured source. The targeted `playlists/source/sync` operation is the preferred normal preset; the active-playlist command is an advanced alternative.

## Features without a documented direct stable-SDK control

| Requested capability | Finding | Available route |
|---|---|---|
| Vocal/voice removal | No direct property or dedicated control interface found in the stable headers or property reference | Open the DSP manager; use a known enabled action ID only if the installation actually resolves one. |
| ReplayGain/normalization configuration | No direct player configuration API found for normalization mode, target level, or related processing switches | Open the documented normalization DSP tab. Tag metadata is not a substitute for processing configuration. |
| Silence-removal settings | No documented direct controls found | Configure in AIMP. No private INI keys are guessed or changed. |
| Arbitrary third-party DSP/VST parameters and bypass | A DSP/VST change notification exists, but no universal host parameter or bypass service is documented | Use the plug-in's own controls or a separately documented integration. |
| Scheduler job CRUD and sleep/shutdown schedules | SDK provides a scheduler settings dialog command, not a documented general scheduler job API | Open scheduler settings. Application quit is separate from scheduling or PC shutdown. |
| Audio Converter job control | Stable SDK contains extension services usable by converter plug-ins, but no documented remote converter queue/job service | Use AIMP's converter UI; no guessed action IDs or process automation are built in. |
| Universal complete action/shortcut inventory | Lookup by known ID exists; enumeration and stable ID catalog do not | Inspect/execute explicitly supplied IDs and report unavailable/disabled IDs accurately. |
| Waveform export through `IAIMPServiceWaveform` | The service is an empty registration anchor for custom waveform providers | A provider API is not a method to read AIMP's current waveform. |
| Visualization inventory through `IAIMPServiceVisualizations` | The service is an empty registration anchor for custom visualizations | Documented next/previous/start/stop/fullscreen messages are implemented. |
| Universal bookmark CRUD | Bookmark display/add commands exist; no single general bookmark CRUD service is documented | Use bookmark UI commands or provider-specific music-library functionality where offered. |

## Documented SDK facilities deliberately outside remote player operation

- Skin installation/uninstallation, plug-in installation, and replacing decoders/encoders or provider implementations are deployment/extension operations. Installed skin selection is implemented; installation/deletion is not.
- Physical media-file deletion, including playlist `FROM_HDD` commands, remains unexposed. It is distinct from playlist cleanup and tag editing.
- Custom GUI construction, menu registration, localization, HTTP/network client helpers, thread pools, image/color utilities, and generic object/stream/config services support plug-in implementations. They do not imply new user-facing player controls.
- Raw database/provider administration, unbounded filtered deletion, clearing/deleting storage databases, arbitrary filesystem access, arbitrary undocumented configuration mutation, and arbitrary code execution are not exposed as remote commands. Supported single-row library removal is distinct from physical file deletion and whole-database clearing.
- Artwork/lyrics/tag providers may offer additional specialized editing or search interfaces. This release fetches playback artwork/lyrics and supports the explicitly advertised tag fields; it does not promise a universal online tagging/editor workflow.

## ABI and validation notes

- The 0.2.1 workaround for `IAIMPServicePlayerEqualizer::GetActive` remains: read `AIMP_MSG_PROPERTY_EQUALIZER` through the dispatcher using a 32-bit `BOOL`.
- `IAIMPEqualizerPreset::SetName`, preset-library `Delete`, and all skin-manager methods used here match their Delphi `stdcall` declarations and method order.
- Official C++ `IAIMPImage::Clone` omits `WINAPI` while Delphi declares `stdcall`; media handling avoids that call. Callback inputs are borrowed references; retain with `AddRef` when needed.
- Album-art and lyrics callbacks are documented main-thread callbacks. Shutdown cancels requests with the documented wait flag and retains code/context lifetime if cancellation cannot be proven complete.
- C++ compilation for x86/x64 and portable mock/validation tests establish source/protocol checks. They do not establish Windows AIMP runtime behavior, audible DSP results, plug-in-specific behavior, or antivirus acceptance. Consult the release verification record for tests actually run.
