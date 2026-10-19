# AIMP 5.40 control capability research

Research date: 18 October 2026. This is an SDK capability audit, not evidence of Windows runtime testing or an assertion that every capability below is implemented in this bridge.

## Sources and target

- Official Windows download page: https://aimp.ru/?do=download&os=windows — stable AIMP 5.40.2727, released 8 September 2026. AIMP 6.00.3090 Beta 7 is a separate beta target.
- Official SDK page: https://aimp.ru/?cat=sdk&do=download&os=desktop — separate 5.40 and 6.00 Beta 7 SDK downloads.
- Official stable SDK archive: https://aimp.ru/?do=download.file&id=15 — downloaded and verified against the SHA-256 displayed on the official page: `aa449fed93b93af7880a5580d71265231d048115e837b8d79f0c32aec59537a7`.
- Primary technical evidence is the English and Russian CHM documentation and C++ headers inside that archive. The CHM's individual page names below make each finding reproducible.
- Official AIMP 5.40 portable package: https://aimp.ru/?do=download.file&id=31 — its bundled English application help and `Langs/english.lng` were inspected to distinguish GUI features from SDK controls.

Working research copies are under `aimp-sdk-official/`: `v5.40/Sources/Cpp`, extracted `help-en`, extracted `help-ru`, and `app-help-en`. They are not required to run the bridge.

The existing plugin headers target **v5.40 build 2650**. All 23 header files were compared byte-for-byte with the official stable SDK: 22 match, and only `apiFileManager.h` differs. Its changes concern file-system interface identifiers and the `IAIMPVirtualFile::IsExists` return type; they do not add the missing player/DSP controls. Avoid wholesale SDK replacement just to implement controls already present in the existing headers.

## Player and sound controls

`IAIMPServicePlayer` supplies play, pause, resume, stop, next/previous, seek, duration, position, mute, volume, playing-file metadata, and the current playlist item. It can play a playlist item, playlist, playback-queue item, or file/URL. Play requests schedule a load: a successful HRESULT does **not** prove audible playback has begun. Confirm subsequent player state/stream-start events.

The message dispatcher exposes the following sound properties as 32-bit floats. Use property GET/SET flags with a pointer to a correctly sized float. These properties are main-thread only despite the dispatcher's broader thread-support declaration.

| Control | SDK range | Neutral value | Evidence / presentation |
|---|---:|---:|---|
| Volume | 0–1 | — | `AIMP_MSG_PROPERTY_VOLUME.html`; present as 0–100% |
| Balance | −1–1 | 0 | `AIMP_MSG_PROPERTY_BALANCE.html` |
| Pitch | −10–10 | 0 | `AIMP_MSG_PROPERTY_PITCH.html`; neither English nor Russian help states the unit, so label as AIMP pitch units unless verified on Windows |
| Speed | 0.5–1.5 | 1 | `AIMP_MSG_PROPERTY_SPEED.html`; explicitly 50–150% |
| Tempo | 0.8–1.5 | 1 | `AIMP_MSG_PROPERTY_TEMPO.html`; explicitly 80–150% |
| Chorus | 0–1 | 0 | `AIMP_MSG_PROPERTY_CHORUS.html` |
| Echo | 0–1 | 0 | `AIMP_MSG_PROPERTY_ECHO.html` |
| Flanger | 0–1 | 0 | `AIMP_MSG_PROPERTY_FLANGER.html` |
| Reverb | 0–1 | 0 | `AIMP_MSG_PROPERTY_REVERB.html` |
| Stereo enhancer | Header: 1–4; CHM: 1–5 | 1 | `AIMP_MSG_PROPERTY_ENHANCER.html`; conservative bridge cap 4 until runtime verification resolves conflict |
| Bass / TrueBass | 0–2 | 0 | `AIMP_MSG_PROPERTY_TRUEBASS.html` |

Pitch, speed, and tempo are distinct AIMP properties. Do not implement one by changing another or assume percentage and pitch units are interchangeable.

### Equalizer

`IAIMPServicePlayerEqualizer` has active state, preamp, current preset, and preset activation. `IAIMPEqualizerBands` supplies the actual band count, each band's frequency in Hz, and gain. `IAIMPServicePlayerEqualizerPresets` can enumerate, find, create, and delete presets. EQ auto-selection is available through `AIMP_MSG_PROPERTY_EQUALIZER_AUTO`.

