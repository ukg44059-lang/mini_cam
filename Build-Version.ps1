param(
    [ValidateSet('V1.0', 'V1.1(test)')]
    [string]$Version = 'V1.0',
    [string]$GccPath = '',
    [int]$ReceiverTxPin = 5,
    [int]$ReceiverRxPin = 3
)
$ErrorActionPreference = 'Stop'
$versionDir = Join-Path $PSScriptRoot $Version
if (-not (Test-Path -LiteralPath $versionDir)) { throw "Missing version: $Version" }
$gccArgs = @()
if ($GccPath) { $gccArgs += "GCC_PATH=$($GccPath.Replace('\','/'))" }
$builds = @(
    @{ Script = (Join-Path $versionDir 'build.ps1'); Args = @('APP=stream','TRANSPORT=dual','FRAMESIZE=480X360','BUILD_DIR=build_release','-j8','all') },
    @{ Script = (Join-Path $versionDir 'cc1310/build.ps1'); Args = @('APP=rf_image_tx','BAND=433','RF_RATE=500','BUILD_DIR=build_release_tx','-j8','all') },
    @{ Script = (Join-Path $versionDir 'cc1310/build.ps1'); Args = @('APP=rf_image_rx','BAND=433','RF_RATE=500','RF_UART_BAUD=921600',"RF_UART_TX=$ReceiverTxPin","RF_UART_RX=$ReceiverRxPin",'BUILD_DIR=build_release_rx','-j8','all') }
)
foreach ($build in $builds) {
    # Child process isolates existing wrappers' `exit` from this orchestrator.
    $shellExe = (Get-Process -Id $PID).Path
    & $shellExe -NoProfile -ExecutionPolicy Bypass -File $build.Script @($build.Args) @gccArgs
    if ($LASTEXITCODE -ne 0) { throw "Build failed: $($build.Script)" }
}
Write-Host "Built $Version (STM32, CC1310 TX, CC1310 RX). No hardware was flashed."
