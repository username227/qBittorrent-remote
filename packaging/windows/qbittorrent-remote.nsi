; SPDX-License-Identifier: GPL-3.0-or-later
; Windows UCRT64 installer: explicit GPL wizard and per-user/per-machine scope.
; Never move or remove user-specific QSettings or Credential Manager entries.
Unicode true

!ifndef PAYLOAD_DIR
  !error "Provide -DPAYLOAD_DIR=<Windows deployment directory>"
!endif
!ifndef OUTPUT_FILE
  !error "Provide -DOUTPUT_FILE=<Windows installer filename>"
!endif

!define UNINSTKEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\qBittorrent Remote"
!define MUI_ICON "${__FILEDIR__}\..\..\src\gui\icons\qbittorrent.ico"
!define MUI_UNICON "${__FILEDIR__}\..\..\src\gui\icons\qbittorrent.ico"

; Per the requested behavior, ask for elevation *before* showing the wizard.
; Note that a Current User installation still runs in an elevated process,
; and supplying a different admin account at UAC changes which HKCU is used.
!define MULTIUSER_EXECUTIONLEVEL Admin
!define MULTIUSER_MUI
!define MULTIUSER_USE_PROGRAMFILES64
!define MULTIUSER_INSTALLMODE_DEFAULT_CURRENTUSER
!define MULTIUSER_INSTALLMODE_DEFAULT_REGISTRY_KEY "${UNINSTKEY}"
!define MULTIUSER_INSTALLMODE_DEFAULT_REGISTRY_VALUENAME "DisplayName"
!define MULTIUSER_INSTALLMODE_INSTDIR "qBittorrent Remote"
!define MULTIUSER_INSTALLMODE_INSTDIR_REGISTRY_KEY "${UNINSTKEY}"
!define MULTIUSER_INSTALLMODE_INSTDIR_REGISTRY_VALUENAME "InstallLocation"
!define MULTIUSER_INSTALLMODE_COMMANDLINE
!define MULTIUSER_INSTALLMODEPAGE_SHOWUSERNAME
!define MULTIUSER_INSTALLMODEPAGE_TEXT_TOP "Choose who can use qBittorrent Remote. Settings and passwords stay in each Windows user's profile regardless of this choice."
!define MULTIUSER_PAGE_CUSTOMFUNCTION_LEAVE CheckScopeSwitch

!include "MultiUser.nsh"
!include "MUI2.nsh"

Name "qBittorrent Remote"
OutFile "${OUTPUT_FILE}"
InstallDir "$LOCALAPPDATA\Programs\qBittorrent Remote"
SetCompressor /SOLID lzma
BrandingText "qBittorrent Remote"
VIProductVersion "0.1.0.0"
VIAddVersionKey "ProductName" "qBittorrent Remote"
VIAddVersionKey "FileDescription" "qBittorrent Remote installer"

Var OldScopePath
Var OldScopeMode
Var OldScopeApproved

!include "FileFunc.nsh"
!include "LogicLib.nsh"

!define MUI_ABORTWARNING
!define MUI_LICENSEPAGE_RADIOBUTTONS
!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_LICENSE "${PAYLOAD_DIR}\LICENSE.txt"
!insertmacro MULTIUSER_PAGE_INSTALLMODE
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
; The user decides whether to start the app; skip automatically in silent CI.
; Use the desktop shell for launch rather than launching a child of the
; elevated setup executable directly.
!define MUI_FINISHPAGE_RUN
!define MUI_FINISHPAGE_RUN_TEXT "Start qBittorrent Remote now"
!define MUI_FINISHPAGE_RUN_NOTCHECKED
!define MUI_FINISHPAGE_RUN_FUNCTION LaunchFromDesktopShell
!insertmacro MUI_PAGE_FINISH

!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "English"

Function .onInit
  SetRegView 64
  !insertmacro MULTIUSER_INIT
FunctionEnd

Function un.onInit
  SetRegView 64
  !insertmacro MULTIUSER_UNINIT
FunctionEnd

