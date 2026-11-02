# LAUNCHXL-CC1310 裸机工程（VSCode + GCC + OpenOCD）

**433 MHz 无线图像：`APP=rf_image_tx` / `APP=rf_image_rx`，必须 `BAND=433`；收发已烧录并通过 COM25 实测图像接收，详见 [RF_IMAGE_LINK.md](../RF_IMAGE_LINK.md)。COM20 是发送板可选状态口。**

V1.1(test) 双向控制版本中，PC只连接收板UART；`WRC1`命令经RX排队、433 MHz转发到TX，再通过STM32 SPI送达。板端状态以`WTS1`记录经433 MHz回传。新的TX/RX镜像需配套刷写；不能把它与V1.0镜像混用。

图像应用默认 500 kbps、满负载组包、接收端后台串口；COM25=921600，COM20 状态口仍为 115200。性能和画质对照见 [RF_PERFORMANCE.md](../RF_PERFORMANCE.md)。

**自制板 SPI 图像转串口：新增 `APP=spiuart`，配置与实测见 [SPI_IMAGE_LINK.md](../SPI_IMAGE_LINK.md)。**
该程序使用 DIO7=SSI RX、DIO6=SSI TX、DIO5=CS、DIO9=SCK、DIO8=就绪，
UART TX=DIO1/RX=DIO2（COM20，115200）；与下述 LaunchPad 默认引脚分别配置。

板子：**TI LAUNCHXL-CC1310 Rev A**（板载 CC1310F128RGZ，Cortex-M3 48MHz，128KB Flash / 20KB SRAM，
外加板载 XDS110 调试器）

**不装 CCS、不装 SimpleLink SDK、不用 SysConfig**，全部复用上一级 STM32 工程已经装好的工具。

---

## 0. 快速开始

433.92 MHz 双板 `123` / `888` 收发 demo 见 [RF433_LINK_TEST.md](RF433_LINK_TEST.md)。两块板均已烧录并确认 RF READY；完整回包尚待两板同时供电后实测。

双板 UART 测试见 [UART_TEST.md](UART_TEST.md)。第一块板已确认使用 **DIO1=TX、DIO2=RX**，在 **COM20、115200** 接收 `1920`。

```powershell
.\build.ps1              # 编译
.\build.ps1 flash        # 编译并烧录
.\build.ps1 erase        # 整片擦除（芯片连不上时的救命稻草）
```

VSCode 里 `Ctrl+Shift+B` 编译，`F5` 调试（需要 cortex-debug 扩展）。

> ⚠ 和 STM32 工程一样，`build.ps1` 必须存成 **UTF-8 with BOM**，
> 而且它会把 `sh/rm/mkdir/cat`（make 要用）和 `openocd` 加进 PATH 再调 make。
> 直接敲 `make` 会报「无法将"make"项识别为 cmdlet」。

---

## 1. 环境组成（全部现成，没有新装东西）

| 组件 | 用的是什么 | 位置 |
|---|---|---|
| 编译器 | arm-none-eabi-gcc **7.3.1** | 借 STM32CubeIDE 1.5.0 自带的 |
| make | GNU Make 4.2.1 | 同上 |
| 烧录 / 调试 | **OpenOCD 0.12**，自带 `interface/xds110.cfg` 和 `board/ti_cc13x0_launchpad.cfg` | `../../tools/openocd` |
| 外设库 | TI **cc13xxware** driverlib | `Drivers/cc13xxware`（BSD 许可） |

### driverlib 是从哪来的

TI 官方的 SimpleLink CC13x0 SDK 要注册账号、装 1GB+ 安装包。
这里用的是 **Contiki-NG 项目 vendor 出来的 `contiki-ng/cc13xxware`**（869KB），
内容就是 TI 的 driverlib 原文（`Copyright (c) 2015-2016, Texas Instruments`，BSD 三条款），
带 `driverlib/` `inc/` `startup_files/`，够裸机开发用。

以后要上 RF（Sub-1GHz 收发）时，`rf_patches/` 也在里面；
真要用完整协议栈再去装官方 SDK 不迟。

---

## 2. 引脚（LaunchPad）

来自 TI 官方板级头 `CC1310_LAUNCHXL.h`，核实过：

