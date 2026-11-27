# SPDX-License-Identifier: GPL-3.0-or-later
# Run on a disposable Windows CI runner, after packaging the real NSIS EXE.
# Exercise the installed payload and registry, not the installer source text.
param(
    [Parameter(Mandatory = $true)]
    [string]$Installer
)
$ErrorActionPreference = "Stop"

$installerPath = (Resolve-Path $Installer).Path
$perUserKey = "HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\qBittorrent Remote"
$allUsersKey = "HKLM:\Software\Microsoft\Windows\CurrentVersion\Uninstall\qBittorrent Remote"
$settingsKey = "HKCU:\Software\qBittorrent Remote\qBittorrent Remote"
$sentinelName = "WindowsInstallerScopeSmokeSentinel"
$sentinelValue = "scope-switch-retains-personal-settings"

# NSIS uninstallers normally copy themselves and start a detached child.
# Run a temporary copy with _?= as documented by NSIS so that the process
# we wait for is the one doing the actual uninstall. Only CI-owned paths
# created by this test are used here.
function Run-Setup([string]$file, [string]$scope, [bool]$autoRemove = $false, [int]$expectedExit = 0, [string]$uninstallDir = "") {
    $arguments = "/S /$scope"
    if ($autoRemove) { $arguments += " /AutoRemoveOldScope" }
    if ($uninstallDir) { $arguments += " _?=$uninstallDir" }
    Write-Host "Starting $file $arguments (expected exit $expectedExit)"
    $process = Start-Process -FilePath $file -ArgumentList $arguments -PassThru
    try {
        if (-not $process.WaitForExit(90000)) {
            Write-Host "Timed out: PID $($process.Id)"
            & taskkill.exe /PID $process.Id /T /F | Out-Host
            throw "Installer/uninstaller exceeded 90 seconds: $file $arguments"
        }
        Write-Host "Exited with code $($process.ExitCode)"
        if ($process.ExitCode -ne $expectedExit) {
            throw "Unexpected exit code from $file $arguments (expected $expectedExit, got $($process.ExitCode))."
        }
    }
    finally { $process.Dispose() }
}

function Run-Uninstall([string]$dir, [string]$scope) {
    $temporary = Join-Path ([System.IO.Path]::GetTempPath()) ("qbt-uninstall-" + [guid]::NewGuid().ToString("N") + ".exe")
    Copy-Item -LiteralPath (Join-Path $dir "Uninstall.exe") -Destination $temporary
    try { Run-Setup $temporary $scope $false 0 $dir }
    finally { Remove-Item -LiteralPath $temporary -Force -ErrorAction SilentlyContinue }
    if (Test-Path -LiteralPath $dir) { throw "Uninstaller left application directory: $dir" }
}

$principal = [Security.Principal.WindowsPrincipal]::new([Security.Principal.WindowsIdentity]::GetCurrent())
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw "Installer CI must already be elevated; refusing to trigger a UAC prompt."
}

function Assert-Installed([string]$registryKey, [string]$oppositeKey) {
    if (-not (Test-Path $registryKey)) {
        throw "Missing uninstall registration: $registryKey"
    }
    if (Test-Path $oppositeKey) {
        throw "Unexpected installation in opposite scope: $oppositeKey"
    }
    $dir = (Get-ItemProperty -LiteralPath $registryKey).InstallLocation
    if (-not $dir -or -not (Test-Path (Join-Path $dir "qbittorrent-remote.exe"))) {
        throw "Installed application missing at: $dir"
    }
    if (-not (Test-Path (Join-Path $dir "Uninstall.exe"))) {
        throw "Missing uninstaller at: $dir"
    }
    return $dir
}

if ((Test-Path $perUserKey) -or (Test-Path $allUsersKey)) {
    throw "CI runner unexpectedly has an existing qBittorrent Remote installation."
}

# QSettings on Windows uses HKCU regardless of per-user/system-wide install.
New-Item -Path $settingsKey -Force | Out-Null
New-ItemProperty -Path $settingsKey -Name $sentinelName -Value $sentinelValue -PropertyType String -Force | Out-Null

try {
    Write-Host "Functional installer smoke test: Current user"
    Run-Setup $installerPath "CurrentUser"
    $userDir = Assert-Installed $perUserKey $allUsersKey
    $expectedUserDir = Join-Path $env:LOCALAPPDATA "Programs\qBittorrent Remote"
    if ($userDir -ne $expectedUserDir) { throw "Unexpected per-user default: $userDir; expected $expectedUserDir" }

    # Upgrade the same scope, verifying it doesn't redirect the install path.
    Run-Setup $installerPath "CurrentUser"
    $upgradedDir = Assert-Installed $perUserKey $allUsersKey
    if ($upgradedDir -ne $userDir) {
        throw "Same-scope upgrade changed the installation location."
    }

    Write-Host "Functional installer smoke test: reject unapproved scope switch"
    Run-Setup $installerPath "AllUsers" $false 3
    $unchangedDir = Assert-Installed $perUserKey $allUsersKey
    if ($unchangedDir -ne $userDir) { throw "Rejected migration changed install location." }

    Write-Host "Functional installer smoke test: automatic Current user -> All users migration"
    Run-Setup $installerPath "AllUsers" $true
    if (Test-Path $perUserKey) { throw "Automatic migration left current-user uninstall registration." }
    if (Test-Path $userDir) { throw "Automatic migration left the previous per-user installation directory." }
    $machineDir = Assert-Installed $allUsersKey $perUserKey
    if ($machineDir -eq $userDir) {
        throw "System-wide install reused the per-user installation directory."
    }

    $expectedMachineDir = Join-Path $env:ProgramFiles "qBittorrent Remote"
    if ($machineDir -ne $expectedMachineDir) { throw "Unexpected machine default: $machineDir; expected $expectedMachineDir" }
    Write-Host "Functional installer smoke test: reject reverse unapproved scope switch"
    Run-Setup $installerPath "CurrentUser" $false 3
    $unchangedDir = Assert-Installed $allUsersKey $perUserKey
    if ($unchangedDir -ne $machineDir) { throw "Rejected migration changed install location." }

    Write-Host "Functional installer smoke test: automatic All users -> Current user migration"
    Run-Setup $installerPath "CurrentUser" $true
    if (Test-Path $allUsersKey) { throw "Automatic migration left all-users uninstall registration." }
    if (Test-Path $machineDir) { throw "Automatic migration left the previous machine installation directory." }
    $backToUserDir = Assert-Installed $perUserKey $allUsersKey
    Run-Uninstall $backToUserDir "CurrentUser"
    if (Test-Path $perUserKey) { throw "Current-user uninstall registration remains." }
    Write-Host "Functional installer smoke test: standalone All users uninstall"
    Run-Setup $installerPath "AllUsers"
    $machineDir = Assert-Installed $allUsersKey $perUserKey
    Run-Uninstall $machineDir "AllUsers"
    if (Test-Path $allUsersKey) { throw "All-users uninstall registration remains." }

    $actual = (Get-ItemProperty -LiteralPath $settingsKey).$sentinelName
    if ($actual -ne $sentinelValue) {
        throw "Personal QSettings data was changed or erased by scope switching."
    }
    Write-Host "Both real installer scopes, upgrades, uninstallers and user-settings preservation passed."
}
finally {
    if (Test-Path $settingsKey) {
        Remove-ItemProperty -LiteralPath $settingsKey -Name $sentinelName -ErrorAction SilentlyContinue
    }
}
