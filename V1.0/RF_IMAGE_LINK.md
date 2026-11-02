# 433 MHz 无线图像链路（性能优化版）

OV3660 硬件 JPEG → STM32U575 DCMI/GPDMA → 硬件 SPI2 → CC1310 TX → 433.92 MHz / 500 kbps → CC1310 RX → UART → 本地网页。当前两块 CC1310 和 STM32 均已烧录并验证。

## 接口与启动

| 设备 | 串口 | 配置 |
|---|---|---|
| STM32 相机控制/可选图像镜像 | COM22 | **1000000，8N1** |
| CC1310 发送板状态 | COM20 | 115200；DIO1 TX / DIO2 RX |
| CC1310 接收板图像 | COM25 | **921600，8N1**；DIO5 TX / DIO3 RX |
| XDS110 辅助端口 | COM19、COM26 | 不用于图像接收 |

启动：G:\sofeware\anaconda\python.exe tools/stream_server.py --source rf433 --rf-port COM25 --no-browser

打开 http://localhost:8080/，选择「433 MHz 无线接收」、COM25、921600。VGA 预设「画质优先 / 平衡 / 流畅」分别使用传感器量化参数 12 / 20 / 28；参数越大，图像越小、细节越少。预设启用硬件 JPEG、VGA、不限速和并行采集。

无线来源自动关闭 COM22 图像镜像，控制和性能日志仍可用；切回「STM32 直接接收」会自动恢复镜像，COM22 波特率选 1000000。发送板 COM20 只输出状态，不能用作无线图像接收口。

## 改动

- 摄像头内部生成 JPEG，STM32 不重复压缩；电脑直接发布收到的 JPEG。
- VGA 及以下将原有 614400 字节池划成两个 307200 字节缓冲，DCMI/GPDMA 采下一帧时 SPI 发送上一帧。更大分辨率继续使用完整池和单缓冲。
- 显式打开 JPEG 的 DCMI FRAME 中断，启动前清理旧结束标记，避免截断帧误用旧 EOI。
- PB13/PB14/PB15 实际使用 **SPI2 AF5**，通过 IOSwap 保持 PB14 MOSI、PB15 MISO；模式 1，默认 **2.5 MHz**。SPI 为硬件外设阻塞收发，未使用 SPI DMA。
- CC1310 跨 256 字节 SPI 块组包，尽量装满 224 字节无线负载，消除每块「224＋32」的碎包；不缓存整张图片。
- 接收端用 8192 字节环形队列和 UART TX 中断后台输出，完整记录入队后确认。队列不足时不推进接收状态、不回 ACK，由发送端重试；重复包只重复 ACK。
- 接收端 ACK 前保护时间从 2 ms 减为 300 us；发送端 ACK 后保留 200 us 等待对端重返接收；SPI READY 低脉冲从 1 ms 减为 100 us。
- 两端采用 433.92 MHz、500 kbps、2-GFSK、175 kHz 频偏、1243 kHz 接收带宽、硬件 CRC；保留已验证的 433 MHz 合成器/频段覆盖表。
- COM25 从 115200 提升为 921600。COM22 大图镜像在 2 Mbps 下实测有缺字节，改为 1 Mbps 后高画质与流畅档均通过原始字节比对；无线关闭镜像时不受此带宽影响。
- 串口按已到达字节读取，消除强等 4096 字节的额外显示延迟；状态日志限频，网页保留白底黑字日志，增加预设和板端耗时显示。
- ARM 图传构建采用 -O2。

## 运行控制

通过 COM22 发到 STM32，网页设置接口 /set 也提供对应参数。

| 串口命令 | 网页参数 | 含义 |
|---|---|---|
| R4 | res=4 | VGA 640×480 |
| M0 | mode=jpeg | 传感器硬件 JPEG |
| Q20 | qs=20 | JPEG 量化参数（8～48） |
| F0 | fps=0 | 不限帧率 |
| P1 / P0 | pipeline=1/0 | JPEG 并行 / 顺序采集 |
| V0 / V1 | mirror=0/1 | 关闭 / 开启 COM22 图像镜像 |
| S0 / S1 / S2 | spi=0/1/2 | SPI 625 kHz / 1.25 MHz / 2.5 MHz |
| T1 / T0 | profile=1/0 | 开启 / 关闭阶段耗时日志 |