| 信号 | DIO | 说明 |
|---|---|---|
| 红 LED | **DIO6** | 高电平点亮 |
| 绿 LED | **DIO7** | 高电平点亮 |
| BTN-1 | DIO13 | 按下为低 |
| BTN-2 | DIO14 | 按下为低 |
| UART TX | DIO3 | ┐ 接板载 XDS110 的 "Application/User UART" |
| UART RX | DIO2 | ┘ XDS110 Application/User UART 的 COM 号由 Windows 动态分配，请在设备管理器核对 |

> ⚠ **别和自制板搞混**：`CC1310F128RSMR_最小系统_XDS110_SPI_RF连接表.md` 里
> DIO6/DIO7 是接 STM32 的 SPI MOSI/MISO —— 那是自制板的定义。
> LaunchPad 上这两个脚是 LED。

---

## 3. CC13xx 点灯为什么比 STM32 麻烦

STM32 开个 GPIO 时钟就完事。CC1310 多一层**电源域**，复位后 GPIO 所在的
PERIPH 域是**断电**的，不先上电，后面写 IOC/GPIO 寄存器全部无效 —— 不报错，就是没反应。

正确顺序四步，少一步灯都不亮（见 `Core/Src/app_blink.c`）：

```c
PRCMPowerDomainOn(PRCM_DOMAIN_PERIPH);                       // 1 域上电
while (PRCMPowerDomainStatus(...) != PRCM_DOMAIN_POWER_ON);  // 2 等上电完成
PRCMPeripheralRunEnable(PRCM_PERIPH_GPIO);                   // 3 开模块时钟
PRCMLoadSet(); while (!PRCMLoadGet());                       // 4 ★ 触发加载 ★
```

**第 4 步最容易漏**：CC13xx 的时钟使能是「写寄存器 + 显式加载」两段式的，
只写第 3 步等于没写。漏了它的现象和「引脚接错」一模一样 ——
灯不亮，但读回寄存器看着都是对的。

---

## 4. ★ CCFG：这颗芯片最容易把自己锁死的地方 ★

CC13xx/CC26xx 的 ROM bootloader 每次复位都会读 **flash 最后 88 字节**
（`0x0001FFA8 ~ 0x0001FFFF`）的 CCFG，用它决定：JTAG 调试口开不开、
bootloader 行为、image 是否有效、扇区写保护等。

**烧进一份错误的 CCFG（比如把 CPU_DAP_ENABLE 关掉），XDS110 就再也连不上，
芯片等于废掉。** 所以：

- 链接脚本 `cc1310f128.ld` 给 CCFG 划了独立的 `CCFG` 区域
- Makefile 强制编译 `Drivers/cc13xxware/startup_files/ccfg.c`（TI 官方默认值）
- **绝对不要**图省事把 `.ccfg` 段删掉或自己乱填

本工程实际编出来的 CCFG（已核对）：

| 字段 | 值 | 含义 |
|---|---|---|
| `CPU_DAP_ENABLE` | `0xC5` | **调试口开着** ✅ |
| `TEST_TAP_ENABLE` | `0xC5` | 开着 |
| `IMAGE_VALID` | `0x00000000` | 镜像有效，从地址 0 启动 |
| `CCFG_PROT_*` | `0xFFFFFFFF` | 扇区不写保护 |
| `BL_ENABLE` | `0xFF` | bootloader 后门关（正常，我们走 JTAG） |

验证方法：

```bash
arm-none-eabi-objdump -s -j .ccfg build/cc1310_blink.elf
# 应该看到 88 字节，起始地址 1ffa8
```

万一真锁了，先试 `.\build.ps1 erase`（整片擦除，CCFG 一起擦成 0xFF，
空片默认允许调试）。

---

## 5. 实测：已跑通 ✅

```
   text    data     bss     dec     hex
   3124       0    1568    4692    1254   build/cc1310_blink.elf
```

烧录 + 校验通过，红绿 LED 交替闪烁。

客观验证（不靠肉眼，直接用 JTAG 读 GPIO 寄存器）：

```
DOE31_0  = 0x001000c8        bit6/bit7 输出使能都开着
DOUT31_0 在两个值之间来回翻：
  0x00100040   bit6=1  -> 红灯(DIO6) 亮
  0x00100080   bit7=1  -> 绿灯(DIO7) 亮
```

连接信息（连上之后 OpenOCD 的输出，留作对照）：

