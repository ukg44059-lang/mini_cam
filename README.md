# mini_cam

OV3660 + STM32U575 + 双 CC1310 的 433 MHz 无线图传工程。本仓库用于保存明确的版本，代码改坏时能按功能、验证状态、更新记录和Git标签恢复。

## 版本选择

|目录|功能|验证状态|回溯标签|
|---|---|---|---|
|[V1.0](V1.0/README.md)|连续拍照→STM32→SPI→CC1310无线发送→另一个CC1310接收→电脑网页显示|基本链路已有真实硬件验证；本次归档重新构建固件尚未重刷验证|`v1.0`|
|[V1.1(test)](V1.1%28test%29/README.md)|在基本流程上增加8项状态/功耗实验、网页按钮、采样积分与结果导出|测试版三端固件已编译、离线接口/协议检查通过；8项实际硬件实验尚未验证|`v1.1-test`|

V1.0导入自本地sofeware_V2；此后Git版本号以本仓库为准，不按旧文件夹名称推断。

## 最近更新

|日期（America/Denver）|更新|内容|验证与已知问题|
|---|---|---|---|
|2026-11-01|V1.1(test) 首次归档|保存8项测试固件和UI、阶段日志、仪表CSV积分、5次上电记录、JSON导出及原图传固件识别|离线检查和编译通过；未烧录或完成8项实板测试；保留TX状态日志缺失问题|
|2026-11-01|V1.0 首次归档|保存完整STM32、CC1310收发端、上位机、厂商驱动、构建参数、三端固件和SHA-256清单；增加恢复说明|60秒116张480×360图像全部与STM32直出一致；校验/不完整/序号丢帧0；TX状态口暂无日志|

完整记录见 [CHANGELOG.md](CHANGELOG.md)。Git标签直接指向相应提交；首次导入之前的本地修改过程没有Git记录，无法凭空还原。

## 快速使用

```powershell
git clone https://github.com/ukg44059-lang/mini_cam.git
cd mini_cam
python -m pip install -r V1.0/requirements.txt
./Build-Version.ps1 -Version V1.0
./V1.0/启动图传上位机.ps1
```

基本流程默认 http://localhost:8080/ 。串口现为STM32 COM30、TX状态COM34、RX图像COM20，换电脑应重新核对。构建需要GCC/make；详细工具链、硬件引脚、烧录和参数在各版本README中。构建和启动上位机均不会自动给板子烧录。

测试版入口：

```powershell
./Build-Version.ps1 -Version 'V1.1(test)'
& './V1.1(test)/启动测试上位机.ps1'
```

测试页面默认 http://localhost:8081/ 。两个版本共用实际串口，同一时间只启动一个上位机。当前正在运行原图传固件时，不能靠打开测试页面获得测试功能，还需要烧录测试固件并逐项核对状态。

## 代码改坏后如何恢复

新目录检出已验证基本版本，不影响手头正在修改的文件：

```powershell
git clone --branch v1.0 https://github.com/ukg44059-lang/mini_cam.git mini_cam_restore_v1.0
cd mini_cam_restore_v1.0
python tools/verify_snapshot.py V1.0
```

要继续修改，执行 `git switch -c fix/from-v1.0`。恢复代码之后，还需要恢复同版本三端固件和匹配的硬件接线；firmware/build_provenance.json记录构建参数及产物哈希。[完整恢复步骤与后续提交约定](docs/恢复版本.md)。

已发布标签保持原样，后续修复建立新提交/新标签。README负责说明，Git提交负责保存内容，SHA-256清单负责核对文件；三者一起使用。建议每次有意义的更新写日期、内容、验证结果和已知问题，并及时推送GitHub。

回溯选择：[v1.0 对应提交](https://github.com/ukg44059-lang/mini_cam/tree/v1.0)；[v1.1-test 对应提交](https://github.com/ukg44059-lang/mini_cam/tree/v1.1-test)；[全部提交记录](https://github.com/ukg44059-lang/mini_cam/commits/main/)。

厂商驱动和OpenOCD保留其原有许可声明。仓库未给所有第三方内容另行授予统一许可。