; Cross-scope migration is opt-in. Do not invoke an old, user-writable
; Uninstall.exe elevated: it can be replaced by a non-admin user.
; We remove only a verified installation at our exact default location
; using this installer's own code, not a user-controlled registry path.
Function CheckScopeSwitch
  StrCpy $0 ""
  StrCpy $1 ""
  ${If} $MultiUser.InstallMode == "AllUsers"
    ReadRegStr $0 HKCU "${UNINSTKEY}" "InstallLocation"
    ReadRegStr $1 HKCU "${UNINSTKEY}" "DisplayName"
    StrCpy $OldScopeMode "CurrentUser"
    SetShellVarContext current
    StrCpy $R2 "$LOCALAPPDATA\Programs\qBittorrent Remote"
    SetShellVarContext all
  ${Else}
    ReadRegStr $0 HKLM "${UNINSTKEY}" "InstallLocation"
    ReadRegStr $1 HKLM "${UNINSTKEY}" "DisplayName"
    StrCpy $OldScopeMode "AllUsers"
    StrCpy $R2 "$PROGRAMFILES64\qBittorrent Remote"
  ${EndIf}
  ${If} $0 == ""
    StrCpy $OldScopePath ""
    StrCpy $OldScopeApproved ""
    Return
  ${EndIf}

  ; Reject arbitrary locations from mutable uninstall registry keys.
  ${If} $0 != $R2
    IfSilent 0 +3
      SetErrorLevel 3
      Quit
    MessageBox MB_OK|MB_ICONEXCLAMATION "An existing $OldScopeMode installation uses a nonstandard directory:$\r$\n$0$\r$\n$\r$\nFor safety, please uninstall it manually before changing installation scope."
    SetErrorLevel 3
    Abort
  ${EndIf}
  ${If} $1 != "qBittorrent Remote"
    IfSilent 0 +3
      SetErrorLevel 3
      Quit
    MessageBox MB_OK|MB_ICONSTOP "Existing installation identity could not be verified. Please uninstall it manually."
    SetErrorLevel 3
    Abort
  ${EndIf}
  IfFileExists "$0\qbittorrent-remote.exe" +2 0
    Goto identity_error
  IfFileExists "$0\Uninstall.exe" +2 0
    Goto identity_error

  ; Do not recurse through a root symlink/junction.
  System::Call 'kernel32::GetFileAttributesW(w "$0") i.r3'
  IntOp $R4 $3 & 0x400
  ${If} $3 == -1
  ${OrIf} $R4 != 0
    IfSilent 0 +3
      SetErrorLevel 3
      Quit
    MessageBox MB_OK|MB_ICONSTOP "The previous install directory is missing or is a filesystem link. Automatic removal is unavailable."
    SetErrorLevel 3
    Abort
  ${EndIf}

  ${If} $OldScopeApproved == "yes"
  ${AndIf} $OldScopePath == $0
    Return
  ${EndIf}
  StrCpy $OldScopeApproved ""
  StrCpy $OldScopePath "$0"
  IfSilent check_silent
  MessageBox MB_YESNO|MB_ICONQUESTION|MB_DEFBUTTON2 "A $OldScopeMode installation already exists at:$\r$\n$0$\r$\n$\r$\nRemove that application automatically and continue with the new $MultiUser.InstallMode installation?$\r$\n$\r$\nSaved servers, passwords and settings are retained." IDYES approved
  Abort ; No means return to the scope picker without removing anything.
check_silent:
  ${GetParameters} $R5
  ClearErrors
  ${GetOptions} $R5 "/AutoRemoveOldScope" $R6
  IfErrors 0 approved
  SetErrorLevel 3
  Quit ; Changing install scope in silent mode requires /AutoRemoveOldScope.
approved:
  StrCpy $OldScopeApproved "yes"
  Return
identity_error:
  IfSilent 0 +3
    SetErrorLevel 3
    Quit
  MessageBox MB_OK|MB_ICONSTOP "Previous qBittorrent Remote executable/uninstaller is missing. Please uninstall the old copy manually."
  SetErrorLevel 3
  Abort
FunctionEnd

