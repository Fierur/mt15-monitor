# Publishes the LOCAL server (127.0.0.1:8000) through a Cloudflare Quick Tunnel.
# Anyone with the printed https://<random>.trycloudflare.com URL reaches the login page.
# The URL changes on every run: update the device with the serial command
#   URL https://<random>.trycloudflare.com
# Quick Tunnels are for demos only (no uptime guarantee). See docs/SETUP.md.
param([int]$Port = 8000)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot

$cloudflared = Get-Command cloudflared -ErrorAction SilentlyContinue
if (-not $cloudflared) {
    throw 'cloudflared not found. Install: winget install --id Cloudflare.cloudflared, then open a new terminal.'
}

# Refuse to publish a server that is down or still using weak/default credentials.
try {
    $health = Invoke-RestMethod -Uri "http://127.0.0.1:$Port/health" -TimeoutSec 5
} catch {
    throw "Local server is not answering on port $Port. Start it first: .\scripts\start-server.ps1"
}
if (-not $health.ok) { throw 'Server health check failed.' }

$envFile = Join-Path $projectRoot 'server\.env'
$password = (Get-Content -LiteralPath $envFile | Where-Object { $_ -like 'ADMIN_PASSWORD=*' }) -replace '^ADMIN_PASSWORD=', ''
if ($password.Length -lt 16) {
    throw 'ADMIN_PASSWORD in server/.env is shorter than 16 characters; use a strong password before publishing.'
}

Write-Host "Publishing http://127.0.0.1:$Port - press Ctrl+C to stop. Vehicle location becomes reachable by anyone who has the URL and password." -ForegroundColor Yellow
& $cloudflared.Source tunnel --no-autoupdate --url "http://127.0.0.1:$Port"
