# VGA 无线图传优化与验证

## 实测结果

全部为真实摄像头 → STM32 → SPI → CC1310 → 433.92 MHz → 接收板 → COM25 → 网页的完整帧统计，取消 F1 限速。FPS = 测试窗口内完整且校验通过的帧数 / 实际秒数。

| 方法 | JPEG 量化参数 | FPS | 平均 JPEG（十进制 KB） | 完整帧 | 秒 | 校验错 / 不完整 / 序号丢帧 | 原始数据 |
|---|---:|---:|---:|---:|---:|---|---|
| 旧版 50 kbps | 12 | 0.12 | 15.3 | 3 | 25 | 0 / 0 / 0 | [JSON](rf_validation/performance/baseline_50k.json) |
| 满包＋后台 UART，50 kbps | 12 | 0.24 | 14.9 | 6 | 25 | 0 / 0 / 0 | [JSON](rf_validation/performance/packed_async_50k.json) |
| 500 kbps＋JPEG 并行，SPI 625 kHz | 12 | 0.96 | 16.6 | 24 | 25 | 0 / 0 / 0 | [JSON](rf_validation/performance/phy500_pipeline_spi625.json) |
| 500 kbps＋快速握手，SPI 2.5 MHz，镜像开 | 12 | 1.43 | 18.5 | 43 | 30 | 0 / 0 / 0 | [JSON](rf_validation/performance/phy500_fast_spi2500.json) |
| 同上，关闭镜像 | 12 | 1.67 | 18.5 | 50 | 30 | 0 / 0 / 0 | [JSON](rf_validation/performance/phy500_fast_no_mirror.json) |
| VGA 平衡画质 | 20 | 2.03 | 15.1 | 61 | 30 | 0 / 0 / 0 | [JSON](rf_validation/performance/vga_qs20.json) |
| VGA 流畅画质 | 28 | 2.29 | 13.3 | 69 | 30 | 0 / 0 / 0 | [JSON](rf_validation/performance/vga_qs28.json) |
| VGA 更强压缩 | 36 | 2.57 | 12.0 | 77 | 30 | 0 / 0 / 0 | [JSON](rf_validation/performance/vga_qs36.json) |
| 关闭 JPEG 并行，其他保持流畅档 | 28 | 1.96 | 12.9 | 49 | 25 | 0 / 0 / 0 | [JSON](rf_validation/performance/vga_q28_sequential.json) |
| 最终配置连续 10 分钟 | 28 | 2.39 | 12.9 | 1436 | 600 | 0 / 0 / 0 | [JSON](rf_validation/performance/stability_vga_q28_10min.json) |

不同方法依次在同一桌面设备上测试，画面和自动曝光并未完全固定，因此 JPEG 大小有变化；这些是运行结果，不能把全部差异归因于某一个参数。高画质参数 12 始终保留，没有通过默认降低分辨率来获得 VGA 帧率。

## 最终参数

- 图像分辨率 640×480；JPEG 在 OV3660 内部生成，网页直接显示原 JPEG。
- 无线 433.92 MHz、500 kbps、2-GFSK、175 kHz 频偏、1243 kHz 接收带宽、硬件 CRC。
- SPI2 AF5，模式 1，2.5 MHz；硬件 SPI，当前 CPU 阻塞收发。
- 无线数据包尽量装满 224 字节；逐包确认与重试，重复数据不会重复输出。
- RX UART TX 中断＋8192 字节队列，完整数据入队才 ACK；队列不足则等待重传。
- COM25=921600；COM20 状态口=115200；COM22 控制/镜像=1000000。
- 两个 307200 字节 JPEG 缓冲用于 VGA 及以下，并行采集/发送；更大分辨率单缓冲。
- 无线来源自动关闭完整图像镜像，保留相机控制和耗时日志；切回 STM32 来源自动恢复镜像。
- 网页提供量化参数 12 / 20 / 28 三档 VGA 预设，参数越大压缩越强、画质越低。

## 图像字节一致性

先用专门的读取线程保存两路 UART 原始字节，再离线重组、校验、解码 JPEG，并以帧序号＋SHA-256 匹配。

- [高画质参数 12](rf_validation/receiver_join/uart1m_pair_q12_validation.json)：35 秒，45 张无线完整帧全部与同期相机原图一致；两路校验错 0，无线序号缺口 0。
- [流畅参数 28](rf_validation/receiver_join/uart1m_pair_q28_validation.json)：30 秒，54 张对应帧一致，两路校验错 0，无线序号缺口 0。另有一张无线帧来自抓取窗口开始前的相机输出，按窗口边界处理，不计为字节匹配。
- 所有本次匹配图像均使用 PIL 实际解码；原始 UART 字节和实际 JPEG 与 JSON 同目录。

## 发现并处理的问题

- 旧版使用 50 kbps；每个 256 字节 SPI 块产生 224＋32 两个无线包，且接收端等 115200 UART 完整发送才 ACK。先优化这些软件等待，VGA 从约 0.12 到 0.24 FPS。
- 100 kbps 的宽带候选没有收到有效 ACK，已从运行配置移除；200 kbps 未实板测试，未声称通过。
- 500 kbps 初次测试需要进一步优化收发保护时序；最终 ACK 前 300 us、ACK 后 200 us、READY 低脉冲 100 us。
- COM22 原有 2 Mbps 在连续 VGA 镜像中实测缺字节，即使扩大 Windows 缓冲并离线解析仍会出现。1 Mbps 在大小两档 JPEG 的双路原始抓取中均通过。无线来源自动关闭镜像，不消耗完整图像串口时间。
- 串口读取不再强等 4096 字节，降低显示延迟；日志限频且保留白底黑字。

## 可复现文件

- `rf_validation/benchmark_http.py`：通过真实网页工作线程计数，不把页面 1 秒统计刷新当成帧率上限。
- `rf_validation/validate_raw_pair.py`：双路原始串口捕获、离线校验、JPEG 解码、逐帧哈希比对。
- `rf_validation/test_protocol.c`、`test_receiver.py`：真实 C 编码、ACK 丢失/去重、缺包、重启、损坏 UART、随机分段及超时恢复测试。
- `rf_validation/performance/flash_*final*.log`：最终烧录与校验；STM32 1 Mbps 固件的烧录见 `flash_stm32_uart1m.log`。
- `rf_validation/performance/baseline_source/` 与同目录三个旧 ELF：优化前源码和回退固件。
- 当前运行服务器 PID 记录在 `rf_validation/server.pid`，服务仅监听本机 127.0.0.1:8080。

## 实际限制

以上为桌面近距离，画面细节、光照、曝光、无线距离及干扰会改变帧率。500 kbps 尚未完成远距离和干扰条件表征。不能承诺任意 VGA 画面都达到 2 FPS；需要画质与流畅度平衡时优先选择网页参数 20 或 28。

参数与构建说明见 [RF_IMAGE_LINK.md](RF_IMAGE_LINK.md)。
