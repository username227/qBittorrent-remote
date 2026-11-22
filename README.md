# qBittorrent Remote

An independent, cross-platform Qt 6 desktop application for controlling
remote qBittorrent instances through their Web API.

**Current status:** early remote-connection foundation. Saved server profiles,
real Web API cookie authentication, qBittorrent/version verification and
disconnect are available. Torrent synchronization and management are not yet
implemented. No local torrent engine or libtorrent dependency.

## Build and test

CMake is the **sole build definition**, and CTest is the **sole test runner**.
There are no alternate build scripts or independent test commands.

Requirements:
- CMake 3.21 or newer
- C++17 compiler
- Qt 6.6 or newer (Core, Widgets, Network and Test when tests are enabled)
- Ninja (for the checked-in presets)

```sh
cmake --preset dev
cmake --build --preset dev
ctest --preset dev
./build/dev/qbittorrent-remote
```

A release configuration is also available:

```sh
cmake --preset release
cmake --build --preset release
ctest --preset release
```

Or configure manually with any supported CMake generator. Standard
`BUILD_TESTING=OFF` disables test targets; tests default to enabled.

### Testing policy

**CTest checks real application functionality and user-visible workflows.**
Tests should confirm observable outcomes (for example, opening the application,
connecting to a server, or completing a torrent-management operation). They
must not be written around individual code blocks, internal implementation
details, or fixed example inputs merely to exercise a parser or function.

Qt Test may drive GUI interactions but is invoked exclusively through CTest.

## Native interface

The main window uses qBittorrent's **original Qt Designer** `mainwindow.ui`,
its collapsible filter-header widget and adapted properties-tab bar, together
with the complete original icon resources, country flags and reference dialog
layouts (attribution in `src/gui/upstream/README.md`). The torrent list,
filter models and central content remain independent Qt Widgets; the local
torrent engine is never imported.

Only working interface actions are enabled. Torrent-mutating controls and
filtering remain disabled until remote synchronization and management exist.

## Connect to a server (foundation)

Use **File > Connect to Server...**. Enter a name, full HTTP(S) URL,
username and password. **Save** stores only the name, URL and username in
Qt's per-user settings; the password is never stored. **Connect** also
saves/updates the profile and performs a real Web API login, followed by
a request for the remote qBittorrent version and Web API version.

The status bar reports the result. **File > Disconnect** releases the
connection and attempts a nonblocking Web API logout.

Authentication uses an independent Qt Network cookie jar per session,
an explicit same-origin Referer, no automatic redirects, bounded request
timeouts and standard HTTPS certificate verification. When a server uses a
valid self-signed certificate or private CA, the user may **explicitly
inspect and trust the SHA-256 certificate fingerprint for that saved
profile**. The trust prompt includes the address, subject, issuer, validity
dates and verification errors. On a changed certificate, both the old and
new fingerprints are displayed and approval is requested again. A modern
certificate with a Subject Alternative Name (SAN) must identify the
requested server address. For an IP-based connection, a legacy private
certificate with **no SAN at all** may instead use an exact matching IP
Common Name, with explicit fingerprint approval and pinning. A conflicting
SAN never falls back to Common Name. Other hostname mismatches, certificate
expiry and unrelated TLS errors cannot be overridden. Trust is invalidated
on profile URL change or removal.

HTTP itself is not encrypted unless protected by a separate trusted tunnel
(such as an existing VPN).

This initial milestone exposes **one selected visible connection**.
Unrestricted multiple concurrent sessions with dynamic tabs are the next
development stage, not a hardcoded architectural server limit. No fake
torrents, server tabs or welcome screen have been added.

CTest includes a disposable local simulated qBittorrent Web API server
to exercise real login, server/version verification, authentication
failure and disconnect without any live NAS or passwords.
An additional CTest workflow spins up temporary HTTPS servers and exercises
the actual certificate approval UI, remembered trust, certificate changes
and hostname mismatch rejection. Test-only certificates are generated at
runtime and **no TLS private key is committed**. The test needs the
`openssl` command-line utility available on `PATH`.

**Future multiple-instance behavior:** saved profiles and active sessions are
distinct; there is no arbitrary connection limit. When remote connectivity
exists, zero active connections will show a connection screen, one active
connection will use the regular interface without server tabs, and two or more
active connections will show dynamic tabs. No premature welcome screen or
placeholder server tabs are included in this GUI milestone.

The remote API implementation will be developed in stages, aiming for full
parity with server-exposed features. See [the Web API parity plan](docs/web-api-parity.md).

## Repository and architecture

- `master`: independent application.
- `upstream-source`: original qBittorrent reference.
- `feature/*`: reviewable development work.

Our code is built independently; individual upstream GUI resources are
selectively imported and attributed, without merging the complete upstream
application.

- `src/api/`: remote server URL and (future) Web API client.
- `src/gui/`: native Qt Widgets interface.
- `tests/`: functional application tests registered through CTest.

## License

GPL-3.0-or-later. Reused upstream components retain their original copyright
notices and applicable license terms. See `LICENSE`.
