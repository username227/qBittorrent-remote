# qBittorrent Web API parity plan

qBittorrent Remote is a **native desktop client** of the qBittorrent Web API.
The local Qt process never runs libtorrent and never substitutes local engine
state for remote server data. Features stay disabled until their remote
implementation and failure handling are available.

Stages:

1. Connection profiles, independent cookie jars, authentication, HTTPS
   validation, and explicit certificate trust/pinning for self-signed servers.
   Multiple concurrent sessions with independent state and no fixed limit.
   Instance tabs appear only for two or more active connections.
   Saved profiles and active sessions remain separate: provide an explicit,
   per-profile **Connect automatically on startup** option (not all saved
   profiles connect by default). An opted-in server should connect without a
   login dialog using an empty password when server authentication allows
   that, or retrieving its credential from the protected operating-system
   vault when needed. Open its live torrent view immediately after connection
   and initial synchronization. One connected server shows no server tabs;
   two or more connected servers show dynamic tabs. Connection failures or
   missing credentials must be visible and recoverable without blocking the
   rest of the application. This startup functionality is planned for the
   connection-lifecycle/multi-server phase, not the initial credential commit.
2. Incremental `/api/v2/sync/maindata`: live torrent list, server transfer
   rates, categories, tags, tracker state, sorting, filtering and status bar.
   Handle disconnects, reconnects and server-side reordering correctly.
3. Torrent operations through remote `/api/v2/torrents/*` endpoints: add,
   start, stop, remove, queue position, limits and priorities, with
   confirmation for destructive operations.
4. Torrent details: general properties, trackers, peers, HTTP sources,
   content and file priorities through relevant remote API endpoints,
   including peer sync where supported.
5. Remote preferences, categories/tags and per-server NAS-to-local path
   mappings, with clear separation of local and remote filesystem paths.
6. Other Web API capabilities in stages: RSS, search, torrent creation,
   execution logs and statistics, and available plugins/preferences.

The API version is queried **per instance**. Unsupported functionality is
disabled or clearly identified instead of silently emulated locally.

This aims for **feature parity** with qBittorrent's exposed Web API. We do
not embed the Web UI's HTML/JavaScript or link the Web UI's server-side
controller code into the native desktop application. The remote server's
API responses are the source of truth.


### Native Content tab mapped opening

The native Content context menu now offers Open, Open containing folder and Copy local path for a single accessible mapped file or directory. Reuse the existing drag-and-drop path resolver, including canonical mount containment and server-specific mappings, and revalidate at activation. Unmapped, missing and multiple selections cannot launch local applications. The containing-folder action opens the directory through Qt desktop services; platform-specific file highlighting and specialized incomplete-file preview remain follow-ups. No torrent API mutation or local torrent engine. The existing authenticated mapped-content workflow exercises actual context menu actions, intercepted desktop URL requests, clipboard, folder selection, and unmapped/multiple selection restrictions.

## Phase 3 — General piece graphs

The native General property layout draws authenticated `torrents/pieceStates` (0 missing, 1 requested/downloading, 2 complete) using palette-derived colors and weighted pixel coverage. When data is missing, the existing overall progress bar remains explicitly identified as the fallback. `torrents/pieceAvailability` is available in upstream 5.2.0/5.2.1 and 5.3.x but absent in 5.1.4; request only for eligible active incomplete downloads on supported versions. Its gradient is normalized to the maximum peer count as upstream does. The number beside the graph is the server's `properties.availability` distributed-copies value, not an arithmetic mean. Missing/malformed arrays clear the relevant graph, not unrelated properties. Existing 10-second selected-visible-detail refresh and authenticated queue are reused; selection/page generations discard obsolete responses including A→B→A. The native piece count/size/have label is restored when the API supplies all three values. Shift-hover file-to-piece mapping remains a follow-up; no additional files requests or local engine. Functional coverage exercises the actual General pane, completed and incomplete selections and graph rendering.

## Phase 3 — Editable torrent comments

Upstream 5.2.0 and 5.3.x provide `torrents/setComment` (`hashes`, `comment`), absent in 5.1.4. The upstream desktop General panel displays comments read-only; the remote client adds a small Edit button beside the existing label, disabled on unsupported servers or until the selected torrent's raw comment is loaded. A native multiline plain-text dialog preserves the original text, supports clearing it, and uses the same trimming semantics as upstream. Cancel/unchanged input causes no write. Requests use the originating authenticated session and exact single hash, with percent-encoded form values, pending-write protection, and a fresh properties read after acceptance or rejection. The displayed comment remains server-authoritative, with the existing escaped HTTP(S) link rendering. Functional coverage uses a legacy and modern server with overlapping hashes, tests Cancel, edit/readback, special characters, clearing and isolation.