Function RemovePreviousScope
  ${If} $OldScopeApproved != "yes"
    Return
  ${EndIf}
  ; Re-read the registry just before removal to avoid stale decisions.
  ${If} $OldScopeMode == "CurrentUser"
    ReadRegStr $0 HKCU "${UNINSTKEY}" "InstallLocation"
    SetShellVarContext current
    StrCpy $R2 "$LOCALAPPDATA\Programs\qBittorrent Remote"
    SetShellVarContext all
  ${Else}
    ReadRegStr $0 HKLM "${UNINSTKEY}" "InstallLocation"
    StrCpy $R2 "$PROGRAMFILES64\qBittorrent Remote"
  ${EndIf}
  ${If} $0 != $OldScopePath
  ${OrIf} $0 != $R2
    SetErrorLevel 3
    IfSilent 0 +2
      Quit
    Abort "Existing installation changed; automatic removal cancelled."
  ${EndIf}
  ; Recheck the root immediately before the destructive operation.
  ; A user-writable tree still needs a security review before release.
  System::Call 'kernel32::GetFileAttributesW(w "$OldScopePath") i.r3'
  IntOp $R4 $3 & 0x400
  ${If} $3 == -1
  ${OrIf} $R4 != 0
    Goto removal_error
  ${EndIf}
  SetOutPath "$TEMP"
  ClearErrors
  RMDir /r "$OldScopePath"
  IfErrors removal_error
  IfFileExists "$OldScopePath" removal_error 0

  ${If} $OldScopeMode == "CurrentUser"
    SetShellVarContext current
    DeleteRegKey HKCU "${UNINSTKEY}"
  ${Else}
    SetShellVarContext all
    DeleteRegKey HKLM "${UNINSTKEY}"
  ${EndIf}
  Delete "$SMPROGRAMS\qBittorrent Remote\qBittorrent Remote.lnk"
  Delete "$SMPROGRAMS\qBittorrent Remote\Uninstall.lnk"
  RMDir "$SMPROGRAMS\qBittorrent Remote"
  ; Restore the newly selected scope for normal install.
  ${If} $MultiUser.InstallMode == "AllUsers"
    SetShellVarContext all
  ${Else}
    SetShellVarContext current
  ${EndIf}
  Return
removal_error:
  SetErrorLevel 3
  IfSilent 0 +2
    Quit
  Abort "Could not remove the previous application (is it still running?). The new installation was not started."
FunctionEnd

Function LaunchFromDesktopShell
  ; Explorer normally runs under the interactive unelevated shell token.
  ; Do not spawn the torrent client directly as a child of elevated setup.
  ExecShell "open" "$WINDIR\explorer.exe" '"$INSTDIR\qbittorrent-remote.exe"'
FunctionEnd

Section "Install"
  ; Recheck for silent (/S) installation: wizard callbacks are skipped then.
  Call CheckScopeSwitch
  Call RemovePreviousScope

  ; Scope was selected on the preceding wizard page. SetShellVarContext and
  ; $INSTDIR are managed by MultiUser.nsh (and preserve old per-user paths).
  SetOutPath "$INSTDIR"
  File /r "${PAYLOAD_DIR}\*.*"
  WriteUninstaller "$INSTDIR\Uninstall.exe"

  CreateDirectory "$SMPROGRAMS\qBittorrent Remote"
  CreateShortCut "$SMPROGRAMS\qBittorrent Remote\qBittorrent Remote.lnk" "$INSTDIR\qbittorrent-remote.exe" "" "$INSTDIR\qbittorrent-remote.exe" 0
  CreateShortCut "$SMPROGRAMS\qBittorrent Remote\Uninstall.lnk" "$INSTDIR\Uninstall.exe" "/$MultiUser.InstallMode"

  ; ShCtx selects HKCU for Current user and HKLM for All users.
  WriteRegStr ShCtx "${UNINSTKEY}" "DisplayName" "qBittorrent Remote"
  WriteRegStr ShCtx "${UNINSTKEY}" "DisplayVersion" "0.1.0"
  WriteRegStr ShCtx "${UNINSTKEY}" "Publisher" "qBittorrent Remote"
  WriteRegStr ShCtx "${UNINSTKEY}" "InstallLocation" "$INSTDIR"
  WriteRegStr ShCtx "${UNINSTKEY}" "DisplayIcon" "$INSTDIR\qbittorrent-remote.exe"
  WriteRegStr ShCtx "${UNINSTKEY}" "UninstallString" '"$INSTDIR\Uninstall.exe" /$MultiUser.InstallMode'
  WriteRegStr ShCtx "${UNINSTKEY}" "QuietUninstallString" '"$INSTDIR\Uninstall.exe" /$MultiUser.InstallMode /S'
  WriteRegDWORD ShCtx "${UNINSTKEY}" "NoModify" 1
  WriteRegDWORD ShCtx "${UNINSTKEY}" "NoRepair" 1
SectionEnd

Section "Uninstall"
  ; MultiUser.nsh restores HKCU/HKLM and shell shortcut scope using the
  ; /CurrentUser or /AllUsers flag written to each uninstall command.
  Delete "$SMPROGRAMS\qBittorrent Remote\qBittorrent Remote.lnk"
  Delete "$SMPROGRAMS\qBittorrent Remote\Uninstall.lnk"
  RMDir "$SMPROGRAMS\qBittorrent Remote"

  ; The installed payload is a self-contained application directory.
  ; Never touch QSettings or any Windows Credential Manager records.
  RMDir /r "$INSTDIR"
  DeleteRegKey ShCtx "${UNINSTKEY}"
SectionEnd
