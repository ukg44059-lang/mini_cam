# STM32U575 工程模板（LED 例程 + OV3660 摄像头验证 + USB 实时图传）

**433 MHz 无线图像：收发固件已烧录，COM25 无线图像接收与网页显示已验证；详情见 [RF_IMAGE_LINK.md](RF_IMAGE_LINK.md)。COM20 是发送板可选状态口。**

性能优化版：433.92 MHz / 500 kbps，硬件 SPI2 2.5 MHz，COM25=921600，COM22=1000000；VGA 画质预设及实测见 [RF_PERFORMANCE.md](RF_PERFORMANCE.md)。无线模式自动关闭图像镜像，STM32 直接模式自动恢复。

**新增：板载 CC1310 SPI 图像转发与网页来源选择，见 [SPI_IMAGE_LINK.md](SPI_IMAGE_LINK.md)。**
按 PB13/PB14/PB15 实际接线使用 **SPI2 AF5**（数据手册不支持这三个引脚映射 SPI3）。
`TRANSPORT=dual` 同时镜像到 STM32 COM22 和 CC1310 COM20，网页可切换接收。

目标芯片：**STM32U575ZIT6Q**（Cortex-M33，2 MB Flash，786 KB SRAM，LQFP144）
开发板：**NUCLEO-U575ZI-Q**（MB1549，板载 STLINK-V3E）
参考文档：`../STM32U575_docs/UM2861_NUCLEO-U575ZI-Q_用户手册.pdf`、`DS13737` 数据手册

---

## 0. 版本说明与 demo 入口

本目录是 **sofeware_V2**。原 README 与 `../sofeware_V1/README.md` 内容相同，残留了 V1 的目录名、固定串口号和不完整的工程结构；本版已按当前 V2 文件核对更新。V2 当前包含 `Core/Src/app_uart.c`、`test_pwm/`、摄像头图传工具与抓图记录；不要把 V1 的 `cc1310-test/`、`esp32-demo/` 或 RF peer demo 当成 V2 已包含功能。

### demo 怎么切

> ⚠ **这台机器上没有全局的 `make`**，直接敲会报「无法将"make"项识别为 cmdlet」。
> 用仓库里的包装脚本 `build.ps1`，参数原样透传给 make：

```powershell
.\build.ps1 APP=camera        # 默认：OV3660 摄像头验证，逐阶段自检，抓一帧就停
.\build.ps1 APP=stream        # OV3660 实时图传（走 USB），配 tools/stream_server.py 在网页上看
.\build.ps1 APP=led           # LED 闪烁 + 串口打印，最小可用工程
.\build.ps1 APP=uart          # UART demo
.\build.ps1 APP=spitx         # SPI 发射 demo
.\build.ps1 APP=test_pwm     # 摄像头接口逐线 PWM 测试（先读 test_pwm/README.md）
.\build.ps1 APP=led flash     # 编译并烧录
.\build.ps1 clean
```

`build.ps1` 干了两件事，缺一个都编不了：用 CubeIDE 1.5.0 自带的 make，
以及把 `sh/rm/mkdir/cat` 这些 Unix 工具加进 PATH（Makefile 里的 `$(shell ...)` 要用）。
在 Git Bash 里则可以直接调那个 make.exe，PATH 本来就齐。

`APP` 决定编译对应的 `Core/Src/app_<APP>.c`（`test_pwm` 除外，源文件在 `test_pwm/app_pwm.c`）；常规 app 共用 `Core/Src/bsp.c`（时钟 / 电源 / 串口 / LED）。

**串口**：`camera` 和 `led` 是 **921600 8N1**；`stream` 的日志口是 2000000，
图像默认不走串口而走 USB（见 1b 节）。

> ⚠ **改了 `APP` / `BAUD` / `FRAMESIZE` / `TRANSPORT` 会自动全量重编。**
> 这几个参数只改编译选项、不改源文件时间戳，make 本来是看不出来的，
> 结果就是「改了参数烧上去还是旧的」。Makefile 里存了一份配置指纹，对不上就删掉 `build/` 重编。

---

## 1. app_camera —— OV3660 验证 demo

分 7 个阶段跑，**每一阶段失败都会停下来并打印该查什么**，不会稀里糊涂往下走：

| 阶段 | 做什么                                     | 通过说明什么                                      |
| ---- | ------------------------------------------ | ------------------------------------------------- |
| 0    | 按序开底板 LDO，量四条控制线 + 11 根数据线 | 底板供电、Nucleo 引脚、模组在不在位               |
| 0.5  | **逐根拉低数据线，找连在一起的引脚对**     | 11 根数据线互相独立，没有连锡                     |
| 1    | SCCB 读芯片 ID                             | 供电、XCLK、I2C 接线、PWDN/RESET 时序**全部**正确 |
| 2    | 寄存器写-读回环                            | SCCB 双向都通，时序余量够                         |
| 3    | 软复位 + 197 条默认表 + RGB565 + QVGA      | 传感器接受配置                                    |
| 4    | 把 VSYNC/HREF/PCLK 当 GPIO 采样 200ms      | 传感器真的在输出，且能推断 VSYNC 极性             |
| 5    | DCMI + GPDMA 抓一帧                        | 8 根数据线和同步线都对                            |
| 6    | 统计画面亮度/最值/未覆盖像素               | 区分「全黑 / 全白 / 花屏 / 正常」                 |
| 7    | 按 B1 把整帧从串口吐出                     | PC 端存成 PNG 肉眼确认                            |

