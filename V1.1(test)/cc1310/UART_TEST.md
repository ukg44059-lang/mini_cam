# CC1310 UART 连续发送测试

两个 demo 共用 `Core/Inc/uart_test_impl.h`，使用 UART0、115200、8N1，无硬件流控。
每隔约 0.5 秒发送一行文本（带 CR LF）。RX 已配置为输入，本 demo 不处理接收数据。

| APP | CC1310 TX | CC1310 RX | 发送文本 | 验证状态 |
|---|---|---|---|---|
| `uart1920` | DIO1 | DIO2 | `1920\r\n` | 已通过 XDS110 在 COM20 实际收到 |
| `uart2526` | DIO5 | DIO3 | `2526\r\n` | 已在 COM25 实际收到；后续按无线任务编号称此板为板子1 |

第一块板的 TX/RX 方向经实测与最初提供的方向相反，现已修正为上表配置。
COM19 是该 XDS110 的 Auxiliary Data Port；本次验证有效的串口为 COM20。
COM 号可能随 USB 插口/设备变化，请按实际枚举结果选择。

从本 `cc1310` 目录构建，两个输出目录独立保存：

```powershell
.\build.ps1 APP=uart1920 BUILD_DIR=build_uart1920 all
.\build.ps1 APP=uart2526 BUILD_DIR=build_uart2526 all
```

只连接第一块板的 XDS110 时，可用下列命令烧录（将整个命令作为一行运行）：

```powershell
G:\material\cam_hardware\tools\openocd\bin\openocd.exe -s G:\material\cam_hardware\tools\openocd\openocd\scripts -f board/ti_cc13x0_launchpad.cfg -c "adapter serial 00002156; adapter speed 200; program build_uart1920/cc1310_uart1920.elf verify reset exit"
```

电脑端关闭其他占用串口的程序后，可在 VOFA/串口助手选择 COM20、115200、8N1、无流控，查看文本接收区。

固件初始化必须同时开启 `PRCM_DOMAIN_PERIPH`（GPIO）和 `PRCM_DOMAIN_SERIAL`（UART0）。
共享实现中的发送计数及 TX 高低电平采样计数用于 JTAG 排查，不改变发送文本。
