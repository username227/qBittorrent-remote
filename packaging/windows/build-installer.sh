#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Run only inside an MSYS2 UCRT64 shell on Windows.
set -euo pipefail

if [[ "$(printenv MSYSTEM || true)" != UCRT64 ]]; then
  echo "ERROR: Open the MSYS2 UCRT64 terminal (not MSYS/MINGW64/Git Bash)." >&2
  exit 2
fi

for tool in cmake ctest ninja windeployqt6 makensis objdump cygpath; do
  command -v "$tool" >/dev/null || { echo "Missing tool: $tool" >&2; exit 2; }
done

source_root="$(cd -- "$(dirname -- "$0")/../.." && pwd)"
build_root="$source_root/build/windows-ucrt"
deploy="$build_root/deploy"
dist="$build_root/dist"
exe="$build_root/qbittorrent-remote.exe"

cd "$source_root"
echo "Configure started: $(date -u +%FT%TZ)"
cmake --preset windows-ucrt
echo "Build started: $(date -u +%FT%TZ)"
cmake --build --preset windows-ucrt
echo "CTest started: $(date -u +%FT%TZ)"
# QtTest GUI workflows run offscreen and never access real Windows secrets.
# Always emit the Qt GUI workflow output on Windows; Qt failures in an
# offscreen test can otherwise appear as only "***Failed" in Actions logs.
QT_QPA_PLATFORM=offscreen ctest --preset windows-ucrt --output-on-failure --verbose

echo "Deployment started: $(date -u +%FT%TZ)"
test -f "$exe" || { echo "Windows executable not generated: $exe" >&2; exit 1; }

# Never reuse old runtime DLLs or plugins from a prior installer.
rm -rf -- "$deploy"
mkdir -p -- "$deploy" "$dist"
cp -- "$exe" "$deploy/"
cp -- "$source_root/LICENSE" "$deploy/LICENSE.txt"
cp -- "$source_root/packaging/windows/THIRD-PARTY-NOTICES.txt" "$deploy/"

# Qt runtime DLLs, platforms, image formats, styles and plugins.
windeployqt6 --release --no-translations --dir "$deploy" "$deploy/qbittorrent-remote.exe"
# QtKeychain is a third-party DLL, not a standard Qt module.
cp -- /ucrt64/bin/libqt6keychain.dll "$deploy/"

# The qBittorrent UI uses original SVG icons from Qt resources. Qt's SVG
# icon engine is a runtime plugin; a binary can pass CTest without it and
# silently show text-only toolbar actions. Bundle and verify both plugins.
mkdir -p -- "$deploy/iconengines" "$deploy/imageformats"
cp -- /ucrt64/bin/Qt6Svg.dll "$deploy/"
cp -- /ucrt64/share/qt6/plugins/iconengines/qsvgicon.dll "$deploy/iconengines/"
cp -- /ucrt64/share/qt6/plugins/imageformats/qsvg.dll "$deploy/imageformats/"

# Use the Windows native Schannel TLS backend, not OpenSSL bundled libraries.
# This does NOT disable hostname, certificate or pin verification.
mkdir -p -- "$deploy/tls"
cp -- /ucrt64/share/qt6/plugins/tls/qschannelbackend.dll "$deploy/tls/"
rm -f -- "$deploy/tls/qopensslbackend.dll" "$deploy/tls/qcertonlybackend.dll"

# Bundle transitive MinGW/Qt dependency DLLs, including dependencies of
# plugins and QtKeychain. Leave Windows system DLLs on Windows itself.
# Scan each binary once, in bounded batches, instead of starting a new
# objdump/awk pair for every DLL on every pass. Keep the same transitive
# closure, including dependencies of plugins in subdirectories.
echo "Dependency scan started: $(date -u +%FT%TZ)"
mapfile -d '' -t pending < <(find "$deploy" -type f \( -iname '*.exe' -o -iname '*.dll' \) -print0)
while ((${#pending[@]})); do
  next=()
  for ((offset=0; offset<${#pending[@]}; offset+=16)); do
    # Bound Windows command-line length; pipefail makes inspection failures
    # fatal rather than silently producing an incomplete installer.
    dependencies="$(objdump -p "${pending[@]:offset:16}" | awk '$1=="DLL" && $2=="Name:" {print $3}' | sort -u)"
    while IFS= read -r dependency; do
      [[ -z "$dependency" || -f "$deploy/$dependency" ]] && continue
      if [[ -f "/ucrt64/bin/$dependency" ]]; then
        cp -- "/ucrt64/bin/$dependency" "$deploy/"
        next+=("$deploy/$dependency")
      fi
    done <<< "$dependencies"
  done
  pending=("${next[@]}")
done
echo "Dependency scan completed: $(date -u +%FT%TZ)"

for essential in \
  "$deploy/Qt6Core.dll" \
  "$deploy/Qt6Widgets.dll" \
  "$deploy/Qt6Network.dll" \
  "$deploy/Qt6Svg.dll" \
  "$deploy/Qt6Multimedia.dll" \
  "$deploy/iconengines/qsvgicon.dll" \
  "$deploy/imageformats/qsvg.dll" \
  "$deploy/libqt6keychain.dll" \
  "$deploy/platforms/qwindows.dll" \
  "$deploy/tls/qschannelbackend.dll"; do
  if [[ ! -f "$essential" ]]; then
    echo "Missing required bundled runtime: $essential" >&2
    exit 1
  fi
done

# MSYS2's license text files, if provided by the package.
mkdir -p -- "$deploy/licenses"
for directory in /ucrt64/share/licenses/qt6-base /ucrt64/share/licenses/qt6-svg /ucrt64/share/licenses/qt6-multimedia /ucrt64/share/licenses/qtkeychain; do
  if [[ -d "$directory" ]]; then
    cp -R -- "$directory" "$deploy/licenses/"
  fi
done

installer="$dist/qBittorrent-Remote-0.1.0-Windows-x64-Setup.exe"
echo "Installer compression started: $(date -u +%FT%TZ)"
makensis \
  "-DPAYLOAD_DIR=$(cygpath -w "$deploy")" \
  "-DOUTPUT_FILE=$(cygpath -w "$installer")" \
  "$source_root/packaging/windows/qbittorrent-remote.nsi"

test -s "$installer" || { echo "NSIS did not create the installer." >&2; exit 1; }
echo "Installer ready: $installer"
echo "Test the installed application outside MSYS2, including TLS and Credential Manager."
