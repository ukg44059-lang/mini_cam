param(
    [string]$Stm32Port = "COM30",
    [string]$ReceiverPort = "COM20",
    [string]$SenderStatusPort = "COM34",
    [int]$HttpPort = 8080,
    [string]$Python = "python"
)
Push-Location $PSScriptRoot
try {
    & $Python tools/stream_server.py --source rf433 --stm32-port $Stm32Port --rf-port $ReceiverPort --tx-status-port $SenderStatusPort --http-port $HttpPort
    exit $LASTEXITCODE
} finally { Pop-Location }