## Phase 3 — Peer table and synchronization

Peer polling now uses the actual per-session `sync/torrentPeers` response ID, resolved at dispatch time, and merges partial updates/removals. Selection/page changes reset the ID to zero; invalid responses clear peer data and force a full refresh. Existing rows remain in place and retain selection while sorting is suspended for updates. The 15 columns cover native peer fields including port, downloaded/uploaded totals, relevance, contribution, files and Peer ID Client; missing fields remain blank or unavailable (e.g. contribution on 5.1.4). Copy IP:port uses the original server endpoint, not a DNS label. The header supports column visibility and resizing. The expanded column layout uses a new saved-header key to avoid restoring an incompatible eight-column layout. Add/ban actions are deferred to the next checkpoint. Tests exercise partial-field updates, empty deltas, removals, stable row identity, selection, and real nonzero RID requests.

## Phase 3 — Peer management

The native Peers context menu adds `Add peers...` and `Ban peer permanently` (also Delete). Addition accepts validated numeric IPv4 and bracketed IPv6 endpoints, rejects malformed input, deduplicates, and uses `torrents/addPeers` with the exact selected hash and server-reported added/failed counts. Both 5.1.4 and 5.3.x support this endpoint. Before adding, obtain authoritative `properties.private` on the selected server (unknown/private, queued and checking torrents are blocked). Ban uses `transfer/banPeers` on the originating authenticated session after a default-No confirmation explicitly stating that the IP ban applies to all torrents on that server. I2P/non-IP endpoints cannot be banned with this API. Port formatting is ungrouped. Tests drive the real context menu, cancellation, accepted add/ban, result counts, private restriction and request parameters. No local networking or optimistic peer list changes.

## Phase 3 — HTTP Sources

Native HTTP Sources supports Add web seed, Edit web seed URL, Remove web seed (multi-selection), Copy web seed URL, F2, Delete, Ctrl+C and double-click editing. Upstream 5.1.4 and 5.3.x expose all three mutation endpoints. Validate HTTP/HTTPS/FTP URLs, reject duplicates and invalid selections, and match the official WebUI's two-layer URL encoding. The exact originating hash/session handles the write; server readback, not an optimistic list change, controls the visible state. Preserve surviving items/selection during polling. Upstream does not impose a private-torrent prohibition for web seeds; none is added here. As the native server applies changes asynchronously, a successful HTTP response is acceptance, not proof of immediate application. Functional tests cover actual native actions, cancellation, percent-escaped URL round trips, copying, editing, multiple removal, and authenticated readback.

## Phase 3 — Tracker tab actions

Native context and keyboard actions now support add/edit/remove/copy and all-tracker reannounce. Selected-tracker reannounce uses the 5.2+ `urls` parameter; unavailable on 5.1.4, never silently widened to all. Selected DHT-only announce is not exposed by this API. Preserve existing private/unknown tracker protection: no additional trackers, except recovery from zero to one; edits/removals remain allowed. Read the selected torrent's authoritative privacy and tracker list, validate on the originating session as well as UI, exclude pseudo and endpoint rows, refresh after accepted/rejected writes. Single edit preserves tier and uses `origUrl` on 5.1.4 versus `url` on 5.2+. Header visibility/resize and F2/Delete/Ctrl+C are provided. Tests cover legacy/modern actual native actions, server readback, selected/all announce scope, and private recovery. No dependencies added.


## Phase 3 — Native Content file and folder rename

A single selected file or directory can be renamed through the native Content context menu or F2, without a local mapping. Use upstream `torrents/renameFile` and `torrents/renameFolder` with `hash`, `oldPath` and `newPath` on both 5.1.4 and 5.3.0; reuse the Manage Content backend, validate relative paths, encode each form value exactly once, preserve the parent path and reject path separators in the new name. Recheck the selected torrent, tab and path after the dialog, serialize pending rename requests, and refresh the authoritative file list on completion without optimistic local renaming. Extend the live authenticated Content workflow to verify cancel, file/folder rename, special characters, server readback and unaffected neighboring torrent. Batch rename, order-based priorities and double-click configuration remain follow-ups.


