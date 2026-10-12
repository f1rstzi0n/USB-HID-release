# 私有仓库软件基线验证（2026-10-12）

本记录验证从 Git 恢复的独立源码工程；未烧录实板。软件基线标签为 usb-porting-baseline-20261012。

## 来源与提交范围

- 原 USB-HID 归档提交 [3e3431e8dc19ff9b2ef52c7b1172894fe702392d](https://github.com/f1rstzi0n/USB-HID/commit/3e3431e8dc19ff9b2ef52c7b1172894fe702392d)，annotated tag usb-hid-archive-20261012；归档只发布 codex/fake-andromeda-device，main 保持 483bfff47042ba5bf7e14a8ba0b24d4ca1b93fe6。
- 本工程独立克隆的受测初始提交为 7ec52e8bcb22b6a62040b6ce6c84b942958446ac；本记录之后提交的差异仅为文档、忽略规则和验证证据，固件源码、构建定义、公钥与共享文件不变。
- 保留本地文件夹 / CubeIDE 名 Test446；新 GitHub private 仓库名为 USB-HID-release，分支 main。未复制原仓库 Git 历史，也不使用原工程或伙伴工程源码/链接产物。
- README / AGENTS / 本地协议及设计参考齐全，链接已改为可随克隆使用的相对路径或固定归档标签；两工程 AGENTS 分别是真源，源 AGENTS 继续指向 CLAUDE。
- 265 个共享文件及生产公钥经原始 SHA-256 校验；.gitattributes 使用 * -text，避免 core.autocrlf 改变共享、第三方及公钥字节。
- 许可证、生产公钥与测试 fixture 入库；构建目录、缓存、备份 ZIP/IOC、scratch 全文副本及私钥排除。源 TinyUSB 保持 submodule 6d46dea28419b100cc7a6d928cce2cbb587bec63。
- 发布阶段复查源基线 105 个生产/配置文件字节未变；唯一另行更新的基线文件 .gitignore 属归档元数据。迁移记录中的 106 文件结论保留为迁移完成时证据。

## 独立克隆结果

采用 git clone --no-hardlinks 从已提交仓库建立新目录，无既有构建输出；使用克隆自身 .cproject clean build Debug / Release。

- 配置核验通过：12MHz HSE bypass、180MHz、板级 USB 48MHz、PE4 高阻、CS、TIM7、无 PCD 与唯一 USB ISR、固定链接地址及 8B NOLOAD mailbox。
- 自研 C 块首变量检查通过（APP 27 / Boot 13）；265 文件 hash 与跨工程副本检查通过。
- Test446 Debug / Release 均 0 errors、0 warnings；实际 ELF 角色边界与地址核验通过。
- 18/18 native 回归通过，直接编译克隆内 App / Drivers 实际源码；APP 范围含协议、目录、MSC、签名目录、Flash/SPI2、交接/窗口/迟到完成及只读镜像；Boot 范围含签名升级、提交恢复及实际 ACK 授权。
- Boot 两配置 42,196B，小于 65,536B，均使用 -Os；APP 无 Boot 擦写实现，Boot 无 RTOS/MSC/W25Q64/目录业务符号。

| 工程 | 配置 | Flash B | 静态 RAM B | 链接保留 B | RAM 地址余量 B |
|---|---|---:|---:|---:|---:|
| APP | Debug | 257588 | 112060 | 3076 | 15928 |
| APP | Release | 158704 | 110744 | 3072 | 17248 |
| Boot | Debug | 42196 | 33032 | 3076 | 94956 |
| Boot | Release | 42196 | 33032 | 3076 | 94956 |

## 复现与日志

使用 PowerShell 7.6.6 / pwsh（不是 Windows PowerShell 5.1）、Node.js 26.7.0、STM32CubeIDE 所配 ARM 工具链及 native GCC 13.2.0 / CMake / Ninja；APP 目录生成另需 .NET。

Windows PowerShell 5.1 的 ANSI 读取导致部分 UTF-8 无 BOM 脚本 ParserError；目标指南和移植命令已统一改用 PowerShell 7，未转换任何共享脚本或第三方字节。

- [配置与完整检查](publication-checks.log)
- [两配置 clean build](publication-build.log) / [stderr](publication-build.stderr.log)
- [克隆 ELF 布局](publication-build-layout.json)
- [Native 配置及构建](publication-host-build.log)
- [Native 回归](publication-host-tests.log)
- [完整复现步骤及硬件验收](USB-PORTING-20261012.md)

## 待验边界

标签是软件基线，SW110048.1.4.9、生产公钥、包格式、PID/UID 与地址契约不变。未接真实业务、未生成或安装签名工厂镜像。源 25MHz / 1.3.F 历史板证据不能代替本轮 12MHz 目标板。

目标 USB 时序、真实 MSC/更新并发、SPI2 波形、重连与签名升级、物理断电矩阵、IWDG 最坏余量仍待实板；压力后 heap 至少余 1KiB、两个 USB 栈各至少余 256B。RAM 地址余量不是 heap/栈实测水位。