```
Info : JTAG tap: cc13x0.jrc tap/device found: 0x2b9be02f
       (mfg: 0x017 Texas Instruments, part: 0xb9be, ver: 0x2)
Info : JTAG tap: cc13x0.cpu enabled
Info : [cc13x0.cpu] Cortex-M3 r2p1 processor detected
Info : [cc13x0.cpu] Examination succeed
```

### ⚠ 踩过的坑：隔离跳线没插 = JTAG 全 0

第一次连的时候报的是：

```
Info : XDS110: connected, firmware version = 3.0.0.32     <- 调试器本身没问题
Error: JTAG scan chain interrogation failed: all zeroes
Error: cc13x0.jrc: IR capture error; saw 0x00 not 0x01
```

原因是**板子中间那排隔离跳线没插全**
（丝印 `GND / 5V / 3V3 / RXD<< / TXD>> / RST / TMS / TCK / TDO / TDI`，
把左边 XDS110 和右边 CC1310 连起来）。插好后一次就连上了。

判断要点：**XDS110 能连上不代表目标能连上**。日志里
「XDS110: connected / firmware version」只说明 USB 那一段通了，
真正说明目标活着的是 `JTAG tap: ... tap/device found: 0x2b9be02f` 这一行。

`all zeroes` 表示 TDO 恒为低。线悬空通常读到全 1（有上拉），
读到全 0 一般是**目标没供电 / 被按在复位上 / JTAG 信号根本没接过去**。

浪费时间的排查（现象都不变，下次别再走一遍）：
JTAG 降速到 200kHz、强制 `arp_init-reset` 触发 cJTAG→4线转义、
`reset_config none`、换 transport（xds110 只有 jtag/swd，CC13x0 不支持 SWD）。

---

## 6. 射频（prop mode，Sub-1GHz）

裸机实现，不用 TI-RTOS 的 RF 驱动，命令直接走 RF core 的门铃(doorbell)。

```powershell
.\build.ps1 APP=rftx BAND=868 flash  # 在 cc1310 目录中运行该目录自带的构建脚本
```

### 启动顺序（顺序错了各种奇怪现象）

```
1. RF core 电源域上电          少了：写 RF core 寄存器全无效
2. 打开 RF core 时钟           少了：同上
3. ★ 切到 24MHz 晶振 ★        少了：合成器锁不上，CMD_FS 报错
4. CMD_PING 唤醒 RF core
5. 打 CPE/RFE 补丁
6. CMD_PROP_RADIO_DIV_SETUP    配调制/速率/频段
7. ★ RFCRfTrimRead/Set + RFCRTrim ★  套用芯片出厂 trim
8. CMD_FS                      锁频
9. CMD_PROP_TX                 发包
```

**第 3 步最容易漏**：复位后跑的是 RCOSC_HF（48MHz RC 振荡器，精度百分之几），
射频合成器要晶振级精度。不切晶振时 **CPU 和串口都完全正常，只有射频不工作** ——
很容易往配置上找半天。

**第 7 步**：每颗芯片的射频参数出厂时测过存在 FCFG 里，不套用的话频偏和输出功率
会有个体差异，而且每颗偏得不一样。

### 判断命令成功要看两层

```c
1) 门铃返回的 CMDSTA   —— 命令有没有【被接收】
2) 命令结构里的 status —— 命令有没有【执行成功】
```

只看第一层的话，"命令被接收但执行失败"会被当成成功，表现为
「初始化每步都过了但就是发不出去」。`cmd_execute_sync()` 两层都查。

### 频段配置

全部集中在 `Core/Inc/rf_prop_band.h`，换频段只改这一个文件。

| 频段 | 状态 |
|---|---|
| **868 MHz** | ✅ 可用。50kbps 2-GFSK，配置来自 Contiki-NG 的 smartrf-settings.c（BSD-3，SmartRF Studio 导出），在 LAUNCHXL-CC1310 上被大量项目实跑过 |
| **433.92 MHz** | 已从 V1 移入完整配置及 RX 支持；板子1 RF 初始化、发包命令成功，双向验证进度见 RF433_LINK_TEST.md |

**频段参数不能只修改中心频率**：

overrides[] 是 SmartRF Studio 针对具体频段+调制参数生成的，含合成器 RTRIM、
精校准、环路带宽、VCO/LDO、抗混叠带宽、PA 斜坡、AGC 参考、RSSI 偏移。
填错的后果**不是"不工作"，而是"看着在工作但实际不对"**：合成器锁得上、
命令返回成功、示波器上也有输出，但频偏/杂散/邻道抑制/灵敏度全不达标 ——
这种问题几乎无法从现象反推。433 要的是 `override_synth_prop_430_510_div10.xml`
那一套（`loDivider=0x0A`、RSSI 偏移 `0x000288A3`），和 868 的 div5 那套不通用。

