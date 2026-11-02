# 摄像头 → STM32 → 板载 CC1310 → XDS110 串口 → 网页

2026-10-14 实机验证通过。两端固件已经烧录；本机网页服务为 http://localhost:8080。
页面新增「图像来源 / 接收串口 / 接收波特率 / 刷新串口 / 连接切换」。

## 接线与实际外设

STM32 是 STM32U575，CC1310 为板载芯片，XDS110 序列号 `00002156`。
**接线标签中的 SPI3 应修正为 SPI2。** ST DS13737 Rev10 第 133 页 Table 27 明确规定：
PB13/PB14/PB15 的 AF5 为 SPI2_SCK/MISO/MOSI，AF6 是 MDF1，并非 SPI3。
保持当前接线，固件使用 SPI2 AF5，并开启 `IOSwap` 交换 MOSI/MISO。
引脚表截图保存在 `link_validation/pb_af.png`。

| STM32 | CC1310 | 实际功能 |
|---|---|---|
| PB13 | DIO9 | SPI2 SCK |
| PB14 | DIO7 | STM32 MOSI → SSI0 RX，IOSwap 后输出 |
| PB15 | DIO6 | STM32 MISO ← SSI0 TX，IOSwap 后输入 |
| PE9 | DIO5 | CS_N，GPIO 控制，低有效 |
| PD15 | DIO8 | CC1310 → STM32 就绪/流控，高=可接收，低=正在转串口 |
| PD14 | RESET_N | 复位脚；STM32 用开漏释放，不能当就绪输入 |
| — | DIO1 | CC1310 UART TX → XDS110 → COM20 |
| — | DIO2 | CC1310 UART RX |

本次 SPI 设置为 **mode 1（CPOL=0，CPHA=1）、625 kHz、8 bit、MSB first**。
CC1310 SSI 在 mode 0 下需要每个字之间切换 FSS；本协议一次 CS 包含整块数据，
因此两端使用 mode 1。实测 mode 0 只收到首字节，切为 mode 1 后整块通过。
SPI 模式下关闭 UCPD dead-battery 下拉，释放与 PB15 共用的 CC2 功能。

## 两个接收来源

| 页面选择 | 图像接收串口 | 波特率 | 相机控制串口 |
|---|---|---|---|
| STM32 直接接收 | COM22（Horco CMSIS-DAP UART） | 2000000 | COM22 |
| CC1310 SPI 转发接收 | COM20（XDS110 Application/User UART） | 115200 | COM22 |

COM19 是 XDS110 Auxiliary Data Port，图像数据使用 COM20。
接收 CC1310 时服务同时打开 COM22 下发 R/M/Q/F 命令，并后台读走镜像数据，
避免串口缓存溢出影响切回 STM32。切换来源会重开接收口、重置解析器/统计并清掉旧画面。
页面下拉框也可以选择其他枚举出的串口。

`TRANSPORT=dual` 将同一帧镜像发送至两路，适合比较和排查；抓帧速度受慢的 CC1310
转发通道限制。若只需要原有高速 STM32 串口图传，仍可编译 `TRANSPORT=uart`。
USB CDC 也保留原来的 `TRANSPORT=usb`。

## 启动网页

在 `G:\material\cam_hardware\sofeware_V2` 下运行：

```powershell
python tools/stream_server.py --source cc1310 --stm32-port COM22 --cc1310-port COM20
```

访问 http://localhost:8080，通过页面「图像来源」选择 STM32 或 CC1310，然后点击「连接 / 切换」。
选择来源会自动填入对应端口和波特率，也可以手动更改。
旧用法 `python tools/stream_server.py COM22 --baud 2000000` 继续有效。
当前服务为本次验证启动的进程，PID 记录在 `link_validation/server.pid`。
重新启动前先结束旧服务，避免端口被占用。

## 编译和烧录

以下路径相对 V2 目录。使用独立构建目录保留原有 UART 版 `build/`。

