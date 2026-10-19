# Music-library protocol, 0.3.0

This is a bounded client of AIMP's real music-library services. It does not register a synthetic storage, enumerate the whole library, or execute SQL. Availability is discovered per storage and per optional command interface. Native Windows/AIMP runtime verification is still required; cross-compilation and portable validation tests do not exercise AIMP providers.

## Capabilities and discovery

`capabilities/get` includes `capabilities.music_library`, the raw result of `GetMusicLibraryCapabilities()`:

```json
{
  "available": true,
  "async": true,
  "active_storage": "TAIMPMLLocalDataStorage",
  "storages": [{
    "id": "TAIMPMLLocalDataStorage", "name": "Local files", "active": true,
    "can_query": true, "paging": "offset", "schema_source": "official_local_catalog",
    "search_supported": true, "sort_supported": true, "fields_available": true,
    "commands": {
      "add_files": true, "reload_tags": true, "add_files_dialog": true,
      "report_dialog": true, "row_remove": true, "row_reload_tags": true, "row_mark": true
    }
  }],
  "limits": {"page":100,"offset":1000000,"fields":14,"filters":8,"sort":3,
             "cached_pages":16,"loaded_track_scan":10000,"search_bytes":512},
  "queue_requires_loaded": true,
  "physical_deletion": false,
  "ui": true
}
```

The booleans above are illustrative; actual providers can omit any command. A storage without a provider-bound filter, background task service, or discoverable required field schema has `can_query:false` and `unavailable_reason`. `schema_source` is `provider` when the optional extension supplies fields. Only the official local storage gets the SDK's documented fallback catalog. Unknown providers never get fabricated field names. Capabilities list all recognized protocol commands; individual operations still check their service and HRESULT when invoked.

| Action | Arguments | Reply payload |
|---|---|---|
| `musiclibrary/storages/get` | none | `music_library:{available,storages,active_storage}` |
| `musiclibrary/storage/select` | `storage_id` | Updated storage list and active storage |
| `musiclibrary/fields/get` | `storage_id` | `music_library.fields:{storage_id,schema_source,search_supported,sort_supported,fields}` |
| `musiclibrary/ui/get` | none | `music_library.ui:{available,path}` |
| `musiclibrary/ui/path/set` | `path` string, maximum4096 UTF-8 bytes | Same UI object after setting grouping-tree focus |

Each field has `id`, `label`, `type`, `filterable`, `internal`, and an `operators` list. Field types are `int32`, `int64`, `number`, `string`, `string_set`, `datetime`, `duration`, `filesize`, or `filename`. Duration is seconds. Datetime retains the provider/SDK Double representation, not an assumed Unix timestamp. Int64/file-size reply values are decimal strings, preserving JavaScript precision. String cells are truncated to128 UTF-16 code units without splitting a surrogate pair.

## Asynchronous queries

```json
{
  "action":"musiclibrary/query", "storage_id":"TAIMPMLLocalDataStorage",
  "search":"piano", "offset":0, "limit":50,
  "fields":["Title","Artist","Album","Duration","UserMark"],
  "match":"all",
  "filters":[{"field":"Duration","operator":"between","value":60,"value2":300}],
  "sort":[{"field":"Artist","direction":"asc"},{"field":"Title","direction":"asc"}]
}
```

Only `storage_id` is mandatory for a first query. Defaults: offset0, limit50, match`all`, default metadata columns. `ID` and `FileName` are always added. Maximum14 requested fields plus those two; filters8; sort keys3; limit1..100; offset0..1000000. No limit0/unlimited query is sent. Filter values must match the discovered field type. Int64 numeric inputs must remain in JavaScript's exact integer range; use cached-row actions to operate on larger returned identifiers.

Operators: `eq`, `ne`, `lt`, `lte`, `gt`, `gte`, `between`, plus `contains`, `starts_with`, `ends_with` for string fields. Search is an OR-of-contains group over up to six supported text fields, preferring Title/Artist/Album/AlbumArtist/Genre/FileName. It is ANDed with the explicit all/any filter group. This does not assume the optional provider SEARCHSTRING property works. Sorting uses the SDK's required combined IAIMPString + IAIMPMLSortDirection objects, including clones that retain both interfaces.

Initial reply:

```json
{"music_library":{"query":{"id":"ml-1","status":"pending","storage_id":"TAIMPMLLocalDataStorage","offset":0,"limit":50,"paging":"offset"}}}
```