### 阶段 1 最有价值

它只跑 SCCB，**不开数据流，电流最小**。读到 `0x3660` 基本就说明硬件没问题了。
排查摄像头永远先看这一步，别急着调 DCMI。

### 看图

```bash
pip install pyserial pillow
python tools/recv_frame.py COMx      # 将 COMx 换成当前 ST-LINK 虚拟串口；再按板子上的 B1
```

收完自动存成 `frame_<时间戳>.png`。校验和不对会提示（说明串口丢字节）。

### 彩条测试图

长按 B1 超过 2 秒 → 打开传感器内部彩条。
**镜头/光路有问题时用它把问题一分为二**：彩条能正常抓到，说明 DCMI 通路没问题，问题在光学侧；彩条也是花的，问题在数据线。

### ⚠ 图像结构正常但颜色不对 → 先看阶段 0.5

FPC 是 0.5mm 间距，**相邻脚连锡的症状极其隐蔽**：画面轮廓、渐变、暗角全都正常，
只是颜色整体偏成某个色调。因为两根数据线合并 = 某几个颜色 bit 恒等，亮度信息几乎不受影响。

阶段 0.5 就是专门抓这个的。它的做法是：**保持 2.8V 供电**，把传感器打进掉电+复位让输出转高阻，
然后逐根拉低、看别的线跟不跟着走。两个前提都不能少：

- 传感器还在输出 → 读到的是图像数据，测不出短路
- 把 2.8V 也关掉 → 模组每个脚到 DOVDD(此时 0V) 的 ESD 二极管会把内部上拉钳在 0.6V，
  **所有线都读成 0**，同样测不出来

### RGB565 是大端

OV 传感器的 RGB565 **高字节先出**，DCMI 按接收顺序填内存，所以 `buf[2i]` 是高字节。
按小端拼会得到一张**结构正确但颜色乱成彩虹**的图 —— 别误判成接线问题。

---

## 1b. app_stream —— 实时图传（USB CDC 或 UART）

板子连续抓帧 -> USB CDC 或 UART 发到本机 -> Python 转成 MJPEG -> 浏览器实时显示。

**默认图像走 USB CDC**（CN15 的 Type-C）；也可以编译成 **UART 串口传图**。UART 模式下图像数据和启动日志共用 ST-LINK 虚拟串口，运行网页接收程序时不要再让 VOFA、串口助手等程序占用该 COM 口。

```powershell
.\build.ps1 APP=stream flash        # 默认 QQVGA 160x120，USB 传输
python tools\stream_server.py COMx  # 换成 CN15 USB CDC 当前枚举出的端口（不是固定 COM 号）
```

**要插两根 USB 线**：CN1(Micro-B) 供电 + 烧录 + 日志，CN15(Type-C) 传图像。
插上 CN15 后设备管理器里会多一个 `STMicroelectronics Virtual COM Port`，
Windows 10 以后自带 usbser.sys 驱动，**不用装驱动**，插上就能用。

> 顺序无所谓。固件会等 PC 把端口打开再开始发（靠 CDC 的 DTR 判断），
> 等待期间红灯慢闪，日志口会打印它在等什么。

### 改用 UART 串口传图

在 **V2 工程根目录**打开 PowerShell。先关闭 VOFA/串口助手，然后编译并通过 ST-LINK/OpenOCD 烧录 UART 版本：

```powershell
.\build.ps1 APP=stream TRANSPORT=uart BAUD=2000000 flash-ocd
```

这里 `TRANSPORT=uart` 选择板上的 ST-LINK 虚拟串口传图，`BAUD=2000000` 必须与 PC 接收端一致；`flash-ocd` 会编译、烧录并校验。UART 模式只需连接板子的 ST-LINK USB 口，不需要用 CN15 USB CDC 传图。

烧录完成后，先查 Windows 实际分配的串口号：

```powershell
python -m serial.tools.list_ports -v
```

找到对应板子的 ST-LINK 虚拟 COM 端口（不是凭旧记录猜 COM 号），再启动网页接收端：

```powershell
python tools\stream_server.py COMx --baud 2000000
```

把 `COMx` 替换成上一步查到的端口，例如 `COM12`。接收程序会自动打开 `http://localhost:8080`；若没自动弹出浏览器，手动访问该地址。按 `Ctrl+C` 结束接收服务。串口被占用时报错时，先关闭 VOFA 等其他串口软件再重试。

首次使用如提示缺少依赖，在工程根目录安装：

```powershell
python -m pip install pyserial pillow numpy
```

