# Windows x64 developer build and self-contained EXE installer

This procedure builds **the same native Qt/CMake application** tested on
Arch Linux. The first supported toolchain is MSYS2 **UCRT64**, not Visual
Studio/MSVC and not the legacy MSYS/MINGW64 environment. Windows 10 or 11
x64 is expected.

## 1. Install the UCRT64 build environment

Install [MSYS2](https://www.msys2.org/) and launch **MSYS2 UCRT64** from
the Windows Start menu. In that terminal, update MSYS2:

```bash
pacman -Syu
```

If it asks you to close/restart MSYS2, do so. Repeat `pacman -Syu` until
fully updated. In **MSYS2 UCRT64**, install:

```bash
pacman -S --needed \
  git \
  mingw-w64-ucrt-x86_64-gcc \
  mingw-w64-ucrt-x86_64-cmake \
  mingw-w64-ucrt-x86_64-ninja \
  mingw-w64-ucrt-x86_64-qt6-base \
  mingw-w64-ucrt-x86_64-qt6-svg \
  mingw-w64-ucrt-x86_64-qtkeychain \
  mingw-w64-ucrt-x86_64-openssl \
  mingw-w64-ucrt-x86_64-nsis
```

QtKeychain 0.18 is provided as `mingw-w64-ucrt-x86_64-qtkeychain`.
It integrates with Windows protected credential storage without GNOME Keyring.

## 2. Obtain the branch and build

In the **same UCRT64 terminal**:

```bash
git clone https://github.com/username227/qBittorrent-remote.git
cd qBittorrent-remote
git fetch origin
git switch -c feature/windows-build-packaging --track origin/feature/windows-build-packaging
bash packaging/windows/build-installer.sh
```

If you already have a Windows checkout, use `git fetch origin`,
switch to the Windows branch, and update normally with `git pull --ff-only`
rather than cloning again.

The script checks its environment, configures and builds in Release mode,
runs **all four CTest workflows**, stages the executable, uses `windeployqt6`
for Qt DLLs and plugins, explicitly bundles QtSvg, qsvgicon.dll and qsvg.dll
so all original qBittorrent SVG icons render, copies QtKeychain and other
imported UCRT64 runtime DLLs, packages the Windows-native **Schannel** TLS backend, and creates
an **NSIS EXE installer**.

For individual build/test commands:

```bash
cmake --preset windows-ucrt
cmake --build --preset windows-ucrt
QT_QPA_PLATFORM=offscreen ctest --preset windows-ucrt --output-on-failure
```

If tests fail, stop and report the output instead of installing the build.

## 3. Install and test outside the development environment

The installer is at:

```text
build/windows-ucrt/dist/qBittorrent-Remote-0.1.0-Windows-x64-Setup.exe
```

Double-click it in Windows Explorer. It installs per-user under:

```text
%LOCALAPPDATA%\Programs\qBittorrent Remote
```

The installer bundles the Qt libraries/plugins, QtKeychain and applicable
MinGW runtime DLLs. It uses the built-in Windows Schannel TLS provider,
not a separately installed OpenSSL runtime. It does **not** require MSYS2,
Qt, or CMake on the target PC. It creates Start Menu shortcuts and an
uninstaller. Uninstalling does **not** erase server profiles or Windows
credentials; those remain under the user's account.

**Run the installed application from the Start menu, not the build folder:**
that catches missing bundled DLLs or plugins.

Suggested manual checks:

1. Open the app from the Start menu. Verify the native window renders.
2. Connect to both known HTTPS QNAP endpoints *one at a time*: confirm
   certificate fingerprint approval and successful login/version.
   Simultaneous multi-server tabs are not implemented in this milestone.
3. Disconnect and reconnect: the same certificate should not reprompt.
4. With password-required login enabled, select **Remember password
   securely**, enter the password, and connect successfully.
5. Close the app entirely, reopen it from Start, and connect with a
   blank password field. It should use Windows Credential Manager;
   Linux passwords do not transfer.
6. Inspect the entry in **Credential Manager → Windows Credentials**
   if desired, without disclosing passwords.
7. Uncheck Remember password and save; verify the old password is
   erased and blank-password login now fails.
8. For a changed certificate, decline trust before attempting approval.
   Do not modify production certificates merely to run a regression.

HTTPS and certificate validation are independent of credential storage:
the app never enables unencrypted QtKeychain fallback.

## GitHub Actions

The workflow in `.github/workflows/windows-installer.yml` runs a UCRT64
Windows build and all four CTest workflows, then uploads the generated
installer as a workflow artifact. This is a **testing** artifact, not a
signed production release; Windows SmartScreen may warn about an
unsigned installer.

## Build/install scope

This is deliberately a development packaging milestone. Code signing,
installer update/repair handling, third-party license auditing, and
future version naming remain release-readiness work.

The Windows branch builds on the unmerged
`feature/remote-connection-foundation` branch. Keep PR #3 untouched
until reviewed, and treat this Windows packaging as a separate change.