Poll `musiclibrary/query/get {query_id:"ml-1"}`. A fast query may already be complete in its initial reply. Complete shape:

```json
{
  "music_library":{"query":{
    "id":"ml-1","status":"complete","storage_id":"TAIMPMLLocalDataStorage",
    "offset":0,"limit":50,"paging":"offset","count":1,"total":null,
    "has_more":false,"next_offset":null,"next_cursor":null,"queue_requires_loaded":true,
    "rows":[{
      "row_id":"ml-1:0","record_id":42,"file_path":"D:\\Music\\track.flac","file_path_truncated":false,
      "title":"Track","artist":"Artist","album":"Album","duration":190,
      "values":{"ID":42,"FileName":"D:\\Music\\track.flac","Title":"Track"},
      "can_play":true,"can_add":true,"can_queue":true,
      "can_remove":true,"can_reload_tags":true,"can_mark":true
    }]
  }}
}
```

Provider errors use query status`failed` with `error:{code,hresult?}`. Provider-returned strings appear as `provider_message`. No total count is invented. Service/protocol/expired-ID errors are top-level `error` objects. Query status is one of pending/complete/failed/cancelled.

Provider1 uses offset pagination and requests one look-ahead row to determine `has_more`. Repeat the same parameters with `next_offset`. At the offset ceiling `has_more` can remain true while `next_offset` is null; the bridge refuses traversal beyond its bound.

Provider2-only storage uses opaque cursor pagination. Start at offset0; later send only `{action:"musiclibrary/query",cursor:"ml-1:next"}`. Cursor requests cannot change filters, fields, sort, storage, offset, or limit. Returned provider PageID values remain server-side. A provider selection larger than the page limit is resumed before requesting its next page. Cursors are single-use and expire when their page leaves the16-page cache; previous pages can be read by their existing query IDs. Clients should retain forward-page history when navigating backward.

Only one query worker is active. A second request returns `LIBRARY_QUERY_BUSY`. `musiclibrary/query/cancel {query_id}` sets a cooperative cancellation flag and returns the current query state, often still pending. Continue polling until cancelled/complete/failed before submitting another query. A blocked provider GetData call cannot be forcibly interrupted by the SDK. Ordinary cancellation does not cancel the SDK task before Execute, which would leave a pending query with no worker to finish it.

## Cached-row playback and playlist actions

All row actions require both `query_id` and the exact server-issued `row_id`. Clients cannot substitute a filename or provider record ID. Expired pages and mismatched/stale row IDs fail. The exact URI is retained server-side; displayed paths stop at1024 UTF-16 units. Paths exceeding32768 units or containing embedded NUL are not actionable. A string record ID exceeding512 UTF-16 units disables record mutations; it never uses a truncated ID.

| Action | Additional arguments | Behavior |
|---|---|---|
| `musiclibrary/row/play` | none | Player.Play4, play without adding to playlist |
| `musiclibrary/row/add` | optional `playlist_id` loaded-playlist index | Add to specified or active playlist, NOEXPAND, asynchronous acceptance |
| `musiclibrary/row/queue` | optional boolean `first` | Queue an already-loaded playlist item with exactly matching URI |
| `musiclibrary/row/reload_tags` | none | Optional storage ReloadTags on one cached <ID,URI> pair |
| `musiclibrary/row/mark` | `value` finite number0..5 | Optional storage SetMark using the exact cached typed ID |
| `musiclibrary/row/remove` | boolean `confirm:true` | Optional storage deletion of one cached pair, Physically=FALSE |

Play/add replies acknowledge SDK acceptance; completion is not assumed. Queue searches at most256 loaded playlists and10000 loaded entries. If the URI is absent, it returns `ROW_NOT_LOADED_ADD_TO_PLAYLIST_FIRST`; it does not freeze the GUI waiting for asynchronous playlist expansion. `can_queue` indicates queue service + usable URI, not a prior loaded-item scan. Case/URI comparison is exact so unrelated provider resources are not conflated. Cached metadata is a snapshot; re-query after modifying a record.

## Storage operations and completion

`musiclibrary/storage/command` takes `storage_id` plus one allowlisted `command`:

| Command | Arguments | SDK call |
|---|---|---|
| `add_files` | `files`:1..100 strings, each maximum32768 UTF-8 bytes | CommandAddFiles.Add(list of IAIMPString) |
| `reload_tags` | none | CommandReloadTags.ReloadTags(nullptr), documented all-files case |
| `add_files_dialog` | none | CommandAddFilesDialog.Execute(NULL owner) |
| `report_dialog` | none | CommandReportDialog.Execute(NULL owner) |