```powershell
# STM32：相机 + UART/SPI 双路镜像，默认 QQVGA 硬件 JPEG
.\build.ps1 APP=stream TRANSPORT=dual BUILD_DIR=build_dual all
& ..\tools\openocd\bin\openocd.exe -s ..\tools\openocd\openocd\scripts -f interface/cmsis-dap.cfg -f target/stm32u5x.cfg -c 'adapter speed 1000; program build_dual/STM32U575_stream.elf verify reset exit'

# CC1310：SSI0 接收，DIO1 UART TX 输出
.\cc1310\build.ps1 APP=spiuart BUILD_DIR=build_spiuart all
& ..\tools\openocd\bin\openocd.exe -s ..\tools\openocd\openocd\scripts -f board/ti_cc13x0_launchpad.cfg -c 'adapter serial 00002156; adapter speed 200; program cc1310/build_spiuart/cc1310_spiuart.elf verify reset exit'
```

建议先烧录 CC1310，再烧录 STM32；启动网页前释放 COM22/20。
仅排查接线可使用 `APP=spitest BUILD_DIR=build_spitest`，该诊断固件在 COM22（921600）
打印包计数，并让 COM20（115200）输出 `SPI2 bridge test`。完成诊断后恢复 `APP=stream`。

## 数据和流控

SPI 每包固定 258 字节：2 字节小端有效长度（1～256），再加 256 字节数据区，末尾补零。
主机等 DIO8 高后拉低 CS 发送整包，结束后抬高 CS，并等待 DIO8 低确认已收包。
CC1310 收满后拉低 DIO8，将有效数据通过 UART 发完，再重新使能接收并拉高 DIO8。
只需 258 字节包缓冲，能转发远大于 CC1310 RAM 的完整图像。

CC1310 串口恢复原来的 `OV3S` 图像帧：14 字节头、payload、4 字节小端字节和。
头中包含宽高、格式、8 位序号和 32 位 payload 长度，原始像素和 JPEG 共用解析器。
原有校验方式是字节和，不是多项式 CRC；页面沿用「校验错」计数。
STM32 从 MISO 读回循环 `CC13` 签名，确认反向引脚也通。

STM32 先完整发完直接串口帧，再尝试 SPI 转发；CC1310 不在线或 SPI 超时时，
直接串口帧仍完整。SPI 等待和发送均有超时。
CC1310 对短包、非法长度和 SSI 溢出计数，并重新准备接收。

## 实测记录

- 两端编译通过，OpenOCD 烧录显示 `Verified OK`，日志在 `link_validation/flash_*.log`。
- 初次同时采集：COM22 收到 38 张、COM20 收到 37 张完整 JPEG；
  37 张共同序号的 payload SHA256 完全一致，字节和错误和帧序号缺口均为 0。
  采集截止时 COM20 最后一帧尚在传输，记录为边界半帧。
- 网页集成验证通过：160×120 / 320×240 JPEG、160×120 RGB565 原始像素，
  JPEG→原始→JPEG，STM32↔CC1310 两轮切换，快照保存及继续接收。
  所有检查阶段均为 0 校验错、0 帧序号缺口。
- 最终连续统计样本：CC1310 412 帧、139 秒，0 丢帧、0 校验错、0 重同步；
  随后的网页截图已超过 500 帧，计数仍为 0。160×120 JPEG 大约 2 KB，约 3.2 FPS。
  160×120 RGB565 为 38400 字节，实测约 0.23 FPS，建议日常使用硬件 JPEG。
- 运行中通过调试口读取：STM32 5752 个 SPI 包全部找到 MISO 签名，SPI 错误和握手超时均为 0；
  稍后读取 CC1310 为 5782 包，非法包与短包均为 0（两端读取时刻不同）。

记录文件：`frames_report.json`（两路逐帧 SHA256）、`http_report.json`、`http_test.log`、
`final_stats.json`、`stm32_counters.log`、`cc1310_counters.log`、`cc1310_web_ui.jpg`。
这些文件均在 `link_validation/`，`cc1310_received.jpg` 是实际转发后解码成功的 JPEG。

复测方法：停止网页服务后，从工作区根目录运行 `link_validation/read_both.py` 和
`link_validation/validate_frames.py` 的完整 V2 路径；两者采集文件路径按工作区根目录解析。
运行网页服务时，可执行 `python -u sofeware_V2/link_validation/verify_http.py` 验证来源切换与控制。
集成脚本会实际更改相机设置，结束时恢复 CC1310、160×120 硬件 JPEG。
