# mini_cam

OV3660 + STM32U575 + 双 CC1310 的 433 MHz 无线图传工程。本仓库用于保存明确的版本，代码改坏时能按功能、验证状态、更新记录和Git标签恢复。

## 版本选择

|目录|功能|验证状态|回溯标签|
|---|---|---|---|
|[V1.0](V1.0/README.md)|连续拍照→STM32→SPI→CC1310无线发送→另一个CC1310接收→电脑网页显示|基本链路已有真实硬件验证；本次归档重新构建固件尚未重刷验证|`v1.0`|
|[V1.1(test)](V1.1%28test%29/README.md)|四档整机电流对比＋ISP/JPEG独立开关：完整运行→停止无线图像→摄像头待机→MCU Sleep；全程无线控制，手动电表录入与导出|STM32烧录校验、11段模式检查及ISP/JPEG无线开关实板检查通过；电表功耗待测|`v1.1-test.4`（此前测试标签均保留）|

V1.0导入自本地sofeware_V2；此后Git版本号以本仓库为准，不按旧文件夹名称推断。

## 最近更新

|日期（America/Denver）|更新|内容|验证与已知问题|
|---|---|---|---|
|2026-11-03|V1.1(test) ISP/JPEG无线开关|新增ISP子模块与JPEG独立开关，JPEG关时采集不无线发送；proto=6板端确认|关/开各两次，恢复图传且CRC/不完整帧0；未测电流；标签v1.1-test.4|
|2026-11-02|V1.1(test) 四档电流测试|替换八项按钮；固定①②同采集节奏、③MCU运行/④Sleep，RF始终在线；手动输入电表V/mA、整机差值与JSON导出|构建、STM32烧录校验、11段实板检查、网页及离线检查通过；未实测电流；回溯标签v1.1-test.4|
|2026-11-02|V1.1(test) 双向无线控制|命令下发、STM32状态确认和TX日志全部经433 MHz；电脑仅接收端COM20；更新三端固件、页面与操作说明|独占COM30/34期间仍可无线启停；连续6秒11张、间歇6秒3张，校验错误/不完整帧0；详见无线验证记录|
|2026-11-02|V1.1(test) 测试准备更新|增加请求编号确认、状态查询、超时/停止失败提示、服务重启恢复记录，原图传页面增加测试入口；更新STM32/TX固件|部分功能响应检查通过，详见CHANGELOG；没有把命令确认当作功耗实测|
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
& './V1.1(test)/启动测试上位机.ps1' -ReceiverPort COM42
```

测试页面默认 http://localhost:8081/ 。最新测试版电脑只需连接收端CC1310，本机当前COM42、921600（辅助口COM41）；发送端独立供电，不依赖COM30或COM34数据线。两个版本共用实际串口，同一时间只启动一个上位机。当前STM32为proto=6，CC1310沿用v1.1-test.2双向无线固件；测试版需三端配套固件，换回原图传固件后不能仅靠打开测试页面获得测试功能。

## 代码改坏后如何恢复

新目录检出已验证基本版本，不影响手头正在修改的文件：

```powershell
git clone --branch v1.0 https://github.com/ukg44059-lang/mini_cam.git mini_cam_restore_v1.0
cd mini_cam_restore_v1.0
python tools/verify_snapshot.py V1.0
```

要继续修改，执行 `git switch -c fix/from-v1.0`。恢复代码之后，还需要恢复同版本三端固件和匹配的硬件接线；firmware/build_provenance.json记录构建参数及产物哈希。[完整恢复步骤与后续提交约定](docs/恢复版本.md)。

已发布标签保持原样，后续修复建立新提交/新标签。README负责说明，Git提交负责保存内容，SHA-256清单负责核对文件；三者一起使用。建议每次有意义的更新写日期、内容、验证结果和已知问题，并及时推送GitHub。

回溯选择：[v1.1-test.4 四档测试](https://github.com/ukg44059-lang/mini_cam/tree/v1.1-test.4)；[v1.1-test.2 八项无线测试](https://github.com/ukg44059-lang/mini_cam/tree/v1.1-test.2)；[v1.0 对应提交](https://github.com/ukg44059-lang/mini_cam/tree/v1.0)；[v1.1-test 对应提交](https://github.com/ukg44059-lang/mini_cam/tree/v1.1-test)；[全部提交记录](https://github.com/ukg44059-lang/mini_cam/commits/main/)。

厂商驱动和OpenOCD保留其原有许可声明。仓库未给所有第三方内容另行授予统一许可。
