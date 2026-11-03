param(
    [string]$ReceiverPort = "COM42",
    [int]$HttpPort = 8081,
    [string]$Python = "python"
)
Push-Location $PSScriptRoot
try {
    & $Python tools/stream_server.py --source rf433 --rf-port $ReceiverPort --rf-baud 921600 --http-port $HttpPort
    exit $LASTEXITCODE
} finally {
    Pop-Location
}