USB CDC 模式则仍按上面的默认 USB 步骤运行，并将接收端 COM 号指向 CN15 枚举出的 CDC 端口；不要把 USB CDC 端口和 UART 图像端口混用。

### 网页上能实时调的三样东西

| 控件                                    | 改的是哪一端 | 说明                                                     |
| --------------------------------------- | ------------ | -------------------------------------------------------- |
| **压缩方式** 原始像素 / 硬件 JPEG       | **板子**     | 切到硬件 JPEG 后所有分辨率都能用，见下表                 |
| **分辨率** 9 档，160×120 ~ 2048×1536    | **板子**     | 下发命令，板子重配传感器 + 重开 DCMI。切换瞬间掉一两帧   |
| **帧率上限** 不限 / 1 / 2 / 5 / 10 / 15 | **板子**     | 板子照抓不误，只是到点才发，所以**限帧不会增加画面延迟** |
| **传感器压缩 qs** 8~48                  | **板子**     | OV3660 的量化步长，越小越清晰。只在硬件 JPEG 下有效      |
| **JPEG 质量** 10~95                     | **本机**     | 只在原始像素模式下有效，不影响板子发的数据               |

### 两种压缩方式

|                | 原始像素                     | **硬件 JPEG**                          |
| -------------- | ---------------------------- | -------------------------------------- |
| 谁压的         | 不压，板子直接发 RGB565/灰度 | **OV3660 内部硬件压缩**                |
| 分辨率上限     | 640×480（RAM 卡死）          | **2048×1536 满分辨率**                 |
| 一帧多大       | VGA 614400 字节              | VGA 约 22 KB，QXGA 约 134 KB           |
| 网页上存下来的 | 本机 Pillow 压的 JPEG        | **传感器直出的原始码流，不经二次编码** |

硬件 JPEG 把 RAM 这道墙彻底绕开了：不用存整帧像素，只存压缩后的码流，
614400 字节的池子随便放，所以**所有分辨率都能用**。

### 各档实测（全部 0 丢帧 0 校验错）

原始像素模式：

| 档  | 分辨率  | 格式   | 每帧字节 | 缓冲 | fps      | 吞吐     |
| --- | ------- | ------ | -------- | ---- | -------- | -------- |
| 0   | 160×120 | RGB565 | 38400    | 双   | **17.8** | 668 KB/s |
| 1   | 320×240 | RGB565 | 153600   | 双   | 5.2      | 773 KB/s |
| 2   | 400×300 | RGB565 | 240000   | 双   | 3.4      | 792 KB/s |
| 3   | 480×360 | RGB565 | 345600   | 单   | 1.5      | 501 KB/s |
| 4   | 640×480 | RGB565 | 614400   | 单   | 0.9      | 534 KB/s |
| 5   | 800×600 | 灰度   | 480000   | 单   | 1.1      | 521 KB/s |

硬件 JPEG 模式（qs=12）：

| 档  | 分辨率        | JPEG 一帧  | 压缩比     | fps     | 吞吐     |
| --- | ------------- | ---------- | ---------- | ------- | -------- |
| 0   | 160×120       | 3.3 KB     | 11.8:1     | 8.9     | 28 KB/s  |
| 1   | 320×240       | 8.5 KB     | 18.1:1     | 8.9     | 74 KB/s  |
| 2   | 400×300       | 10.0 KB    | 23.9:1     | 8.9     | 87 KB/s  |
| 3   | 480×360       | 14.1 KB    | 24.4:1     | 6.6     | 91 KB/s  |
| 4   | 640×480       | 21.7 KB    | 28.3:1     | 8.9     | 191 KB/s |
| 5   | 800×600       | 29.4 KB    | 32.7:1     | 8.9     | 255 KB/s |
| 6   | 1024×768      | 46.2 KB    | 34.1:1     | 5.9     | 268 KB/s |
| 7   | 1600×1200     | 90.3 KB    | 42.5:1     | 3.0     | 262 KB/s |
| 8   | **2048×1536** | **134 KB** | **46.9:1** | **3.0** | 394 KB/s |

对比最能说明问题的一组：
**640×480 原始要 614400 字节只能跑 0.9 fps，硬件 JPEG 只要 21.7 KB 能跑 8.9 fps。**
而 2048×1536（原始要 6 MB，物理上不可能）在 JPEG 下轻松跑到 3 fps。

### 传感器的压缩强度 qs

寄存器 `0x4407`，**数值越小画质越好、码流越大**（和常见的「质量 0~100」方向相反）。
640×480 实测：

| qs   | 8       | 12      | 20      | 32      | 48      |
| ---- | ------- | ------- | ------- | ------- | ------- |
| 一帧 | 24.5 KB | 23.0 KB | 16.0 KB | 14.0 KB | 11.7 KB |

⚠ **上限只开到 48**：实测 qs=63 时传感器只吐 68 字节的空壳 JPEG（没有图像数据），
等于坏帧，所以固件和网页都把上限收在 48。

