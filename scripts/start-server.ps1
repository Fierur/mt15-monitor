# Usage: .\scripts\start-server.ps1                      (real data, server/.env, port 8000)
#        .\scripts\start-server.ps1 -Demo -Port 8001     (simulated data, server/.env.demo)
#        .\scripts\start-server.ps1 -Supabase            (dashboard reads Supabase)
param([int]$Port = 8000, [switch]$Demo, [switch]$Supabase)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
Set-Location -LiteralPath $projectRoot
$pythonExe = Join-Path $projectRoot '.venv\Scripts\python.exe'
if (-not (Test-Path -LiteralPath $pythonExe)) {
    throw 'Create .venv and install server/requirements.txt first. See docs/SETUP.md.'
}
if ($Demo -and $Supabase) { throw 'Choose either -Demo or -Supabase.' }
$env:MT15_DASHBOARD_MODE = if ($Supabase) { 'supabase' } else { 'local' }
if ($Demo) {
    if ($Port -eq 8000) { throw 'Use a different port for the demo server, e.g. -Port 8001.' }
    & $pythonExe scripts/setup_server.py --env server/.env.demo --template server/.env.demo.example
    $env:MT15_ENV = 'server/.env.demo'
} else {
    & $pythonExe scripts/setup_server.py
    Remove-Item Env:MT15_ENV -ErrorAction SilentlyContinue
}
# Proxy headers are trusted only from loopback (cloudflared runs on this laptop).
& $pythonExe -m uvicorn server.app:app --host 127.0.0.1 --port $Port --proxy-headers --forwarded-allow-ips 127.0.0.1