Storage commands and row reload/mark/remove are posted to a private main-thread message window. This lets WebSocket acknowledgement flush before a provider opens a modal dialog. One operation can be queued/executing at a time; another returns `STORAGE_OPERATION_BUSY`. The storage/interface and row identity are captured before queuing, so changing the active UI storage does not retarget the command.

```json
{"acknowledgement":"queued","music_library":{"operation":{"id":"ml-op-1","status":"queued","command":"row_mark","storage_id":"TAIMPMLLocalDataStorage"}}}
```

Poll `musiclibrary/operation/get`. It returns `music_library.operation` (null before any operation). Status becomes executing/completed/failed, with the returned HRESULT and error on failure. Completed means the provider method returned S_OK; a provider may continue its own background import/reload. No completion or success is fabricated before that return. Physical file deletion, filtered bulk deletion, and clearing an entire storage are not exposed.

## SDK/ABI audit and lifecycle

Authoritative evidence is the official stable SDK5.40 (`Sources/Cpp/apiMusicLibrary.h`, matching Delphi unit, and extracted English CHM), plus `Demos/MusicLibraryBrowserDemo/uDataProvider.pas` for task usage and initial selection position. The demo reads the initial current row before NextRow; this bridge follows it. Both providers/selections and filters support asynchronous access; storage discovery, commands, playlist/player operations, UI, and object creation run on AIMP's main thread.

New interfaces compared for calling convention, method order, return width, and pointer levels: IAIMPMLDataStorage/2, IAIMPMLExtensionDataStorage.GetFields, IAIMPMLDataProvider/2, IAIMPMLDataProviderSelection, IAIMPMLDataField, IAIMPMLDataFilterGroup/Filter, IAIMPMLSortDirection, IAIMPMLFileList, the six storage command interfaces used above, IAIMPServiceMusicLibrary/UI, IAIMPTask/Owner, IAIMPServiceThreads. Methods used have matching stdcall/WinAPI declarations. Provider2 PageID is `var IUnknown` (IUnknown** in/out). Selection's NextRow and command CanDelete are32-bit LongBool/BOOL, not HRESULT. The CHM's CanDelete return label is inconsistent; both language headers agree BOOL. Const OleVariant filter/ID arguments are passed by pointer; doubles are passed as the SDK declarations specify. No direct Equalizer.GetActive call exists in this component.

The pre-existing C++ IAIMPPropertyList2.GetValueAsVariant declaration has an extra pointer level compared with Delphi; this code never calls it. IAIMPMLDataStorageManager.BackgroundTask IDs have a C++/Delphi size discrepancy on64-bit; this bridge neither implements nor calls that interface. Provider extension Initialize/Finalize/Config/FlushCache and registration are provider lifecycle hooks, not public refresh/admin commands, and are not invoked.

Root integration calls `FinalizeMusicLibraryControls()` before releasing g_core and links oleaut32. The worker never waits for a callback to AIMP's main thread. Finalization sets cancellation and joins through checked Cancel(WAITFOR)/WaitFor. A module reference is acquired before any asynchronous worker or posted operation. If the SDK cannot confirm a join, or finalization reenters during task submission/provider UI execution, bounded contexts and the module reference are retained until process exit, preventing execution into an unloaded DLL. A normal successful teardown releases the module reference while AIMP still owns its plugin reference.

## Deliberate scope boundaries

The SDK exposes more interfaces than remote playback needs. This bridge does not implement provider registration, custom album-art/grouping-tree providers, grouping-preset creation/deletion, bulk filtered deletion, DropData, physical deletion, or an unbounded UI selection export. These are bridge scope decisions, not claims that AIMP lacks those APIs. The documented query/filter service covers browsing without traversing provider grouping trees. A storage without a discoverable field catalog remains listed but unqueryable. This release supplies no arbitrary action-ID, SQL, shell, or keyboard-injection fallback.

Portable tests cover malformed requests, exact field/operator/type validation, numeric overflow/precision, page/offset/filter limits, cursor parameter isolation, and stale query-row identity. Both32-bit and64-bit Windows translation units compile. Provider ordering, service availability, actual storage command behavior, and teardown against live AIMP remain Windows runtime checks.
