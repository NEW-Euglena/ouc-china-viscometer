param([string]$Port = '', [int]$HttpPort = 8080)
$ErrorActionPreference = 'Stop'
# Start the receiver without a device connection unless a serial port is specified.
$receiverPython = Join-Path $PSScriptRoot '.venv/Scripts/python.exe'
if (-not (Test-Path -LiteralPath $receiverPython)) { $receiverPython = 'python' }
$receiverArgs = @('-X', 'utf8', (Join-Path $PSScriptRoot 'telemetry_server.py'), '--host', '127.0.0.1', '--port', [string]$HttpPort)
if ($Port) { $receiverArgs += @('--serial', $Port) }
Write-Host "Web interface: http://127.0.0.1:$HttpPort/; press Ctrl+C to stop."
& $receiverPython @receiverArgs