性能日志每 16 帧报告 capture_ms、send_ms、缓冲方式、镜像开关及 SPI 档位。采集时间与发送时间在并行模式下重叠，不能相加解释为总帧周期。

## 构建和烧录

在 sofeware_V2 下：

    .\build.ps1 APP=stream TRANSPORT=dual BUILD_DIR=build_perf_uart1m all

在 cc1310 下：

    .\build.ps1 APP=rf_image_tx BAND=433 BUILD_DIR=build_perf_tx_final all
    .\build.ps1 APP=rf_image_rx BAND=433 BUILD_DIR=build_perf_rx_final all

图像应用默认 RF_RATE=500，接收 UART=921600，ACK guard=300 us，-O2。发送端与接收端必须同一 RF_RATE。旧 50 kbps 配置可显式选择 RF_RATE=50 RF_ACK_GUARD_US=2000；UART 可通过 RF_UART_BAUD 覆盖。未通过验证的 100 kbps 候选已从运行配置中移除，200 kbps 未实板测试。

两块 XDS110 序列号相同，不能只靠串口或探针序列号决定烧录对象。rf_validation/flash_image_board.py 在实际 JTAG 连接中核对工厂地址后才写 Flash：

- TX：00124B00376111F7
- RX：00124B00219D1542

    python rf_validation/flash_image_board.py tx cc1310/build_perf_tx_final/cc1310_rf_image_tx.elf --log rf_validation/performance/flash_tx_final.log
    python rf_validation/flash_image_board.py rx cc1310/build_perf_rx_final/cc1310_rf_image_rx.elf --log rf_validation/performance/flash_rx_final.log

当首个探针连到另一块芯片时，脚本临时令首个探针进入 DFU，以暴露第二个探针。完成后重写原有同一序列号并退出 DFU，不下载探针固件、不改变序列号。全部操作记录在日志里。

## 实测和边界

详细表格、原始 JSON 和测试方法见 [RF_PERFORMANCE.md](RF_PERFORMANCE.md)。旧版不限速 VGA 约 0.12 FPS；优化后同参数 12 约 1.67 FPS，参数 20 / 28 / 36 的短测约 2.03 / 2.29 / 2.57 FPS。最终参数 28 连续测试 600 秒，收到 1436 张完整图像，平均 2.39 FPS；校验错误、不完整帧和序号丢帧均为 0。画面细节和光照影响 JPEG 大小，以上为当时桌面场景的实测值，并非任意场景的保证。

VGA 双路原始 UART 抓取在 COM22 1 Mbps 下通过：参数 12 的 45 张无线图像全部与同期相机直出 SHA-256 一致；参数 28 的 54 张对应图像一致，采样边界另有 1 张无线帧先于本次直出抓取。两路校验错误和无线序号缺口均为 0。

原功能联调记录在 rf_validation/receiver_join/initial_validation.json 等文件，早期 0.92～1 FPS 启用了 F1 限制，不能用作链路最大性能。本轮改动文件的优化前源码备份和三份已知可工作的 ELF 已保存在 rf_validation/performance/baseline_source 和同级目录。

500 kbps 配置参考 TI SmartRF 导出和 TI 工程师在 433 MHz 的测试方案；本轮为桌面近距离，尚未完成远距离、遮挡及干扰环境的性能表征。

- [TI 433 MHz 高速率讨论](https://e2e.ti.com/support/wireless-connectivity/sub-1-ghz-group/sub-1-ghz/f/sub-1-ghz-forum/1177612/cc1310-how-to-change-frequency-and-other-settings)
- [TI 500 kBaud 完整配置来源](https://e2e.ti.com/support/wireless-connectivity/sub-1-ghz-group/sub-1-ghz/f/sub-1-ghz-forum/801214/cc1310-different-power-emission-with-different-settings---all-don-t-make-sense)

UART 协议仍为 RFI1 + uint16 包长 + DATA 包 + sum32；无线协议仍为 20 字节头、最多 224 字节数据，保留会话号、帧号、偏移、确认、重试和去重。网页只发布长度和校验通过的完整 OV3S 帧。

