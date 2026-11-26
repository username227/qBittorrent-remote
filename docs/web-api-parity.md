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