分辨率切换会跨过 HAL 那个 64KB 的 DMA 链表分界（见下面「坑」那节），
所以 `apply_framesize()` 里必须用新的帧字节数重新初始化 DCMI。

限帧的实测值会有量化：设 5 fps 实际得到 4.45。因为传感器每 56ms 出一帧，
200ms 的间隔要等到第 4 帧（224ms）才够，这是帧周期的整数倍限制，不是 bug。

**原始像素模式下**，JPEG 大小和链路吞吐没有关系（JPEG 是本机压的）：

| 质量 | JPEG 大小 | 压缩比 | 链路吞吐 |
| ---- | --------- | ------ | -------- |
| 95   | 7949 B    | 4.8:1  | 667 KB/s |
| 85   | 4685 B    | 8.2:1  | 667 KB/s |
| 60   | 2994 B    | 12.8:1 | 667 KB/s |
| 20   | 1885 B    | 20.4:1 | 667 KB/s |

吞吐一直是 667 —— 板子发的是 RGB565 原始像素，JPEG 是本机压的，调质量省不了带宽。
**要省带宽就切硬件 JPEG**：压缩发生在传感器里，链路上跑的就是压缩后的码流
（VGA 从 614400 字节降到 21.7 KB）。

「保存当前帧」存的就是**正在推给浏览器的那串 JPEG 字节本身**，不重新编码，落在 `captures/`。
文件名标明是谁压的：`..._640x480_hwjpeg_qs12.jpg`（传感器压的）/
`..._640x480_pc_q85.jpg`（本机压的）。
**硬件 JPEG 模式下存下来的就是 OV3660 原封不动吐出来的码流**，一个字节都没动过。
存完网页上直接显示字节数和压缩比，不用再去资源管理器看属性。

不重新编码是有意的：重新编码的话画面已经变了，磁盘上的大小就和统计里
「JPEG 当前帧」对不上了。现在两个数字保证是同一个值，调质量滑块前后各存一张，
文件大小的差异就是质量参数的真实效果。

### 命令协议

PC → 板子走的是同一个口的反方向，几条 ASCII 短行，用任何串口工具手敲都能调：

```
R<n>\n    切到第 n 档分辨率（0~8，见上面的表）
M<0|1>\n  0 = 硬件 JPEG，1 = 原始像素
Q<n>\n    传感器 JPEG 量化步长 8~48，越小越清晰
F<n>\n    限制帧率为 n fps，F0 = 不限
```

板子**故意不回复**：回复会混进图像流，PC 端虽然能靠找帧头恢复，
但会平白多出「重同步」计数，看着像出问题。命令有没有生效，
看帧头里的宽高和统计里的 fps 就知道了 —— 网页上的按钮高亮就是按
**板子实际报上来的宽高**点亮的，不是按你点了哪个。

### 实测：USB 比串口快 3~5 倍

| 分辨率        | 每帧字节 | UART 2Mbaud         | **USB CDC**              | 提升 |
| ------------- | -------- | ------------------- | ------------------------ | ---- |
| QQVGA 160x120 | 38400    | 5.19 fps / 194 KB/s | **17.79 fps / 667 KB/s** | 3.4x |
| QVGA 320x240  | 153600   | 1.1 fps / 194 KB/s  | **5.16 fps / 774 KB/s**  | 4.7x |

四种组合全部 **0 丢帧 0 校验错 0 重同步**。

**QQVGA 那档 17.79 fps 已经是传感器自己的上限**（驱动里那档 PLL 的注释写的就是
17.77 FPS），也就是说瓶颈已经从链路转移到摄像头了，USB 还有余量。
QVGA 那档 774 KB/s 则是 USB 全速的实际极限，想再快只能上 JPEG 或者 USB HS。

```powershell
.\build.ps1 APP=stream FRAMESIZE=QVGA flash    # 大图，5.16 fps
.\build.ps1 APP=stream TRANSPORT=uart flash    # 退回串口传输（USB 出问题时的退路）
```

`TRANSPORT=uart` 时图像和日志共用 ST-LINK 那个口，PC 端要加 `--baud 2000000`。

### USB 这块有几个 U5 特有的坑

都写在 `Core/Src/usbd_conf.c` 的文件头注释里，这里只列结论：

- **必须 `HAL_PWREx_EnableVddUSB()`**。U5 的 USB 收发器是独立供电域，复位后是关的。
  不开的话插上电脑**一点反应都没有**，和线没插一样，特别难查。
- **48MHz 时钟走 `RCC_PERIPHCLK_ICLK` + `RCC_CLK48CLKSOURCE_HSI48`**，
  不是别的系列那套 CLK48/PLLQ 宏。顺手开了 CRS 拿 USB 的 SOF 自动校准 HSI48。
- **`vbus_sensing_enable` 必须 DISABLE**。本板的 VBUS_SENSE 接的是 PC2 普通 GPIO(SB6)，
  没接到 OTG 的专用检测脚，开了会一直认为没 VBUS 而不上拉 DP，主机根本看不到设备。
