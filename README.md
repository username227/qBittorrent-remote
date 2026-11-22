# qBittorrent Remote

An independent, cross-platform Qt 6 desktop application for controlling
remote qBittorrent instances through their Web API.

**Current status:** early development. The first milestone is a native window
and server-address foundation; the bootstrap does **not** connect to a server
yet. No local torrent engine or libtorrent dependency is included.

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
```

A release configuration is also available:

```sh
cmake --preset release
cmake --build --preset release
ctest --preset release
```

Or configure manually with any supported CMake generator. Standard
`BUILD_TESTING=OFF` disables the test targets; tests default to enabled.

### Testing policy

**CTest checks real application functionality and user-visible workflows.**
Tests should confirm observable outcomes (for example, opening the application,
connecting to a server, or completing a torrent-management operation). They
must not be written around individual code blocks, internal implementation
details, or fixed example inputs merely to exercise a parser or function.

Qt Test may be used to drive GUI interactions, but is invoked exclusively
through CTest. The initial functional smoke test opens the real application
window and uses the File > Quit action. Linux runs this test with Qt's
offscreen platform so a graphical session is not required.

## Repository and architecture

- `master`: independent application.
- `upstream-source`: untouched qBittorrent source reference.
- `feature/*`: reviewable development work.

Our code is built independently; individual upstream GUI resources will be
selectively imported and attributed as needed. We do not merge the complete
upstream application into `master`.

- `src/api/`: remote server URL and (future) Web API client.
- `src/gui/`: native Qt Widgets interface.
- `tests/`: functional application tests registered through CTest.

## License

GPL-3.0-or-later. Reused upstream components retain their original copyright
notices and applicable license terms. See `LICENSE`.
