<#
    build.ps1 -- make 的包装脚本

    这台机器上没有全局的 make，用的是 STM32CubeIDE 1.5.0 里自带的那个。
    直接敲 make 会报「无法将"make"项识别为 cmdlet」，所以用这个脚本代劳。

    用法（参数原样透传给 make）：

        .\build.ps1                          # 编译默认 demo (camera)
        .\build.ps1 APP=stream               # 编译串口图传
        .\build.ps1 APP=stream flash         # 编译并烧录（走 E: 盘拖拽）
        .\build.ps1 APP=stream flash-ocd     # 编译并用 OpenOCD 烧录
        .\build.ps1 APP=test_pwm flash-dap   # 编译并用 CMSIS-DAP 烧录
        .\build.ps1 APP=stream FRAMESIZE=QVGA flash
        .\build.ps1 clean

    ----------------------------------------------------------------------
    两个必须注意的点（都踩过）：

    1. 本文件必须存成 UTF-8 with BOM。
       PowerShell 5.1 默认按系统 ANSI(GBK) 读 .ps1，无 BOM 的 UTF-8 会整个
       乱码并报「字符串缺少终止符」之类的语法错。

    2. 必须把 Unix 工具目录加进 PATH 再调 make。
       Makefile 里有 $(shell cat ...) / rm -rf / mkdir -p，make 要靠 sh.exe
       去执行；从 Git Bash 调用时 PATH 里本来就有，从 PowerShell 调用时没有，
       表现是 make 报「The system cannot find the path specified.」。
       CubeIDE 的 tools\bin 提供 sh/rm/mkdir/echo，cat 得从 Git 的 usr\bin 借。
    ----------------------------------------------------------------------
#>

# 注意别用 'Stop'：PowerShell 5.1 会把原生程序写到 stderr 的每一行都包装成
# ErrorRecord(NativeCommandError)，于是 gcc 的一条 warning 就会被当成致命错误，
# 明明编译成功也报失败。这里靠 $LASTEXITCODE 判断真正的成败。
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
    Write-Host ''
    Write-Host '这个路径来自 STM32CubeIDE 1.5.0。如果 CubeIDE 挪了或卸了，'
    Write-Host '改本脚本开头的 MakeBin 变量，或者装一个 GNU make 并加进 PATH。'
    exit 1
}

# 找 Git 自带的 usr\bin，Makefile 里的 cat 靠它
$GitUsrBin = $null
$gitCmd = Get-Command git -ErrorAction SilentlyContinue
if ($gitCmd) {
    $candidate = Join-Path (Split-Path (Split-Path $gitCmd.Source -Parent) -Parent) 'usr\bin'
    if (Test-Path (Join-Path $candidate 'cat.exe')) { $GitUsrBin = $candidate }
}
if (-not $GitUsrBin) {
    Write-Host '警告：没找到 Git 的 usr\bin，Makefile 里的 cat 可能会失败。' -ForegroundColor Yellow
    Write-Host '      后果是每次都全量重编，不影响正确性。'
}

# OpenOCD：Makefile 的 flash-ocd 目标直接调 openocd，不在 PATH 里会报 'openocd: not found'
$OpenOcdBin = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../tools/openocd/bin'))

$prepend = $MakeBin + ';'
if ($GitUsrBin)                   { $prepend += $GitUsrBin + ';' }
if (Test-Path $OpenOcdBin)        { $prepend += $OpenOcdBin + ';' }
$env:PATH = $prepend + $env:PATH

# 切到脚本所在目录，这样在任何路径下调用都能编
Push-Location $PSScriptRoot
try {
    & $MakeExe @args
    if ($LASTEXITCODE -ne 0) {
        Write-Host ''
        Write-Host ('编译失败，make 退出码 ' + $LASTEXITCODE) -ForegroundColor Red
    }
    exit $LASTEXITCODE
}
finally {
    Pop-Location
}
