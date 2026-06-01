# Removes the auto-start scheduled task for Flipper System Dashboard daemon.

$ErrorActionPreference = "Stop"

$taskName = "Flipper System Dashboard"

# Stop running instance first
$task = Get-ScheduledTask -TaskName $taskName -ErrorAction SilentlyContinue
if ($task) {
    if ($task.State -eq "Running") {
        Stop-ScheduledTask -TaskName $taskName
        Write-Host "Stopped running daemon."
    }
    Unregister-ScheduledTask -TaskName $taskName -Confirm:$false
    Write-Host "Unregistered scheduled task: $taskName"
} else {
    Write-Host "No scheduled task named '$taskName' found."
}

# Also stop any stray pythonw running dashboard.py (manual launches)
Get-Process pythonw -ErrorAction SilentlyContinue | Where-Object {
    try { $_.CommandLine -match "dashboard\.py" } catch { $false }
} | ForEach-Object {
    Stop-Process -Id $_.Id -Force
    Write-Host "Killed pythonw PID $($_.Id)"
}