**Preamp and band gains are −15 to +15 dB.** Use the service's `double` values and dynamic band count. Both `IAIMPServicePlayerEqualizer.html` and `IAIMPEqualizerBands.html` state these ranges. The header comment for the older `AIMP_MSG_PROPERTY_PREAMP` incorrectly says 0.5–1.5; its current CHM page instead states −15–15 dB, neutral 0, and says the equalizer must be enabled. This discrepancy must not become an incorrect percentage control.

### Playback modes and loops

Documented read/write properties include shuffle, current-track repeat, repeating one-track playlists, automatic jump to next track, cross-mixing, end-of-playlist action, and end-of-track action. End-of-playlist values are 0 = next playlist, 1 = repeat playlist, 2 = do nothing. End-of-track values are 0 = default, 1 = next track then stop, 2 = next track then pause.

The legacy player property header calls ID 1 `STOP_AFTER_TRACK`, while current help calls it `ACTION_ON_END_OF_TRACK`. Prefer the unambiguous `AIMP_MSG_PROPERTY_ACTION_ON_END_OF_TRACK` message for all three modes.

Part-repeat status is read-only (0 = disabled, 1 = A assigned, 2 = B assigned/repeating). `AIMP_MSG_CMD_TOGGLE_PARTREPEAT` advances the A/B operation. `AIMP_MSG_PROPERTY_PARTREPEAT_RANGE` reads/writes an array of **two 32-bit floats in seconds**, with −1 for an unassigned endpoint. This supports explicit A, B, and clear operations; distinguish an unsupported/failed read from a real disabled state.

### Transition timings and output devices

`IAIMPServicePlayer.html` documents `IAIMPPropertyList` access through the player service:

- Automatic and manual switching are separate integer switches. Zero disables all transition effects in that group.
- Automatic crossfade, fade-in, fade-out, and inter-track pause are integer milliseconds; zero disables the respective effect.
- Manual crossfade, fade-in, and fade-out are integer milliseconds; zero disables the respective effect.
- The SDK does not document an upper timing bound. Any bridge maximum is a bridge validation limit, not an AIMP guarantee.
- Property `AIMP_PLAYER_PROPID_OUTPUT` queried as `IAIMPString` gives the selected output device; setting the string changes it. Querying the same property as `IAIMPObjectList` gives the available devices. Preserve the native device strings rather than guessing their format.

Internet-radio recording exposes a read/write capture switch and a separate single-track-only switch. This records the internet stream according to AIMP's own recording settings; it is not a general microphone or system-audio recorder. Evidence: `AIMP_MSG_PROPERTY_RADIOCAP.html`, `AIMP_MSG_PROPERTY_RADIOCAP_SINGLE_TRACK.html`.

## Playlists, tracks, queue, ratings, and bookmarks

`IAIMPPlaylist` and `IAIMPServicePlaylistManager` support enumerating, activating, creating/loading, closing, renaming, and inspecting playlists; adding files/URLs, removing playlist entries, sorting, reversing, shuffling, rescanning information, and synchronizing a preimage. Playlist properties expose name, group, read-only state, focused item, playback cursor, playing index, total bytes/duration, and approximate remaining duration. Additional presentation properties govern grouping, formatting, and playlist-view options.

Important implementation details from `IAIMPPlaylist.html` and `IAIMPPlaylistItem.html`:

- Add/AddList normally initiate asynchronous processing. The CHM calls flag 4 `NOASYNC`, whereas the header calls it `NOTHREADING`; the documentation discourages it because it blocks the main thread. Do not claim an asynchronously accepted addition has already appeared in the list.
- Item index is a writable absolute integer position. Setting it can move an item.
- Item mark is a writable **double from 0 to 5**. This is the playlist/user mark; do not confuse it with the file-tag rating or calculated music-library rating. The file-info `ML_MARK` header comment also incorrectly says Int32: `IAIMPFileInfo.html` declares a double and requires Get/SetValueAsFloat; `ML_DISPLAYING_MARK` is likewise a double.
- `PLAYINGSWITCH` is the automatic-play switch: **0 = disabled; any nonzero = enabled**, as explicitly documented in the Russian help (the English page omits the description).
- Selected state is a writable integer; nonzero means selected.
- `Close(FORCE_UNLOAD)` overrides user settings and retains the saved playlist file. `Close(FORCE_REMOVE)` removes it from disk.
- Ordinary playlist entry removal and physical file deletion are distinct operations. The SDK supplies physical-delete flags and commands, but a remote control need not expose those to deliver ordinary playlist editing.
- Player `GetInfo` returns a global file-info object: **do not mutate it**. Clone before editing. Playlist-item file info and tag-writing services are separate concepts.

