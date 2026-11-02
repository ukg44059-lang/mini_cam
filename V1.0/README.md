# V1.0：基本连续无线图传

本版本保存可工作的基本流程：OV3660 连续拍照 → STM32U575 DCMI/GPDMA 接收硬件 JPEG → SPI2 交给发送端 CC1310 → 433.92 MHz 无线 → 接收端 CC1310 → USB-UART → 电脑网页显示。导入来源为本地 `sofeware_V2`；它的旧目录名不等同于这里的发布版本号。

## 功能和验证状态

- 连续采集和显示、硬件 JPEG、分辨率/量化参数/帧率控制、网页快照。
- STM32 与 CC1310 的 SPI 转发；无线分包、逐包 ACK、重传、去重和图像完整性校验。
- VGA 及以下 JPEG 双缓冲并行采集/发送，可关闭串口图像镜像减少开销。
- 近距离真实链路已验证：2026-11-01，480×360、QS12，60秒收到116张图像，校验错误、不完整帧、序号缺口均为0；116张与STM32直出逐帧 SHA-256 一致。见 [验证记录](validation/basic_link_2026-11-01.json)。
- 已知限制：COM34 可打开但没有发送端状态日志；功耗、远距离和干扰场景未在本次检查中验证。历史 VGA 性能数据见 RF_PERFORMANCE.md，不应当作所有场景的保证。

此版本不包含 V1.1(test) 的8项功耗实验控制。

## 源码位置

|位置|内容|
|---|---|
|Core/、Drivers/、Middlewares/|STM32 固件、摄像头适配、HAL/CMSIS、USB组件|
|cc1310/Core/、cc1310/Drivers/|收发端固件、射频协议及 TI 驱动|
|tools/stream_server.py|电脑网页上位机|
|rf_validation/|协议测试夹具和历史性能统计|
|firmware/|按本仓库源码重新构建的三端固件、参数和SHA-256|
|SOURCE_ORIGIN.json|导入前每个原始文件的来源和校验值|
|SNAPSHOT.json|本发布目录实际归档文件的SHA-256|

导入时未修改STM32/CC1310应用算法。只补充仓库说明、启动入口、固件和验证文件，并将构建包装脚本中的OpenOCD路径改为仓库相对路径，允许通过环境变量选择make。

## 构建、启动和恢复

在仓库根目录：

```powershell
python -m pip install -r V1.0/requirements.txt
./Build-Version.ps1 -Version V1.0
./V1.0/启动图传上位机.ps1
```

默认构建固定 `APP=stream TRANSPORT=dual FRAMESIZE=480X360`，与源 Makefile 中用于其它demo的默认值区别开；原 Makefile 保留供开发使用。默认波特率 STM32=1000000、RX=921600、TX状态=115200；无线433.92MHz/500kbps，SPI2模式1/2.5MHz。

当前这套接线串口为 COM30/COM34/COM20。启动脚本提供参数覆盖，换电脑后必须重新核对。接收CC1310固件默认 DIO5=UART TX、DIO3=RX，发送端状态口DIO1=TX、DIO2=RX；COM编号不能证明实际DIO接线。若接收机用DIO1/2接线，构建时加 `-ReceiverTxPin 1 -ReceiverRxPin 2`，并在实验记录注明。

工具链：Windows PowerShell、GNU Arm Embedded GCC 7.3.1（原机器由STM32CubeIDE 1.5.0提供）、GNU make、Git for Windows 的 usr/bin（sh/rm/cat）。make 可用 `$env:MINICAM_MAKE_BIN` 覆盖目录，编译器可用 `Build-Version.ps1 -GccPath 'D:/gcc/bin'` 覆盖。源码Makefile保留原机器的GCC回退路径；新机器需要配置工具链，仓库未打包编译器。OpenOCD随仓库归档于 tools/openocd，依赖其原许可文件和USB驱动。

本次重新构建的 `firmware/` 用于完整保存源代码对应产物，未重新烧录到板上，不声称与当前运行Flash逐字节比对过。恢复硬件须分别烧录STM32、TX、RX同版本固件，确认接线/频段/引脚相符；XDS110存在相同序列号，不能仅凭序列号识别烧录目标。历史 factory-ID 脚本不是通用一键烧录入口。

标签为 `v1.0`。完整回溯方法见 [恢复版本](../docs/恢复版本.md)。原项目操作细节保留在 README-v2.md / RF_IMAGE_LINK.md / SPI_IMAGE_LINK.md 中，其中旧COM编号、绝对目录和烧录身份仅供历史参考；本README和firmware构建参数优先。
