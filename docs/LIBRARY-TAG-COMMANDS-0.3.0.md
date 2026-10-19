# Playlist and tag additions in 0.3.0

These commands extend the existing protocol. Existing playback, queue, playlist,
volume and DSP commands keep their original payloads. All indexes below are zero
based. Every reply includes its request `action`; only one request may be in flight.

## Target selection

Playlist targets are `active` (default), `focused`, `playing`, or `explicit` with
`playlist_id`. Track targets are `focused` (default), `playing`, or `explicit` with
`playlist_id` and `track_id`. Supplying numeric IDs without `target` selects the
explicit mode. A saved Companion preset must resolve playlist GUIDs and track
paths against fresh inventory before sending current numeric indexes.

**0.3.1 correction:** `focused` resolves AIMP’s current active playlist and actual
track focus property. No current playlist returns `NO_FOCUSED_PLAYLIST`; no
focused track, including group focus, returns `NO_FOCUSED_TRACK`. No index-zero
fallback is used. Track actions affect one focused entry, not all selected entries.

## Playlist operations

| Action | Additional arguments | Result/behaviour |
| --- | --- | --- |
| `playlists/import` | `uri`, optional `activate:true` | Loads a playlist file/URL through AIMP; returns playlist and accepted status. |
| `playlists/track/move-to` | Track target, `destination_playlist_id` | Moves that entry to another loaded playlist using its writable PLAYLIST property. |
| `playlists/track/copy-to` | Track target, `destination_playlist_id`, optional `insert_at:-1` | Adds the same media URI to the destination asynchronously; `-1` appends. |
| `playlists/track/selected` | Track target, Boolean `value` | Changes selection without replacing other selected entries. |
| `playlists/track/refresh` | Track target | Reloads that entry's media information. |
| `playlists/selection/set` | Playlist target, `mode:all/none/invert` | Changes selection in a notification batch; bounded to 100,000 entries. |
| `playlists/groups/get` | Playlist target, `offset:0`, `limit:100` (maximum 250) | Returns a page of groups. |
| `playlists/group/set` | Playlist target, `group_index`, `parameter:expanded/selected`, Boolean `value` | Expands/collapses or selects/deselects one group. |
| `playlists/groups/merge` | Playlist target, optional `group_index` | Merges matching groups; omitted index merges all matching groups, as defined by AIMP. |
| `playlists/properties/get` | Playlist target | Returns values and per-property availability. |
| `playlists/properties/set` | Playlist target, `parameter`, typed `value` | Sets an allowlisted property and reads the resulting state. |
| `playlists/sort/template` | Playlist target, `template` | Sorts using AIMP's own metadata template format. |
| `playlists/refresh` | Playlist target, `mode:missing/all/selected` | Requests AIMP's background metadata rescan. |
| `playlists/source/get` | Playlist target | Reads smart-playlist source and supported options. |
| `playlists/source/create` | Playlist target; `kind:folders` with `folders:[{path,recursive}]`, or `kind:playlist` with `uri` | Creates and attaches a new supported source; can replace the current source. |
| `playlists/source/set` | Playlist target, `parameter:autosync/autosync_on_startup/sort_template`, typed `value` | Updates a supported option on the attached source. |
| `playlists/source/sync` | Playlist target | Requests synchronization with the attached source. |

`source/create` also accepts optional Boolean `autosync`, Boolean
`autosync_on_startup`, and string `sort_template`. Folder arrays contain 1–256
entries; paths/URIs are limited to 16,384 UTF-8 bytes. Source objects are configured
before attachment, so an invalid option does not replace the existing source.
Source replacement/synchronization can change playlist membership; files on disk
are not deleted. The native SDK decides when a changed source is synchronized.

Successful mutations include `library_changed:true`. Import/copy/refresh/source
operations can return `accepted:true`: loading or scanning was accepted, not
necessarily completed. Clients should refresh inventory after a short delay and
retain subsequent player/inventory events. Batch selection failures return the
number already changed; there is no fabricated all-or-nothing claim.

### Property names and types

- Strings: `name`, `group_name`, `grouping_template`, `format_line1`, `format_line2`.
  Maximum 4096 UTF-8 bytes. An empty playlist name is rejected; other strings can
  be cleared.
- Booleans: `read_only`, `grouping_override`, `grouping`, `grouping_auto_merge`,
  `format_override`, `view_override`, `view_duration`, `view_expand_buttons`,
  `view_marks`, `view_numbers`, `view_numbers_absolute`, `view_second_line`,
  `view_switches`.
- Integers: `focused_index`, `playback_cursor`, `playing_index`; accepted range
  `-1` through the last current track index. AIMP controls which values apply in
  the current playback state.

AIMP requires the corresponding override switch before applying overridden
formatting/grouping/view settings. The bridge does not silently enable overrides.
Group names are read-only in `IAIMPPlaylistGroup`; playlist group names are writable
through the playlist property. Read-only playlist properties such as ID, duration,
remaining duration, byte size and track count remain in `playlists/info`.

### Response namespaces

