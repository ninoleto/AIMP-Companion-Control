# Focused-target correction, 0.3.1

AIMP reports `AIMP_PLAYLIST_PROPID_FOCUSINDEX = -1` when no track is focused.
The legacy navigation cache normalizes negative indices to zero. Extended
library commands previously reused that cache, so a focused-track action could
operate on the first entry when the user had no focused track or focused a group.

The extended library resolvers now read the SDK directly before accessing a track:

- A focused playlist resolves through `GetActivePlaylist`; no active playlist
  returns `NO_FOCUSED_PLAYLIST`.
- A focused track reads `AIMP_PLAYLIST_PROPID_FOCUSINDEX` from that playlist.
  Missing properties, unsuccessful reads, negative indices, and indices outside
  the current item count return `NO_FOCUSED_TRACK` before `GetItem` or mutation.
- Explicit numeric targets and the playing-track service path retain their
  existing semantics. Legacy focus navigation remains unchanged.

The shared resolver covers focused playlist-entry removal, queue add/remove,
selection/rating/enable changes, move/copy, metadata reads/refreshes, and file-tag
operations. It does not implement bulk removal of all highlighted tracks.

Regression tests execute the exact production resolver bodies using portable
SDK doubles. Cross-compilation verifies both x86 and x64 declarations. These
checks do not execute the plugin inside Windows AIMP.

AIMP’s numeric-index protocol still requires fresh inventory for explicit targets.
This patch does not add an atomic expected-GUID precondition for multi-request
clients or change unrelated visualization, source, or music-library behavior.
