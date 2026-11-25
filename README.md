# BT GAMEPAD 蓝牙手柄

基于 STM32F103RCT6 和 RDA5876 的标准蓝牙 HID 游戏手柄，包含 Windows Qt 调试上位机。

## 主要功能

- 标准蓝牙 HID 游戏控制器，Windows 系统驱动直接识别
- 双摇杆四路 16 位轴、10 个按键，50 Hz 输入报告
- Link Key 保存、掉电自动回连和 HID 通道自动恢复
- 上位机显示按键与摇杆，支持 RGB 灯效和验收报告导出

## 仓库内容

- `BT GAMEPAD设备工程/`：STM32 Keil 固件工程与源码。
- `BT GAMEPAD设备上位机/`：Windows Qt 上位机可执行程序及运行库。
- `firmware/Project.hex`：可直接烧录的固件（源码中已提供时）。
- 根目录设计文档：项目原理、协议、流程、调试方法与测试结果。

## 构建与烧录

1. 使用 Keil µVision 打开 `BT GAMEPAD设备工程/Project.uvprojx`。
2. 执行 Build／Rebuild，生成固件。
3. 也可直接烧录 `firmware/Project.hex`（若仓库包含该文件）。
4. 主控为 STM32F103RCT6；具体引脚、供电和接线以设计文档为准。

## 上位机

进入 `BT GAMEPAD设备上位机/`，运行 `btgamepad.exe`。请保留整个目录，不要只复制 EXE，以免缺少 Qt 插件或运行库。

## 设计文档

- `BT GAMEPAD设备设计文档.pdf`

## 开源许可

项目自有代码与文档采用 [MIT License](LICENSE)。第三方组件遵循各自原始许可证，详见 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)。