`IAIMPPlaylistQueue` supports adding items at the beginning/end, removing items or a playlist's entries, moving items, reading the queue, and pausing/resuming the queue via its `SUSPENDED` property. It is distinct from the player's internal automatic next-track selection. `IAIMPPlaylistQueue2` adds listeners. Do not replace AIMP's internal playback queue merely to expose queue controls.

The documented message commands can open Bookmarks and add the currently playing file or the selected playlist files to Bookmarks. Additional file/position bookmark navigation exists in the application UI, but the stable SDK has no simple universal bookmark-list CRUD service analogous to the playlist queue. Music-library APIs expose more specialized data-storage capabilities; those require a separate design and cannot be assumed to operate on all configured libraries.

## Application UI and action commands

The SDK provides specific commands for options, plugin settings, scheduler settings, About, DSP manager, file/folder/playlist dialogs, Add URL, quick file info, quick tag editor, advanced search, bookmarks, visualization start/stop/next/previous, and playlist operations. Read/write properties include stay-on-top, minimizing to tray, reverse time display, and visualization fullscreen. Known application window handles can also be queried.

The SDK documents DSP-manager tabs 0 = common, 1 = equalizer, 2 = normalization, 3 = cross-mixing. The current application's localization shows additional DSP/VST and silence-removal pages. Do not invent numeric tab indexes for those newer pages.

`IAIMPServiceActionManager` only documents lookup by a known ID and hotkey encoding. It does not document enumeration of all built-in actions or a stable catalog of their IDs. Localization names such as `acMainDSPManagerDSP` prove those UI actions exist, but are not by themselves proof that `GetByID` accepts the same identifier. An optional advanced lookup must report unavailable IDs rather than pretending every shortcut is supported.

Message dispatch only has synchronous `Send`, not a documented asynchronous `Post`. Modal UI actions must not keep a WebSocket command waiting indefinitely. Queue the UI operation on the existing UI thread after the protocol acknowledgement; do not move unsupported SDK calls to a worker thread.

## What cannot honestly be advertised as universal remote control

The SDK is an extension SDK: it includes facilities to build decoders, encoders, visualizations, tag editors, music-library providers, artwork/lyrics providers, and custom UI. Their presence does not make every AIMP setting remotely controllable through a simple command.

This audit did **not** find documented direct stable-SDK player properties for arbitrary third-party DSP/VST parameters or bypass, vocal removal, ReplayGain normalization configuration, silence-removal parameters, all scheduler settings, converter job control, or every music-library/database operation. Some can be opened in AIMP's dialogs; metadata read/write and music-library service APIs exist but are separate workflows. Do not scrape undocumented configuration keys or claim these are implemented because a settings dialog can be opened.

AIMP 6 beta is not the stable compatibility target and needs its own runtime validation. A feature can be supported by the 5.40 SDK and still be unavailable for a particular track, stream, output backend, or installed component. Report native HRESULT failures and discover capabilities at runtime; never substitute a fabricated success value.

## Validation boundary

The audit verifies official documentation, constants, signatures, ranges, and the source-level compatibility target. It does not verify sound, UI modal behavior, device changes, capture, SDK events, or ABI behavior in an actual Windows AIMP process. Preserve a working plugin/module pair and perform a Windows acceptance pass before calling a broader build production-ready.


## 0.2.1 correction: EQ enabled-state ABI

The official C++ and Delphi SDK declarations disagree for `IAIMPServicePlayerEqualizer.GetActive`: C++ uses `WINAPI`, while Delphi omits `stdcall` on this method. The Delphi default register convention is incompatible with the C++ x86 call. Other methods in this interface explicitly use `stdcall`. This was missed in the initial audit.

Version 0.2.1 avoids `GetActive` entirely. Both EQ state polling and EQ toggle use `IAIMPServiceMessageDispatcher.Send` with `AIMP_MSG_PROPERTY_EQUALIZER`, `AIMP_MSG_PROPVALUE_GET`, and a `BOOL*`. The documented payload is a 32-bit Boolean. Failed reads remain errors; toggle does not change state after a failed read. See `CRASH-FIX-0.2.1.md` for the crash evidence and verification boundary.