- **别调 `HAL_PWREx_DisableUCPDDeadBattery()`**。Type-C 靠 CC 线上的 dead-battery Rd
  让主机认出「这是个设备」并供 VBUS，撤掉 Rd 主机就断电了。
- **`OTG_FS_IRQHandler` 别忘了写**（在 `stm32u5xx_it.c`），忘了同样是插上毫无反应。

能跑满是因为用了**双缓冲 + 抓发重叠**：DCMI 往 buffer B 搬的同时，CPU 把 buffer A
从串口发出去，总时间从 `抓帧 + 发送` 变成 `max(抓帧, 发送)`。
对应 `CAM_CaptureStart()` / `CAM_CaptureWait()` 这对非阻塞接口。

### 抓不到帧时会自己打诊断

`app_stream` 试完四种 DCMI 极性都失败的话，会把传感器的实际输出尺寸寄存器
（0x3808~0x380F）读出来，再把 VSYNC/HREF/PCLK 当普通 GPIO 采样 300ms，
用来区分两类完全不同的故障：

- **VSYNC 不翻转** → 传感器根本没在输出，是寄存器把它配停了
- **VSYNC 在翻转、尺寸也对** → 传感器没问题，是 DCMI/DMA 这边的配置不对

---

## 2. 硬件组合与 OV3660 引脚

**NUCLEO-U575ZI-Q 插在自制底板上，OV3660 通过 24P FPC 接在底板的 FPC1。**
引脚来自底板网表 `../Netlist_Schematic1_1_2026-08-27.tel`，
AF 编号逐条对照 **DS13737 Rev10 的 Table 26/27/28** 核实过，不是猜的：

| FPC 脚 | 信号          | STM32              | AF            | 说明                          |
| ------ | ------------- | ------------------ | ------------- | ----------------------------- |
| 3      | SIOD / SDA    | **PD13**           | AF4           | I2C4_SDA，4.7k 上拉到 2V8_NEW |
| 5      | SIOC / SCL    | **PD12**           | AF4           | I2C4_SCL，4.7k 上拉到 2V8_NEW |
| 6      | RESETB        | **PA2**            | GPIO **开漏** | 低有效，4.7k 上拉到 2V8_NEW   |
| 7      | VSYNC         | PB7                | AF10          | DCMI_VSYNC                    |
| 8      | PWDN          | **PA3**            | GPIO **开漏** | 高有效，4.7k 上拉到 2V8_NEW   |
| 9      | HREF          | **PD8**            | AF10          | DCMI_HSYNC                    |
| 12     | D9 (MSB)      | **PB9**            | AF10          | DCMI_D7                       |
| 13     | XVCLK         | PA8                | AF0           | MCO，经 33R 串联              |
| 14     | D8            | PB8                | AF10          | DCMI_D6                       |
| 16     | D7            | PB6                | AF10          | DCMI_D5                       |
| 17     | PCLK          | PA6                | **AF4**       | DCMI_PIXCLK（注意不是 AF10）  |
| 18     | D6            | PE4                | AF10          | DCMI_D4                       |
| 19     | D2 (LSB)      | PC6                | AF10          | DCMI_D0                       |
| 20     | D5            | PC9                | **AF4**       | DCMI_D3（注意不是 AF10）      |
| 21     | D3            | PC7                | AF10          | DCMI_D1                       |
| 22     | D4            | PC8                | AF10          | DCMI_D2                       |
| 2, 15  | AGND / DGND   | GND                | —             |                               |
| 4      | AVDD 2.8V     | 2V8_NEW 经 L1 磁珠 | —             |                               |
| 10     | **DVDD 1.5V** | 1V5_NEW            | —             | ⚠ 见第 7 节「摄像头供电」     |
| 11     | DOVDD 2.8V    | 2V8_NEW            | —             |                               |

模组只引出 8 根数据线，接的是 sensor 的 **D[9:2]**（丢掉 2 个 LSB），不是字面上的 D[7:0]。

### 底板电源由 MCU 控制

三路 LDO 的使能脚都带 4.7k **下拉**，上电默认全关，必须固件拉高：

| GPIO    | 控制              | 产生                                      |
| ------- | ----------------- | ----------------------------------------- |
| **PD6** | RT9013 EN         | TP_3V3（另外三颗 LDO 的输入，必须最先开） |
| **PD7** | U8 (TPS7A0215) EN | 1.5V → DVDD                               |
| **PD5** | U2 (TPS7A0228) EN | 2.8V → AVDD + DOVDD                       |
| PD4     | U1 (TPS7A0218) EN | 1.8V（摄像头用不到）                      |

`CAM_PowerRailsOn()` 按 **3V3 → 1V5 → 2V8** 顺序打开，顺序写死在固件里，不会搞反。

### ⚠ RESETB / PWDN 必须配成开漏

底板上这两个脚用 4.7k 上拉到 **2.8V**，而 STM32 的 VDD 是 **3.3V**。
推挽输出高电平会顶到 3.3V，**超过 OV3660 数字脚的绝对最大值 DOVDD+0.3V = 3.1V**。
代码里已经用 `GPIO_MODE_OUTPUT_OD`，高电平交给外部上拉产生，就是干净的 2.8V。