## Phase 3 — Native Content batch rename

Batch rename opens the same native Manage Torrent Content dialog as upstream, with the selected file IDs (including selected folder descendants, deduplicated) preselected and all torrent paths visible. It works without local mounts on existing authenticated `renameFile` API support (5.1.4 and 5.3.0); validate originating server, selected torrent and Content-tab generation. Common-path edits preserve relative suffixes even with an empty common prefix. Preflight duplicate, occupied and conflicting file/directory targets before dispatch; swaps and cycles need a separate temporary-name workflow and are intentionally rejected. Serialize edits using the existing remote queue, disable editing/closing while writes are pending, and read back server state. The API does not supply an atomic batch transaction: server-side errors can still leave partial changes. Extend the functional Content workflow to exercise the actual menu, preselection, duplicate folder/child selection, common-prefix editing, collision rejection, remote readback and unaffected neighboring files/torrent.

## Phase 3 — Content priority by shown file order

Follow upstream `TorrentContentWidget::applyPrioritiesByOrder` (release-5.3.1): divide selected rows into groups of max(floor(n/3),1) rows, assigning Maximum (7), High (6), and Normal (1, including remainder). Walk the currently sorted tree in shown order, treating selected folders as one row and deduplicating overlapping descendants (parent wins). Capture the selection, groups and torrent generation when opening the menu; reject stale triggers. Reuse existing `torrents/filePrio` operations in at most three queued requests, with server readback rather than optimistic edits. The functional workflow checks the actual context menu, descending order versus selection order, exact resulting priorities, readback and torrent isolation.

## Phase 3 — Content interaction preferences

Upstream release-5.3.1 `src/gui/properties/propertieswidget.cpp` sets double-click Open and Enter/Return opening, while `TorrentContentWidget` also supports Rename mode (used in Add Torrent). Expose these as local desktop preferences under Behavior / Torrent Content View, defaulting to upstream Properties behavior. Double-click rename uses the existing guarded remote rename dialog; open and keyboard activation require one accessible, mapped item and reuse the existing containment checks. No new server API. Disable automatic expansion on double-click, as upstream does. Test actual mouse and keyboard interactions, preferences persistence/live application, cancellation and no mapping/multiple selection safeguards. These settings are a deliberate local extension of upstream's fixed Properties defaults.

## Phase 3 — Mapped incomplete-file preview

Content context menu provides Preview incomplete file for a single partially downloaded audio/video file with an accessible mapped path. Based on upstream release-5.3.1 `TorrentImpl::contentPath`, `Path::findRootFolder`, and `PreviewSelectDialog::previewButtonClicked`: the actual storage location is represented by content_path, not the eventual save_path. Use the file tree to distinguish a single logical file, a common first-level root and a rootless torrent. Resolve only the expected logical name or its .!qB variant; refuse ambiguous matches, unmapped content, missing files, non-media, zero/complete progress and multi-selection. Reuse existing canonical mount containment and local desktop URL opening; revalidate selection and path at activation. No mutation, copying, forced cache flush or remote streaming. Desktop association and playable pieces are not guaranteed, especially for .!qB. This is the Content milestone, not the full transfer-list preview chooser or Phase 5 remote file retrieval. Functional coverage uses a temporary download location and stale finished copy, suffix/plain/ambiguous/missing cases, and actual context menu plus captured desktop launch.

## Phase 3 — Content final review (header and filtering)

Upstream release-5.3.1 `TorrentContentWidget::displayColumnHeaderMenu`, `setFilterPattern`, and `PropertiesWidget::showContentFilterContextMenu`: expose per-column visibility (Name always visible), Resize columns, movable/resizable sections, and native Plain text/Wildcards/Regular expression modes (case-insensitive, unanchored wildcards by default). Reuse one matcher for interactive and synchronized updates; invalid patterns show an error tooltip and match nothing; hidden selections are cleared. Persist format alongside the existing per-server header state. Expand matching branches on explicit filtering but preserve expansion during polling. Functional test exercises actual menus, visibility, wildcard/regex/plain filtering and invalid expressions. Further review items: multi-selection Space/Select checkbox behavior and OS-specific containing-folder item highlighting; transfer-list preview chooser remains separate. No change to server endpoints.
