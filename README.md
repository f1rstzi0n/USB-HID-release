# USB-HID-release

私有仓库 [f1rstzi0n/USB-HID-release](https://github.com/f1rstzi0n/USB-HID-release) 保存 STM32F446VET6 APP：FreeRTOS、原生 HID 参数通信与只读 MSC 光驱。仓库名与本地工程名分开；本地目录和 CubeIDE 工程继续叫 `Test446`。

APP 与 [USB-Bootloader](https://github.com/f1rstzi0n/USB-Bootloader)（本地/CubeIDE 名 `Test446-Boot`）各自独立构建。Boot 提供裸机 HID、签名镜像验证、升级提交及启动；APP 保留 Fake 参数语义，真实业务从 APP adapter 接入。

源码来源是已归档的 [USB-HID：usb-hid-archive-20261012](https://github.com/f1rstzi0n/USB-HID/tree/usb-hid-archive-20261012)。后续固件开发在这两个目标仓库进行。

## 阅读与构建入口

- [AGENTS.md](AGENTS.md)：APP agent 指南唯一真源。
- [PORTING.md](docs/PORTING.md)：工程结构、业务接入、板级契约及构建/安装说明。
- [移植验证记录](docs/validation/USB-PORTING-20261012.md)：构建与回归证据、内存核算、实板待验清单。
- [协议规范](docs/reference/PROTOCOL.md)、[参数维护](docs/reference/CATALOG.md)、[目录设计](docs/reference/USB-CATALOG-DESIGN.md)、[Boot 设计](docs/reference/BOOTLOADER_DESIGN.md)：本地协议与设计参考。
- [CubeIDE 构建工具](tools/cubeide/build.ps1)、[生成恢复工具](tools/cubeide/apply_sources.ps1)、[布局检查](tools/cubeide/verify_builds.js)：使用本工程 Debug/Release managed build。完整检查顺序见 AGENTS.md。

需要 STM32CubeMX、STM32CubeIDE、PowerShell、Node.js，以及用于 host 回归的 native GCC/CMake/Ninja；目录生成需 .NET。CubeIDE 打开本地 `Test446` 工程，命令行工具使用同一构建定义。

`usb-porting-baseline-20261012` 是软件基线：APP/Boot 四配置构建及 APP 18、Boot 2 项 host 回归通过。目标板尚未烧录或验收，USB 时序、heap/栈水位、IWDG 和升级/掉电验证仍待执行；此标签不代表实板发布。

来源归档提交：[3e3431e8dc19ff9b2ef52c7b1172894fe702392d](https://github.com/f1rstzi0n/USB-HID/commit/3e3431e8dc19ff9b2ef52c7b1172894fe702392d)。
