# Installs Flipper System Dashboard daemon as auto-start on user logon.
# - Uses Windows Task Scheduler (per-user, no admin needed)
# - Runs via pythonw.exe (no console window)
# - Daemon polls for Flipper indefinitely; auto-connects when you open
#   System Dashboard FAP on Flipper.

$ErrorActionPreference = "Stop"

$daemonDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$script = Join-Path $daemonDir "dashboard.py"
$logPath = Join-Path $env:USERPROFILE ".flipper_dashboard.log"

# Find pythonw.exe — windowless python
$pythonw = (Get-Command pythonw.exe -ErrorAction SilentlyContinue).Source
if (-not $pythonw) {
    $python = (Get-Command python.exe).Source
    $pythonw = Join-Path (Split-Path $python -Parent) "pythonw.exe"
    if (-not (Test-Path $pythonw)) {
        throw "pythonw.exe not found. Reinstall Python with default options."
    }
}

Write-Host "Daemon script: $script"
Write-Host "Python:        $pythonw"
Write-Host "Log file:      $logPath"
Write-Host ""

# Pre-flight: make sure dependencies are installed
Write-Host "Installing Python dependencies (pyserial, psutil)..."
& (Join-Path (Split-Path $pythonw -Parent) "python.exe") -m pip install --quiet -r (Join-Path $daemonDir "requirements.txt")

# Compose scheduled task
$taskName = "Flipper System Dashboard"
$action = New-ScheduledTaskAction -Execute $pythonw -Argument "`"$script`"" -WorkingDirectory $daemonDir
$trigger = New-ScheduledTaskTrigger -AtLogOn -User $env:USERNAME
$settings = New-ScheduledTaskSettingsSet `
    -AllowStartIfOnBatteries `
    -DontStopIfGoingOnBatteries `
    -ExecutionTimeLimit (New-TimeSpan -Seconds 0) `
    -RestartCount 99 `
    -RestartInterval (New-TimeSpan -Minutes 1)
$principal = New-ScheduledTaskPrincipal `
    -UserId $env:USERNAME `
    -LogonType Interactive `
    -RunLevel Limited

Register-ScheduledTask `
    -TaskName $taskName `
    -Action $action `
    -Trigger $trigger `
    -Settings $settings `
    -Principal $principal `
    -Description "Streams CPU/RAM/GPU/Net to Flipper Zero System Dashboard FAP" `
    -Force | Out-Null

Write-Host "Scheduled Task registered: $taskName"

# Start it right now so it picks up the currently-connected Flipper
Start-ScheduledTask -TaskName $taskName
Write-Host "Daemon started in background. Logs: $logPath"
Write-Host ""
Write-Host "From now on it will auto-start on every Windows logon."
Write-Host "To remove: run uninstall_autostart.ps1"
