$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
$runDir = Join-Path $root 'data\mqtt-live'
$config = Join-Path $runDir 'mosquitto.conf'
$settings = Join-Path $runDir '.env'
$broker = Join-Path $env:ProgramFiles 'mosquitto\mosquitto.exe'
$python = Join-Path $root '.venv\Scripts\python.exe'
$bridge = Join-Path $PSScriptRoot 'mqtt_bridge.py'
foreach ($path in @($config, $settings, $broker, $python, $bridge)) {
    if (!(Test-Path -LiteralPath $path -PathType Leaf)) { throw "Missing MQTT file $path" }
}

$instances = Get-CimInstance Win32_Process -Filter "Name='mosquitto.exe' OR Name='python.exe'"
if (!($instances | Where-Object { $_.CommandLine -and $_.CommandLine.Contains($config) })) {
    Start-Process -FilePath $broker -ArgumentList @('-c', ('"' + $config + '"')) -WindowStyle Hidden `
        -WorkingDirectory $root -RedirectStandardOutput (Join-Path $runDir 'broker.log') `
        -RedirectStandardError (Join-Path $runDir 'broker-errors.log')
}
if (!($instances | Where-Object { $_.CommandLine -and $_.CommandLine.Contains($bridge) -and $_.CommandLine.Contains($settings) })) {
    Start-Process -FilePath $python -ArgumentList @('-u', ('"' + $bridge + '"'), '--mqtt-env', ('"' + $settings + '"')) `
        -WindowStyle Hidden -WorkingDirectory $root -RedirectStandardOutput (Join-Path $runDir 'bridge.log') `
        -RedirectStandardError (Join-Path $runDir 'bridge-errors.log')
}
