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
- Qt 6.6 or newer (Core, Widgets, Network, Svg, Multimedia, DBus on Linux and Test when tests are enabled)
- Arch Linux: `qt6-base`, `qt6-svg`, `qt6-multimedia` (with a working audio backend)
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

### Linux desktop integration

`cmake --install` installs the application's own
`qbittorrent-remote.desktop` in the standard applications directory and its
teal SVG icon in the hicolor scalable apps directory, respecting GNUInstallDirs
and DESTDIR for packaging. Packages should not synthesize another desktop
entry or icon. The desktop entry declares `application/x-bittorrent` and
`x-scheme-handler/magnet` so file managers can discover it. Registration does
not set or override defaults; external file/magnet activation and the
Preferences association controls remain a separate milestone. Until external
activation is implemented, choosing this application as the default will
launch it but not add the passed torrent or magnet.

The Linux CTest installation workflow validates a staged installation and,
when desktop-file-utils and GIO are available, checks handler discovery in
an isolated XDG environment. Linux CI installs those utilities.

### Testing policy

**CTest checks real application functionality and user-visible workflows.**
Tests should confirm observable outcomes (for example, opening the application,
connecting to a server, or completing a torrent-management operation). They
must not be written around individual code blocks, internal implementation
details, or fixed example inputs merely to exercise a parser or function.

Qt Test may drive GUI interactions but is invoked exclusively through CTest.
The functional suites share one linked `qbittorrent_remote_tests` executable;
CTest still launches each registered workflow in a separate process.

All three test presets (development, release, Windows UCRT64) run up to
four CTest workflow processes concurrently. Linux CI and the Windows
installer build already use those presets and inherit the same setting.
This changes scheduling only: all registered tests and their timeouts
remain intact. Override with `ctest --preset dev --parallel 1` for
serial diagnosis, or choose a different job count on your machine.
For packaging or manual builds that do not use a preset, pass
`ctest --test-dir <build-dir> --parallel 4 --output-on-failure` in the
package check step.

### Windows installer and cross-platform CI

A native Windows x64 installer can be built from **MSYS2 UCRT64** using the
same Qt 6/CMake/CTest source tree. It packages Qt, QtKeychain and compiler
runtime DLLs, Qt SVG icon-rendering plugins, the Windows platform plugin, and the native Schannel TLS
backend. Windows Credential Manager is used for protected password memory.

Follow [the Windows build and installer guide](docs/windows-build.md).
The build/installer workflow also runs in Windows GitHub Actions; the draft
must be validated on a real Windows computer before distribution.

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
username and an optional password. **Save** stores nonsecret profile fields,
including the optional Remember-password choice, in Qt's per-user settings.
When built with QtKeychain, selecting **Remember password securely** saves
the credential only through the operating system's protected store (GNOME
Keyring/Secret Service, KWallet, Windows Credential Manager or macOS
Keychain). The program never writes passwords into QSettings and never
enables QtKeychain's plaintext fallback. If QtKeychain is not available at
build time, the app still works and the Remember-password option is
disabled. A blank password is valid for servers that permit passwordless
access. **Connect** performs a real Web API login, followed by
a request for the remote qBittorrent version and Web API version.

On successful login, a newly entered password is saved only if Remember
password is enabled. On the next manual connection, leaving the password
field empty retrieves the protected password automatically. Unchecking
Remember password forgets the old credential; removing a profile or
changing its URL/username also invalidates the old credential. A locked
or inaccessible keyring is reported as an error, not silently replaced
with plaintext storage.

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
certificate should identify the requested address in its Subject Alternative
Name (SAN). For an IP-based connection, a legacy private certificate with
**no SAN at all** may use an exact matching IP Common Name. A conflicting
SAN never falls back to Common Name. If the server certificate does not
match the address (as with some default NAS certificates), the dialog
explicitly warns of the identity mismatch and offers an alternative:
verify and pin that exact SHA-256 fingerprint for the saved endpoint.
Reject is the default. Never approve an unverified fingerprint, including
a generic certificate shared by multiple devices. Certificate expiry,
revocation and unrelated reported TLS verification errors cannot be
overridden, even with a stored pin. Trust is invalidated on profile URL
change or removal.

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
and explicit hostname-mismatch approval/rejection, endpoint isolation and expired-certificate rejection. Test-only certificates are generated at
runtime and **no TLS private key is committed**. The test needs the
`openssl` command-line utility available on `PATH`.
A credential-storage CTest workflow exercises remembering, reuse, opting
out and passwordless login through the actual application UI using a
disposable fake vault; it never modifies your personal keyring.

**Future multiple-instance behavior:** saved profiles and active sessions are
distinct; there is no arbitrary connection limit. When remote connectivity
exists, zero active connections will show a connection screen, one active
connection will use the regular interface without server tabs, and two or more
active connections will show dynamic tabs. No premature welcome screen or
placeholder server tabs are included in this GUI milestone.

**Planned startup behavior (not implemented):** Profiles will have an
independent **Connect automatically** option. On launch, connect to those
profiles (not all saved profiles), using blank credentials when allowed or
their protected stored passwords, and show the actual torrent view after
synchronization. One connected server gets no tab strip; two or more get
instance tabs. Startup failures must not prevent other saved servers from
connecting. Do not mistake a saved profile for an active server.

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
