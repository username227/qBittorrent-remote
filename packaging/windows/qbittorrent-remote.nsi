; SPDX-License-Identifier: GPL-3.0-or-later
; Per-user NSIS installer of the staged self-contained UCRT64 deployment.
Unicode true
!include "MUI2.nsh"

!ifndef PAYLOAD_DIR
  !error "Provide -DPAYLOAD_DIR=<Windows deployment directory>"
!endif
!ifndef OUTPUT_FILE
  !error "Provide -DOUTPUT_FILE=<Windows installer filename>"
!endif

Name "qBittorrent Remote"
OutFile "${OUTPUT_FILE}"
InstallDir "$LOCALAPPDATA\Programs\qBittorrent Remote"
RequestExecutionLevel user
SetCompressor /SOLID lzma
BrandingText "qBittorrent Remote"
VIProductVersion "0.1.0.0"
VIAddVersionKey "ProductName" "qBittorrent Remote"
VIAddVersionKey "FileDescription" "qBittorrent Remote installer"

!define MUI_ABORTWARNING
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "English"

Section "Install"
  SetOutPath "$INSTDIR"
  File /r "${PAYLOAD_DIR}\*.*"
  WriteUninstaller "$INSTDIR\Uninstall.exe"
  CreateDirectory "$SMPROGRAMS\qBittorrent Remote"
  CreateShortCut "$SMPROGRAMS\qBittorrent Remote\qBittorrent Remote.lnk" "$INSTDIR\qbittorrent-remote.exe"
  CreateShortCut "$SMPROGRAMS\qBittorrent Remote\Uninstall.lnk" "$INSTDIR\Uninstall.exe"

  WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\qBittorrent Remote" "DisplayName" "qBittorrent Remote"
  WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\qBittorrent Remote" "DisplayVersion" "0.1.0"
  WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\qBittorrent Remote" "Publisher" "qBittorrent Remote"
  WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\qBittorrent Remote" "InstallLocation" "$INSTDIR"
  WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\qBittorrent Remote" "UninstallString" '"$INSTDIR\Uninstall.exe"'
  WriteRegDWORD HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\qBittorrent Remote" "NoModify" 1
  WriteRegDWORD HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\qBittorrent Remote" "NoRepair" 1
SectionEnd

Section "Uninstall"
  Delete "$SMPROGRAMS\qBittorrent Remote\qBittorrent Remote.lnk"
  Delete "$SMPROGRAMS\qBittorrent Remote\Uninstall.lnk"
  RMDir "$SMPROGRAMS\qBittorrent Remote"
  ; No settings or Windows Credential Manager credentials are removed.
  RMDir /r "$INSTDIR"
  DeleteRegKey HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\qBittorrent Remote"
SectionEnd