- `playlist_properties:{playlist_id,values:{parameter:value},available:{parameter:boolean}}`
- `groups:{playlist_id,total,offset,items:[{id,name,track_count,expanded?,selected?,duration?}],has_more}`
- `playlist_source:{playlist_id,attached,factory_id?,sort_template?,autosync?,autosync_on_startup?,available:{...},folders?:[{path,recursive}],uri?,read_error?}`

No attached source is a successful `attached:false` result. Individual sources
may not implement automatic synchronization or startup synchronization. Source
read failures are not presented as false values.

## Direct tag editing

`tags/get` uses a track target and returns:

```json
{
  "tags": {
    "file_path": "D:\\Music\\Track.flac",
    "tag_count": 1,
    "fields": {"title": "Track", "artist": "Artist", "rating": 4}
  }
}
```

`tags/write` requires **both explicit confirmation and the exact file path returned
by a preceding read**, in addition to the track target and changed fields:

```json
{
  "action": "tags/write",
  "target": "focused",
  "expected_file_path": "D:\\Music\\Track.flac",
  "confirm": true,
  "fields": {"title": "New title", "rating": 5}
}
```

If the selected file changed between read and write, the bridge returns
`TAG_FILE_CHANGED` before opening the editor. The Companion action must default
its file-write confirmation checkbox to false; a preset must not opt the user in.
These operations support existing local or normal network-share media file paths.
Internet streams, virtual/archive media paths and device namespaces are excluded
from this disk-writing operation. File format support comes from AIMP's tag editor.

The exact writable field allowlist is exposed in `capabilities.tags.fields`:

- Text: title, artist, album, album_artist, genre, date, track_number, track_total,
  disc_number, disc_total, comment, lyrics, composer, copyright, publisher,
  conductor, lyricist, encoded_by, mood, catalog, isrc, key, url.
- Integers: bpm and file-tag rating (0–5). This rating is distinct from the
  existing playlist/library star rating.
- Numbers: track_gain, album_gain, track_peak, album_peak (ReplayGain tag values).

Empty text clears that field. NUL bytes, unknown fields, Boolean numeric values,
non-finite numbers, invalid ratings, and oversized text are rejected before disk
access. The request allows at most 49,152 text bytes in total; each descriptor
also supplies its individual length/range bound. Gain/peak and BPM limits are
bridge input guards, not claims about an undocumented AIMP range. Technical
stream properties, file names, library database fields, and arbitrary numeric
property IDs cannot be written through this interface.

The writer clones the editor's existing mixed metadata, changes the named fields
in that clone, calls `SetToAll` to apply the merged metadata to AIMP's supported tag
formats, then calls `Save`. Unspecified merged values remain, but applying the
merged metadata can normalize differences between existing tag formats, including
fields not named in the request. It does not preserve each format's separate
values or byte-for-byte contents. It never modifies the player's global `GetInfo`
object. A successful save returns
`tags:{file_path,saved:true,changed_fields:[...]}` and requests playlist metadata
refresh. `changed_fields` lists the requested fields, not every possible change
caused by normalization. Read tags again to inspect the saved file. A failed
native `Save` reports `file_may_have_changed:true`; native file writes are not
guaranteed transactional and must not be automatically retried.

## Capability catalogue

`capabilities.library.commands` includes the 17 management actions above.
`library.playlist_properties` supplies `{id,label,type,min?,max_length?}` entries.
`library.source_types` lists registered supported factories as `{id,label}`.
Source creation fails cleanly if a factory disappears or is unavailable.
`capabilities.tags` contains `available`, `commands`, `fields`,
`requires_confirmation:true`, `requires_file_identity:true`, target choices and
text limits. Startup discovery only queries services/factories; it does not open
or write media files.

## SDK and ABI evidence

All new calls were checked against the official AIMP 5.40 C++ declarations,
Delphi declarations, and extracted English reference help. Playlist/group/tag
methods used here are `stdcall` in both languages. The tag editor's methods
require AIMP's main thread even though the parent file-tag-editor service is
thread-capable; no unsupported worker-thread SDK calls are introduced.

The official C++ `IAIMPPlaylistPreimageFolders::ItemsGet` declaration is defective:
its `IAIMPString*` argument conflicts with Delphi's `out Path: IAIMPString`, which
requires `IAIMPString**`. The bridge uses a private corrected interface with the
same IID, inherited methods and five-method order. It does not invoke the bad C++
slot signature. The unrelated `IAIMPPlaylistPreimageDataProvider::GetFiles`
C++/Delphi pointer mismatch is not used.

Existing Save Playlist, metadata rescan and tag-editor dialogs remain available
as application commands. Arbitrary physical file deletion, tag-format deletion,
raw custom-property writes, and unsupported source configuration serialization are
not implemented by these music-control commands. Music-library query/filter
sources belong to the separate music-library component.

## Verification

Both x86 and x64 translation-unit syntax builds passed. Portable tests exercise
the exact tag-write confirmation, file-identity requirement, field allowlist,
integer/number typing, finite-number checks and payload limits. These checks do
not constitute a Windows AIMP tag-write or smart-playlist runtime test.