### Nucleo 板载 LED 的冲突

- **PC7** = DCMI_D1，同时是板载绿灯 LD1
- **PB7** = DCMI_VSYNC，同时是板载蓝灯 LD2（DCMI_VSYNC 在这颗芯片上**只有 PB7 一个选项**，没有备选）

原型验证不拆也能用，只是 LED 会跟着帧同步闪。要彻底解决就把 Nucleo 上的 **R50** 拆掉。
所以 camera demo 只用 **PG2 上的红灯 LD3** 做状态指示。

---

## 3. 环境组成（已经配好，不用再装东西）

| 组件                | 版本                                             | 位置                                                                                  |
| ------------------- | ------------------------------------------------ | ------------------------------------------------------------------------------------- |
| 编译器              | arm-none-eabi-gcc **7.3.1**                      | 借 STM32CubeIDE 1.5.0 自带的：`F:\stm32cube_ide\...\gnu-tools-for-stm32...\tools\bin` |
| make                | GNU Make 4.2.1                                   | 同上 `...externaltools.make.win32...\tools\bin`                                       |
| 调试 / 烧录         | OpenOCD **0.12.0**（自带 `target/stm32u5x.cfg`） | `G:\material\cam_hardware\tools\openocd`                                              |
| HAL 库              | STM32U5xx HAL Driver **V1.6.3**                  | `Drivers/STM32U5xx_HAL_Driver`                                                        |
| CMSIS Core          | **6.1**                                          | `Drivers/CMSIS/Include`                                                               |
| OV3660 寄存器表     | 转自 espressif/esp32-camera (Apache-2.0)         | `Drivers/BSP/OV3660/ov3660_regs.h`                                                    |
| USB Device 中间件   | STM32_USB_Device_Library **V2.11.6**             | `Middlewares/ST/...`（GitHub 拉的，CDC 类）                                           |
| SVD（调试看寄存器） | STM32U575                                        | `tools/STM32U575.svd`                                                                 |

> 机器上装的 **STM32CubeIDE 是 1.5.0（2020年）、CubeMX 的芯片库里也没有 U575**，
> 两个都不能用来生成 U5 工程，所以这里**只借它的 gcc 和 make**，工程用独立 Makefile 管。
> 板载 STLINK-V3E 固件是 V3J15M7，支持 U5。

VSCode 扩展（已安装）：`ms-vscode.cpptools`、`marus25.cortex-debug`、`dan-c-underwood.arm`、`zixuanwang.linkerscript`

---

## 4. 怎么用

**用 VSCode 打开 `sofeware_V2` 这个文件夹**（不要打开 V1 或上一级 `cam_hardware`，否则本目录配置不生效）。

- `Ctrl+Shift+B` 编译
- `F5` 当前 launch 配置指向 `build/STM32U575_LED.elf`，仅适用于 LED 固件；调试其他 app 前需调整 `.vscode/launch.json`
- 烧录目标以根目录 Makefile 为准（`flash`、`flash-ocd`、`flash-dap`），请按实际连接的板卡选择
- 用 `build.ps1` 包装构建，避免依赖终端当前 PATH

---

## 5. 工程结构

```
sofeware_V2/
├── .vscode/                       VSCode 配置（任务/调试/智能提示）
├── Core/
│   ├── Inc/
│   │   ├── bsp.h                  板载 LED/按键/串口定义
│   │   ├── camera_port.h          ★ 摄像头引脚表 + 板级适配层接口
│   │   ├── stm32u5xx_hal_conf.h   HAL 裁剪配置：开哪些模块
│   │   └── stm32u5xx_it.h
│   ├── Src/
│   │   ├── app_camera.c           ★ 摄像头验证 demo（分阶段自检）
│   │   ├── app_led.c              LED 闪烁 demo
│   │   ├── app_stream.c           ★ USB/UART 实时图传
│   │   ├── app_uart.c             UART demo
│   │   ├── app_spitx.c            SPI 发射 demo
│   │   ├── bsp.c                  时钟 160MHz / SMPS / VDDIO2 / ICACHE / 串口
│   │   ├── camera_port.c          ★ XCLK(MCO) / SCCB(I2C3) / 上电时序 / DCMI+GPDMA
│   │   ├── stm32u5xx_hal_msp.c    外设底层初始化
│   │   ├── stm32u5xx_it.c         中断向量实现
│   │   ├── system_stm32u5xx.c     CMSIS 启动期系统初始化
│   │   ├── syscalls.c             printf 重定向到串口
│   │   └── sysmem.c               堆管理 _sbrk
│   └── Startup/startup_stm32u575xx.s
├── Drivers/
│   ├── BSP/OV3660/                ★ 传感器驱动（与 MCU 无关，靠回调对接）
│   │   ├── ov3660.h / ov3660.c
│   │   └── ov3660_regs.h          197 条初始化表
│   ├── CMSIS/
│   └── STM32U5xx_HAL_Driver/
├── tools/
│   ├── recv_frame.py              ★ PC 端收图并存 PNG
│   ├── stream_server.py           ★ 本地网页实时预览服务
│   └── STM32U575.svd
├── test_pwm/                      摄像头接口逐线 PWM 测试（先读安全说明）
├── cc1310/                        独立 CC1310 裸机工程（能力限制见其 README）
├── STM32U575ZITXQ_FLASH.ld
└── Makefile
```

