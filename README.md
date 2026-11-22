# qBittorrent Remote

An independent, cross-platform Qt 6 desktop application for controlling
remote qBittorrent instances through their Web API.

**Current status:** native GUI preview, without remote connectivity or
torrent management. No local torrent engine or libtorrent dependency.

## Build and test

CMake is the **sole build definition**, and CTest is the **sole test runner**.
There are no alternate build scripts or independent test commands.

Requirements:
- CMake 3.21 or newer
- C++17 compiler
- Qt 6.6 or newer (Core, Widgets, and Test when tests are enabled)
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
filtering remain disabled until the remote API implementation exists.

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
