# Album artwork and lyrics: protocol 0.3.0

This extension reads the currently playing track through AIMP's documented album-art and lyrics services. It also works when playback has no playlist item. It does not modify media files, lyrics, AIMP's search preferences, or the active artwork.

## Capabilities and identity

`capabilities/get` includes `capabilities.media`:

```json
{"artwork":true,"lyrics":true,"synchronized_lyrics":true,"max_artwork_bytes":262144,"max_artwork_dimension":144,"max_lyrics_bytes":65536,"max_lyrics_lines":1000}
```

Availability is detected after AIMP reports that loading is complete. Unsupported services return `false`. Synchronized-lyrics support means the API is available; individual tracks may have plain lyrics or no lyrics.

`media.track_key` is an opaque SHA-256 identity based on the current file URI, title, artist and album. It changes for track changes and changed stream metadata. It is an empty string when stopped. Use this key rather than playlist indices when deciding whether an asynchronous result belongs to the current track.

## Lightweight state

`media/get` and `extensions/get` return a `media` object. Reading state never starts a search and never includes the artwork payload, complete lyric text, or complete line array.

```json
{"media":{"track_key":"...","artwork":{"status":"ready","revision":3,"track_key":"...","mime":"image/png","width":144,"height":144},"lyrics":{"status":"ready","type":"synced","revision":4,"track_key":"...","offset_ms":0,"line_count":20,"truncated":false,"previous":{"index":0,"start_ms":0,"end_ms":999,"text":"Previous line"},"current":{"index":1,"start_ms":1000,"end_ms":1999,"text":"Current line"},"next":{"index":2,"start_ms":2000,"end_ms":2999,"text":"Next line"}}}}
```

Status is `idle`, `pending`, `ready`, `missing`, or `error`. Revisions change when native media state changes. A missing line is `null`. Plain lyrics have `type:"plain"` and no invented current-line timing. `offset_ms` is AIMP's raw offset, or `null` if that property cannot be read. The SDK's `Find` method determines the current synchronized line from playback position; the bridge does not guess how to apply the offset.

An optional media read failure is contained in `media.read_error`; it does not make the whole `extensions/get` response fail. The failure snapshot has an empty key and empty current-line fields, so clients can clear stale media while continuing ordinary controls.

## Fetch commands

| Command | Optional arguments | Response content |
| --- | --- | --- |
| `media/artwork/get` | `expected_track_key`, `refresh` | `media.track_key`, `artwork` |
| `media/lyrics/get` | `expected_track_key`, `refresh`, `include_content` | `media.track_key`, `lyrics` |

The first fetch starts AIMP's asynchronous search. Poll the same command while status is `pending`; 500 ms is sufficient. Searches time out after 30 seconds. A completed `ready`, `missing`, or `error` result remains cached until the track changes or `refresh:true` is requested. Refresh uses AIMP's `NOCACHE` flag and is limited to one request per media kind per second; an early refresh returns `refresh_deferred:true` with the existing state.

Artwork in a `ready` fetch response adds `data_base64` to the metadata. It is a complete PNG, without a data-URL prefix. Aspect ratio is preserved, upscaling is avoided, and neither dimension exceeds 144 pixels. The encoded image is limited to 262144 bytes. Add `data:image/png;base64,` only in a client that requires a data URL.

Lyrics fetches default to `include_content:true`. A completed result adds `text` and `lines`. Each line is `{index,start_ms,end_ms,text}`. Times are milliseconds. `include_content:false` returns only the lightweight metadata and previous/current/next line. Complete text and aggregate line text are each capped at 65536 UTF-8 bytes; at most 1000 lines and 2048 bytes per line are returned. Truncation is indicated by `truncated:true` on the result and/or line. Current-line lookup can still follow lines beyond the exported first 1000 lines.

If `expected_track_key` is present and differs from the current key, the command returns `STALE_MEDIA_TRACK` plus the latest lightweight media state. Fetching while stopped returns `NO_PLAYING_MEDIA`. Invalid arguments and unavailable services produce structured errors. Always compare reply keys with the current key before displaying a payload.

The bridge uses AIMP's configured media providers and normal cache. Depending on the user's AIMP settings, AIMP itself may search online. No separate downloader or arbitrary image/lyrics URL endpoint is added.

## Lifecycle and SDK evidence

All SDK calls and documented callbacks run on AIMP's main thread. Initialization only allocates state; optional services are queried after `AIMP_MSG_PROPERTY_LOADED`. File-info objects are cloned for outstanding requests. Callback objects are borrowed; a synchronized-lyrics object is explicitly retained only while its current track is active.

Track changes, timeouts and finalization cancel outstanding work using the SDK's `WAITFOR` cancellation flag before freeing callback contexts. Contexts also survive synchronous cache callbacks and reentrant submission. If cancellation fails or a callback unexpectedly uses another thread, the affected bounded context and module reference are retained until process exit instead of risking a callback into an unloaded DLL. New requests stop after four such failures.

Artwork serialization uses a size-limited `IAIMPStream`. The prepared callback image is copied through documented stream methods before resizing. When only a compressed container is available, the bridge bounds its size and reported dimensions before creating an image. These limits are resource bounds, not a replacement for AIMP's decoder validation.

The checked official SDK 5.40 C++ and Delphi declarations agree for the album-art, lyrics, string, file-info clone, property, stream and image methods used here. One exception was found and avoided: C++ `IAIMPImage::Clone` omits `WINAPI`, while Delphi declares `stdcall`. This bridge never calls that method. The existing equalizer `GetActive` workaround is preserved.

Primary references are the official SDK's `IAIMPServiceAlbumArt`, `TAIMPServiceAlbumArtReceiveProc`, `IAIMPServiceLyrics`, `TAIMPServiceLyricsReceiveProc`, `IAIMPLyrics`, `IAIMPImage`, `IAIMPImageContainer`, `IAIMPStream`, and `IAIMPFileInfo` help pages, checked against their C++ and Delphi declarations. Both callback pages explicitly specify the main application thread. Both cancellation pages document completion with `WAITFOR`.

Cross-compilation and portable boundary tests do not establish Windows/AIMP runtime behavior. Validate artwork, plain/LRC lyrics, seeks, stream metadata changes, rapid track changes and AIMP shutdown on the intended AIMP installation before relying on this preview in production.