当前参数保存在 `rf_prop_band.h`，433 分支记录了本地原配置引用的 TI SmartRF 导出来源。

### ⚠ 433 在这块 LaunchPad 上能跑到什么程度

要区分「芯片」和「板子」——我一开始说得太笼统，查了一手资料后修正：

| | 支不支持 433 |
|---|---|
| **CC1310 芯片** | ✅ 支持 431–527MHz（需 die Rev B 及以后） |
| **LAUNCHXL-CC1310 板** | ❌ 射频前端（balun + 匹配 + PCB 天线）是 868/915 的 |

一手依据：

- **TI 产品页** [ti.com/tool/LAUNCHXL-CC1310](https://www.ti.com/tool/LAUNCHXL-CC1310)：
  *"broad band antenna supports both **868 MHz** ISM band for Europe and
  **915 MHz** ISM band for US with a single board"* —— 全页没提 433
- **TI E2E**：*"the launchpad is not designed to give good performance on 433 MHz"*
- **TI SWRA640**（已存到 `../../LAUNCHXL-CC1310/`）：433MHz 的官方板是
  LAUNCHXL-CC1350-4 / CC1352P-4 / LP-CC1352P7-4，**不含 LAUNCHXL-CC1310**

所以在这块板上配 433：

- ✅ **能做**：合成器照样锁得住，可以验证固件/协议/收发流程；两块板放桌上
  近距离（几十厘米）多半也能通 —— 做**功能验证**够用
- ❌ **不能做**：距离、灵敏度、输出功率、杂散 —— 任何可测量的射频指标

**433 的性能验证必须换板**：CC1310EM-433、LAUNCHXL-CC1350-4，
或者你自己那块 433 板（那本来就是最终目标）。

> 顺带，SWRA640 里对你自制 433 板有一条直接可用的结论：
> 433–510MHz 的天线匹配需要额外加一个 **LANT 电感，433MHz 取 51nH**。

另外 **433MHz 需要 die Revision B 或更高**（见 `../../CC1310_技术支持与官方资源.md`）。

### 踩过的坑：不要自己重写 TI 的编码宏

`rf_mailbox.h` 里已经有 `HW_REG_OVERRIDE / ADI_REG_OVERRIDE /
ADI_HALFREG_OVERRIDE / MCE_RFE_OVERRIDE`。我一开始照着文档自己写了一套，
**位序是反的**（TI 是「地址在低位、值在高位」，我写反了），
整张覆盖表会写到完全错误的寄存器上，而且不报错。
是编译器的 `redefined` 警告抓到的。**厂商头文件里已有的编码宏一律直接用。**

---

## 7. 工程结构

```
sofeware_V2/cc1310/
├── .vscode/                    VSCode 配置（编译任务 / cortex-debug 调试）
├── Core/
│   ├── Inc/board.h             LaunchPad 引脚定义
│   ├── Src/app_blink.c         LED 闪烁 demo
│   ├── Src/app_rftx.c          RF 发射 demo
│   ├── Src/app_spirx.c         SPI 接收 demo
│   └── Src/rf_prop.c           RF core 初始化/命令封装
├── Drivers/cc13xxware/         TI driverlib（driverlib / inc / startup_files）
├── cc1310f128.ld               链接脚本（含 CCFG 区域划分）
├── Makefile
├── build.ps1
└── README.md
```

当前 V2 的 CC1310 Makefile 默认 `APP=blink`、`BAND=868`。现有 app 为 `blink`、`rftx`、`spirx`、`uart1920`、`uart2526`、`rf433_board1`、`rf433_board2`。V2 的双板无线应用为独立实现，不使用 V1 的 `app_rfpeer.c`。


本目录 `Core/Inc/rf_prop_band.h` 现已包含 868 MHz 与 433.92 MHz 配置。两个 `rf433_board*` 应用必须指定 `BAND=433`。MCU 不能识别被动天线型号或连接状态，完整链路需以对端收到正确负载并返回数据为准。

新增 demo：写 `Core/Src/app_<名字>.c`，从 `cc1310` 目录运行 `.\build.ps1 APP=<名字>`。
