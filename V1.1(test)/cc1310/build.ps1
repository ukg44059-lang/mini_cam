<#
    build.ps1 -- make 的包装脚本（CC1310 工程）

    这台机器没有全局 make，借 STM32CubeIDE 1.5.0 里那个。用法：

        .\build.ps1              # 编译
        .\build.ps1 flash        # 编译并用 OpenOCD + XDS110 烧录
        .\build.ps1 erase        # 整片擦除（CCFG 写坏、芯片连不上时用）
        .\build.ps1 size
        .\build.ps1 clean

    和上一级 STM32 工程那份是同一套逻辑，两个注意点也一样：
      1. 本文件必须存成 UTF-8 with BOM，否则 PowerShell 5.1 按 GBK 读会乱码报语法错
      2. 必须把 sh/rm/mkdir/cat 和 openocd 加进 PATH 再调 make
#>

# 别用 'Stop'：PowerShell 5.1 会把原生程序 stderr 的每一行包成 ErrorRecord，
# gcc 一条 warning 就会被当成致命错误。靠 $LASTEXITCODE 判断真正的成败。
$ErrorActionPreference = 'Continue'

$MakeBin = 'F:\stm32cube_ide\STM32CubeIDE_1.5.0\STM32CubeIDE\plugins\com.st.stm32cube.ide.mcu.externaltools.make.win32_1.5.0.202011040924\tools\bin'
if ($env:MINICAM_MAKE_BIN) { $MakeBin = $env:MINICAM_MAKE_BIN }
$MakeExe = Join-Path $MakeBin 'make.exe'
if (-not (Test-Path -LiteralPath $MakeExe)) {
    $foundMake = Get-Command make.exe -ErrorAction SilentlyContinue
    if ($foundMake) { $MakeExe = $foundMake.Source; $MakeBin = Split-Path $MakeExe -Parent }
}

if (-not (Test-Path $MakeExe)) {
    Write-Host ('找不到 make: ' + $MakeExe) -ForegroundColor Red
    exit 1
}

$GitUsrBin = $null
$gitCmd = Get-Command git -ErrorAction SilentlyContinue
if ($gitCmd) {
    $c = Join-Path (Split-Path (Split-Path $gitCmd.Source -Parent) -Parent) 'usr\bin'
    if (Test-Path (Join-Path $c 'cat.exe')) { $GitUsrBin = $c }
}

$OpenOcdBin = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../../tools/openocd/bin'))

$prepend = $MakeBin + ';'
if ($GitUsrBin)            { $prepend += $GitUsrBin + ';' }
if (Test-Path $OpenOcdBin) { $prepend += $OpenOcdBin + ';' }
$env:PATH = $prepend + $env:PATH

Push-Location $PSScriptRoot
try {
    & $MakeExe @args
    if ($LASTEXITCODE -ne 0) {
        Write-Host ''
        Write-Host ('失败，make 退出码 ' + $LASTEXITCODE) -ForegroundColor Red
    }
    exit $LASTEXITCODE
}
finally {
    Pop-Location
}
