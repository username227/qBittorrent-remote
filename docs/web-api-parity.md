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
