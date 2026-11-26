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

## Phase 3 — General piece graphs

The native General property layout draws authenticated `torrents/pieceStates` (0 missing, 1 requested/downloading, 2 complete) using palette-derived colors and weighted pixel coverage. When data is missing, the existing overall progress bar remains explicitly identified as the fallback. `torrents/pieceAvailability` is available in upstream 5.2.0/5.2.1 and 5.3.x but absent in 5.1.4; request only for eligible active incomplete downloads on supported versions. Its gradient is normalized to the maximum peer count as upstream does. The number beside the graph is the server's `properties.availability` distributed-copies value, not an arithmetic mean. Missing/malformed arrays clear the relevant graph, not unrelated properties. Existing 10-second selected-visible-detail refresh and authenticated queue are reused; selection/page generations discard obsolete responses including A→B→A. The native piece count/size/have label is restored when the API supplies all three values. Shift-hover file-to-piece mapping remains a follow-up; no additional files requests or local engine. Functional coverage exercises the actual General pane, completed and incomplete selections and graph rendering.

## Phase 3 — Editable torrent comments

Upstream 5.2.0 and 5.3.x provide `torrents/setComment` (`hashes`, `comment`), absent in 5.1.4. The upstream desktop General panel displays comments read-only; the remote client adds a small Edit button beside the existing label, disabled on unsupported servers or until the selected torrent's raw comment is loaded. A native multiline plain-text dialog preserves the original text, supports clearing it, and uses the same trimming semantics as upstream. Cancel/unchanged input causes no write. Requests use the originating authenticated session and exact single hash, with percent-encoded form values, pending-write protection, and a fresh properties read after acceptance or rejection. The displayed comment remains server-authoritative, with the existing escaped HTTP(S) link rendering. Functional coverage uses a legacy and modern server with overlapping hashes, tests Cancel, edit/readback, special characters, clearing and isolation.
