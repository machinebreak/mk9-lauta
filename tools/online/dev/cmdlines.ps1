# Writes the command line of every mk9.exe seen in the next $Seconds seconds (once per process) to $Out.
param([int]$Seconds = 90, [string]$Out = '')
if (-not $Out) { $Out = Join-Path (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path 'out\f3\cmdlines.txt' }
$seen = @{}
Set-Content -Path $Out -Value '' -Encoding utf8
$end = (Get-Date).AddSeconds($Seconds)
while ((Get-Date) -lt $end) {
    Get-CimInstance Win32_Process -Filter "Name='mk9.exe'" -ErrorAction SilentlyContinue | ForEach-Object {
        if (-not $seen.ContainsKey($_.ProcessId)) {
            $seen[$_.ProcessId] = $true
            Add-Content -Path $Out -Value ("{0} pid {1}: {2}" -f (Get-Date -Format 'HH:mm:ss'), $_.ProcessId, $_.CommandLine) -Encoding utf8
        }
    }
    Start-Sleep -Milliseconds 300
}