### 占用

| APP    | Flash | RAM                                  |
| ------ | ----- | ------------------------------------ |
| led    | 21 KB | 3.3 KB                               |
| camera | 41 KB | 158 KB（其中 150 KB 是 QVGA 帧缓冲） |

串口号由 Windows 按当前 USB 连接动态分配。请在设备管理器或运行
`python -m serial.tools.list_ports` 查询，不要照抄其他电脑/上次连接记录里的 COM 号。

---

## 6. 要加新外设怎么办

比如想用 ADC：

1. `Core/Inc/stm32u5xx_hal_conf.h` 里把 `/* #define HAL_ADC_MODULE_ENABLED */` 的注释去掉
2. `Makefile` 的 `C_SOURCES` 里加上 `..._hal_adc.c` 和 `..._hal_adc_ex.c`
3. 在 `stm32u5xx_hal_msp.c` 里写 `HAL_ADC_MspInit()`
4. 重新编译

已启用：CORTEX / **DCMI** / DMA / EXTI / FLASH / GPIO / I2C / ICACHE / PWR / RCC / SPI / TIM / UART

---

## 7. 坑（都是实际踩过或核实过的）

### U5 通用

- **PG2 红灯不亮** → 忘了 `HAL_PWREx_EnableVddIO2()`。U5 的 PG[2:15] 是独立 VDDIO2 电源域。
- **卡在 HAL_RCC_ClockConfig** → 跑 160MHz 必须先 `HAL_PWREx_ControlVoltageScaling(PWR_REGULATOR_VOLTAGE_SCALE1)`，Flash 等待周期给 4。EPOD 升压 HAL 会自动开。
- **程序莫名很慢** → ICACHE 没开。
- **printf 打不出 `%f`** → nano.specs 裁了浮点格式化，把 Makefile 里 `LDFLAGS += -u _printf_float` 取消注释。
- **HSE** → 这块 Nucleo 默认**没焊外部高速晶振**，别配 `RCC_OSCILLATORTYPE_HSE`。板载只有 32.768kHz 的 LSE。
- **B1 按键** → UM2861 明确要求配成输入下拉，配成输出低电平会和按键短路。

### DCMI 特有

- **一帧超过 64KB 必须用链表 DMA**。GPDMA 单个 block 上限 65535 字节，而 QVGA RGB565 是 153600 字节。
  看 `HAL_DCMI_Start_DMA()` 源码：超过 0xFFFF 时它会拆块乒乓搬运，而**这条路径只支持链表模式**，非链表分支直接 `return HAL_ERROR`。
  而且 HAL 会同时改写「头节点」和「头节点的下一个节点」，所以**队列里必须放两个不同的节点**再设循环。只放一个的话两次改写互相覆盖，第一块目标地址就错了。
- **一帧不到 64KB 反而要用「单节点 + 非循环」链表**（这个坑很隐蔽，QQVGA 卡在这里过）。
  同一个 `HAL_DCMI_Start_DMA()` 以 `0xFFFF` 字节为界走两条**互相不兼容**的内部路径：

  | 帧大小                  | HAL 走的分支              | 需要的链表结构      |
  | ----------------------- | ------------------------- | ------------------- |
  | > 0xFFFF（QVGA 153600） | 双节点乒乓，把帧拆成 N 块 | 2 节点 + **循环**   |
  | ≤ 0xFFFF（QQVGA 38400） | 单次传输，只设 Head 节点  | 1 节点 + **非循环** |

  按大帧那套去跑小帧，HAL 只会设第一个节点的长度，**第二个节点长度还是 0，
  循环模式又会转过去**，于是 DMA 停在死胡同、DCMI 的 FRAME 中断永远不来，
  症状是抓帧一直 `HAL_TIMEOUT`——而且传感器侧一切正常，很容易往摄像头上找半天。
  所以 `CAM_DcmiInit()` 多了个 `frame_bytes` 参数，按帧大小建对应的链表。

- **OV3660 硬件 JPEG：压缩使能藏在 `0x3821` 的 bit5**。这是最坑的一条。
  JPEG 通路的寄存器（`0x501F/0x4300/0x3002/0x3006/0x471C/0x4713/0x4407`）全配对了也没用，
  不置这一位 DVP 照样吐 YUV422 原始像素。而且**寄存器回读一切正常**，
  只能从数据本身看出来：开头是 `41 78 40 7E` 这种 YUYV 交替，而不是 `FF D8`。
  这一位在 esp32-camera 里是在 `set_image_options()` 里按当前像素格式加进去的
  （`if (pixformat == JPEG) reg21 |= 0x20;`），移植时很容易漏掉，
  因为它和 binning/镜像/翻转混在同一个函数里，看着像是跟图像方向有关的东西。
