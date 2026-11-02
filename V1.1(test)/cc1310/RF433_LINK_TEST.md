# CC1310 双板 433.92 MHz 收发：123 / 888

## 板卡与串口

此处按本次无线测试重新编号。两块板运行自定义固件，使用相同的无线协议。

| 项目 | 板子1（发起端） | 板子2（应答端） |
|---|---|---|
| Windows 端口组 | COM25 / COM26 | COM19 / COM20 |
| 查看日志的 UART | **COM25** | **COM20** |
| CC1310 TX / RX | **DIO5 / DIO3** | **DIO1 / DIO2** |
| 固件 APP | `rf433_board1` | `rf433_board2` |
| 无线发送 | ASCII `123`，3 字节 | 收到 `123` 后回复 ASCII `888`，3 字节 |
| 无线接收 | `888` | `123` |

串口均为 **115200、8N1、无流控**。COM26、COM19 是 Auxiliary Data Port，本次使用对应的 Application/User UART 看日志。
第一块 `1920` 串口测试中已确认 COM20 板的 TX/RX 与最初提供的方向相反，当前采用实测正确的 DIO1=TX、DIO2=RX。

## 无线参数与流程

- 433.92 MHz，50 kbps，2-GFSK，差分 RF 前端、外部偏置。
- 前导 4 字节，同步字 `0x930B51DE`，变长包、CRC 校验，关闭白化。
- 板子1发 `123` 后立即进入接收，最多等待 1000 ms，之后间隔约 200 ms 重试。
- 板子2监听；收到 CRC 正确的 `123` 后，等待约 20 ms，再发送 `888`，随后继续监听。
- RF 负载不带换行；串口日志带 CR LF。上电后自动收发，不需要向串口发送命令。

此 demo 的日志区分发送命令成功、接收到 CRC 正确的负载，以及完整往返成功。被动天线不会被 MCU 自动识别；单板发包成功不等于天线性能或双板链路已验证。

## 构建与烧录

从 `G:\material\cam_hardware\sofeware_V2\cc1310` 打开 PowerShell：

```powershell
.\build.ps1 APP=rf433_board1 BAND=433 BUILD_DIR=build_rf433_board1 all
.\build.ps1 APP=rf433_board2 BAND=433 BUILD_DIR=build_rf433_board2 all

$rfOcd = 'G:\material\cam_hardware\tools\openocd\bin\openocd.exe'
$rfScripts = 'G:\material\cam_hardware\tools\openocd\openocd\scripts'
```

**烧录时只连接要烧录的那只 XDS110。** 本次两只探针的 USB 序列号相同，均为 `00002156`，不能用序列号区分；当前 XDS110 驱动也未按 `adapter usb location` 筛选，已做只读测试确认。

连接板子1（确认仅出现 COM25/26），烧录：

```powershell
& $rfOcd -s $rfScripts -f board/ti_cc13x0_launchpad.cfg -c 'adapter speed 200; program build_rf433_board1/cc1310_rf433_board1.elf verify reset exit'
```

换成只连接板子2（确认仅出现 COM19/20），烧录：

```powershell
& $rfOcd -s $rfScripts -f board/ti_cc13x0_launchpad.cfg -c 'adapter speed 200; program build_rf433_board2/cc1310_rf433_board2.elf verify reset exit'
```

完成两块板烧录后，给两块板同时供电，串口助手分别打开 COM25、COM20 看文本日志；UART 测试脚本/其他串口程序须先关闭以释放端口。COM 号改变时按 Windows 实际枚举结果选择。

## 如何判定结果

成功往返时的预期日志（示例，尚非双板实测记录）：

```text
板子1 / COM25:
RF READY
TX data="123" count=1
RX data="888" len=3 CRC_OK count=1
ROUNDTRIP PASS

板子2 / COM20:
RF READY
RX data="123" len=3 CRC_OK count=1
TX data="888" count=1
```

`RX TIMEOUT waiting for 888` 只表示接收窗口内没有正确回包。`RF INIT FAIL`、`TX FAIL` 或 `RX FAIL` 会附带错误码、启动阶段和 RF 命令状态字。

## 本次实测进度（2026-10-14）

- 板子1：JTAG ID `0x3B9BE02F`，芯片工厂 IEEE 地址 `00124B00219D1542`。
- 板子1 UART DIO5=TX、DIO3=RX：此前在 COM25 连续收到 `2526`，串口路径通过。
- 两份 433 MHz 固件均编译成功，输出目录分别为 `build_rf433_board1/`、`build_rf433_board2/`。
- 板子1已烧录，OpenOCD 返回 `Verified OK`；复位后 COM25 实际读取到：

```text
CC1310 RF433 BOARD=1 FREQ=433.92MHz RATE=50kbps UART=115200
RF READY
TX data="123" count=1
RX TIMEOUT waiting for 888
TX data="123" count=2
RX TIMEOUT waiting for 888
```

- 板子2：JTAG ID `0x3B9BE02F`，芯片工厂 IEEE 地址 `00124B00376111F7`，与板子1不同。
- 板子2现已单独连接 COM19/20 烧录，OpenOCD 返回 `Verified OK`。复位后在 COM20、115200 实际收到：

```text
CC1310 RF433 BOARD=2 FREQ=433.92MHz RATE=50kbps UART=115200
RF READY
RX WAITING for 123
```

- 两块板的单板初始化、板子1发包和板子2监听已验证；目前两板分开连接，双板 CRC 接收及 `ROUNDTRIP PASS` **尚待同时供电验证**。

## 代码位置

- `Core/Src/app_rf433_board1.c`、`app_rf433_board2.c`：角色及 UART 引脚。
- `Core/Inc/rf433_demo_impl.h`：123/888 收发协议和串口日志。
- `Core/Src/rf_prop.c`、`Core/Inc/rf_prop.h`：RF 初始化、发包、接收队列与超时。
- `Core/Inc/rf_prop_band.h`：868 MHz / 433.92 MHz PHY 参数。

RF 层与 433 MHz 配置由本地 V1 工程迁入，保留其驱动代码；应用协议为本次新增。原 433 MHz 参数引用的 TI SmartRF 导出来源：
[TI E2E CC1310 433 MHz 配置讨论](https://e2e.ti.com/support/wireless-connectivity/sub-1-ghz-group/sub-1-ghz/f/sub-1-ghz-forum/769016/rtos-cc1310-cc1310-433mhz)。