- **JPEG 抓帧不能用 FRAME 中断，只能用 VSYNC**。`HAL_DCMI_Init` 只使能
  LINE/VSYNC/ERR/OVR，**FRAME 中断是在 `DCMI_DMAXferCplt` 里才打开的** ——
  而 JPEG 是变长的，DMA 永远填不满、传输完成事件不会来，于是 FRAME 中断永远不使能。
  用 `CAM_CaptureFrame` 只会一直超时。`CAM_CaptureJpeg` 改成数 VSYNC 判断帧结束，
  再扫 `FFD9` 定长度（JPEG 熵编码段里的 `FF` 一律做了字节填充，
  `FF D9` 只可能是 EOI 本身，不会在图像数据里偶然出现）。
- **发送超时必须按帧大小算，不能写死**。原来固定 300ms，USB 约 800 KB/s 只够发 240KB，
  于是 480×360(345600 字节)以上**每一帧都发到一半就超时返回**，PC 收到的永远是残帧。
  症状极具误导性：小分辨率一切正常，一换大分辨率就一帧都收不到，
  而板子日志显示「分辨率切换成功」—— 很容易误判成 DCMI 或传感器的问题。
  定位靠的是数帧头间隔：两个 magic 之间只有 258058 字节，而完整帧应该是 345614。
- **PC 端的帧解析器必须扛得住「假帧头」**。4 字节 magic 在几十万字节像素里是会撞上的
  （`'O''V''3''6'` = 0x4F/0x56/0x33/0x36 正好落在平坦画面常见的字节区间）。
  两条必须做的防护：① 头部要严格校验（宽高是 4 的倍数、整帧不超过物理上限），
  ② **校验和不过时只跳 1 个字节重扫，绝不能把整段 payload 丢掉** ——
  丢掉会把藏在里面的真帧头一起丢了，然后一直对不上，越大的分辨率越容易进死循环。
- **`HAL_DCMI_Start_DMA` 的 Length 单位是「字」不是「字节」**，传字节数会多抓 4 倍。
- **PA6 = DCMI_PIXCLK 是 AF4，PC9 = DCMI_D3 是 AF4**，其余数据/同步线才是 AF10。整排都写 AF10 会挂。
- **VSYNC/HREF 极性**：`0x4740` 默认写的是 `0x21`（PCLK 上升沿、VSYNC 高有效），但不同批次模组可能不同。
  demo 的阶段 4 会实测占空比推断极性，阶段 5 两种都试一遍，不用手工猜。

### 摄像头供电（重要）

OV3660 是**三路电源**：AVDD 2.8V / DOVDD 2.8V / DVDD 1.5V。

- **上电顺序：先 1V5，再 2V8**。反过来的话 I/O 环带电而数字核心不带电，会**闩锁（latch-up）→ 大电流 → 模组发烫到烧毁**。
  固件的 `CAM_PowerRailsOn()` 已经把顺序写死，不用再靠手插跳线控制顺序。
- **每条电源除了使能，还有一道「串接跳线」把 LDO 输出接到负载**。
  不插的话，量 LDO 输出脚一切正常，但负载端是 0V —— 这个坑最容易踩：

  | 跳线   | 连通                  | 封装                     |
  | ------ | --------------------- | ------------------------ |
  | U7     | TP_3V3 → TP_3V3_NEW   | 公针，跳线帽             |
  | **H6** | **1.5V → 1V5_NEW**    | **母座，要用杜邦线短接** |
  | U6     | 2V8 → 2V8_NEW         | 公针，跳线帽             |
  | U5     | 1V8 → 1V8_NEW         | 公针（用不到）           |
  | **H1** | **USB-C 5V → 5V_NEW** | **母座**                 |

  注意 H1 和 H6 是**母座**，插不上普通跳线帽，需要用杜邦线短接，很容易漏掉。

- 三颗 LDO **封装一样、型号只差两位数**（TPS7A02**15**/**18**/**28**），贴片极易调包。
  **DVDD 上了 2.8V 会直接烧模组。**
- FPC 的**第 10 脚(1.5V) 和第 11 脚(2.8V) 物理相邻**，0.5mm 间距连锡 → 2.8V 倒灌进核心电源。

> **量电压要直接量 FPC 脚，不要量 LDO 输出脚**：
> 第 4 脚 = 2.8V(AVDD)、第 10 脚 = **1.5V(DVDD)**、第 11 脚 = 2.8V(DOVDD)。
> 中间隔着串接跳线，LDO 输出正常 ≠ 模组有电。

在当前目录下面，G:\material\cam_hardware\sofeware_V2，跑下面终端的命令
python tools\stream_server.py COMx ，这里的x填写对应的端口号

就目前来看，当这个由于串口带宽有限制，分辨率太高选不了，要把压缩质量选成硬件压缩，然后把图像质量压缩出去
