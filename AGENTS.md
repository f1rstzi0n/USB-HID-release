# AGENTS.md

本文件是 APP 工程 agent 指南的唯一真源；后续指南只修改本文件。README 面向人类阅读，`docs/reference/` 保留协议与设计参考。

## 工作范围与阅读入口

- 本仓库为私有 [f1rstzi0n/USB-HID-release](https://github.com/f1rstzi0n/USB-HID-release)，本地目录与 CubeIDE 工程名保持 `Test446`。APP 使用 FreeRTOS，提供 HID 参数通信及只读 MSC 光驱。
- 后续代码开发仅在 `Test446` 与伙伴 [USB-Bootloader](https://github.com/f1rstzi0n/USB-Bootloader)（本地/CubeIDE 名 `Test446-Boot`）进行；两工程各自包含源码、驱动和 Debug/Release 构建定义，构建不引用源 USB-HID 或对方源码。USB-HID 归档保存历史。
- 开始修改、排查或审查 APP 时，先读 [移植说明](docs/PORTING.md) 与 [目标验证记录](docs/validation/USB-PORTING-20261012.md)，确认接缝、验证边界和待测项。
- 修改 HID 报表、帧、服务码、错误码或事务行为时，先读 [协议规范](docs/reference/PROTOCOL.md)。当前为文档 v4.3、wire major 4。
- 修改参数树、初始化或生成链路时，先读 [参数维护说明](docs/reference/CATALOG.md)；修改目录格式、运行契约、挂载、更新或缓存时，再读 [目录设计](docs/reference/USB-CATALOG-DESIGN.md) 及协议 §7.14/§8.2。
- 修改 MSC 更新、初始化或恢复时，先读协议 §7.11–8 和移植说明的单槽布局。修改 APP 地址、进入 Boot、镜像验证、固件包或公钥时，先读协议 §7.13 与 [Boot 设计](docs/reference/BOOTLOADER_DESIGN.md)，并检查伙伴工程对应契约。
- 参考文档中的旧路径、源板验收与 HostRoot 示例属于历史；目标路径和当前验收以移植说明/目标验证记录为准。历史 Andromeda 修改授权不延续，跨仓库上位机修改需有当前任务授权。

## APP 并发与业务边界

- `TASK_USB` 是 worker，独占分片重组、帧解析、服务分发、事务和目录/光盘后台状态；断线或未挂载时仍推进后台。`TASK_USB_IO` 独占 TinyUSB 的栈事件、OUT 投递、IN 提交与实际完成通知；worker 就绪后 IO 才初始化 USB 栈。其他任务与 ISR 不直接调用 TinyUSB。
- 保留 `epoch`（连接）、`ticket`（接收窗口）和非零 `token`（应答）的隔离与实时校验。完整 EOF 原子 claim 窗口，忙期请求直接丢弃，不排到稍后执行业务；reset/overflow 使旧半帧和完成事件失效，溢出后等待新 SOF。
- 普通应答最终 IN 实际完成后，IO 同步发布 DONE 并开放新 ticket；worker 先消费 DONE 再处理新窗口 RX。入队和提交成功不能授权后续动作；超时、迟到或旧代次 completion 不能授权复位、重枚举或维护动作。重连断开时长从 IO 实际断连起算，至少 250 ms。
- 锁顺序固定为 `media mutex → SPI2 mutex`。worker 发布一致的容量/介质事件快照；MSC 回调只尝试取媒体锁，锁忙返回 BUSY。SPI2 仲裁覆盖 W25Q64 与 Motor；维护 reserve 排除 Motor，但不跨会话占锁。ISR 不取媒体/SPI2 锁、不访问 Flash、不 reserve/release。
- MSC 是只读 CD-ROM，主机写入拒绝，内容更新只走 HID。BVC4 布局 4：光盘 4 MiB 单槽、签名目录 1 MiB 单槽。空白/旧布局/损坏保持空盘，不自动初始化；光盘 BEGIN 先提交无介质，目录两遍更新先零擦写预检，再提交无效记录并覆盖，最终验证后提交。中断需整包恢复，不回退旧内容。
- Fake 参数仅写 RAM，USB 拔插保留，MCU 复位恢复生成初值；OWO 只校验并应答，不执行电机、学习、EEPROM 保存或业务重启。MSC/签名目录后端保持真实 W25Q64。真实业务在 `App/usb/usb_app.c` adapter 接入，补齐受控值同步、命令完成语义、维护安全判定和 SPI2 仲裁；耗时动作交给业务任务。
- `catalog/SW110048.catalog.yaml` 是参数、运行契约与描述的唯一人工维护源；由 `tools/catalog/Generate-Catalog.ps1` 生成到 `App/generated/` 等派生位置，不直接编辑生成数据。`runtime.initial` 决定复位 RAM 初值，`description.default` 只描述目录默认值；外部目录只能收紧硬规则，不能定义执行动作。运行契约变化需配套固件与回归。
- APP 的 `App/firmware/fw_image.*` 只做镜像验证与查询，APP 不引入 Boot 的 `fw_update.c`、内部 Flash 擦写状态机或 HAL Flash 擦写入口。

## 源码与板级契约

- clean-room：使用自研协议，只参照竞品行为与已有证据，禁止复制竞品代码、报文格式或身份。USB 与 RS485 共享地址/类型及值编码，USB 目录与限制使用独立 context；RS485 报文、读写、保存、轮询及恢复保持既有边界。未完成 RS485 实现与调用保留原文并注释。
- 文档与注释用简体中文，代码标识符保留英文。自有 C 在块首声明变量，再赋值和使用；第三方代码及许可证保持原样。保留已有全局变量名、类型与初值，存储在 `App/global.c`，头文件使用 extern。
- 共享源码、公钥与固件契约在两工程保留同版本副本，以 [共享 hash 清单](docs/shared-source-manifest.json) 和 `tools/cubeide/verify_shared.js` 核验。共享变更同时同步两工程、审核并更新清单，分别回归；显式传入伙伴目录仅用于比较，不作为编译依赖。
- 保持 12 MHz HSE bypass、HSE PLL 源及 CPU 180 MHz；USB 48 MHz PLLSAIP 由 `Drivers/USB/board.c` 初始化。TinyUSB 独占 OTG_FS，保持唯一 IRQ，不生成 HAL PCD；HAL 毫秒时基为 TIM7，APP SysTick 留给 FreeRTOS。
- PE4 与 D+ 相连，保持无上下拉输入高阻；连接控制使用 TinyUSB。Flash/Motor CS 在访问前取消选中。保留 IWDG P32/R4095 与长验签路径喂狗；APP 只有 IO 最近 1000 ms 有推进时才刷新 IWDG。
- 固定 Boot 64 KiB（`0x08000000` 起），APP 元数据 512 B（`0x08010000` 起），APP 向量/正文与 VTOR 为 `0x08010200`。SRAM 顶部 `0x2001FFF8` 保留 8 B `NOLOAD` 一次性 boot-request，初始 MSP 为该地址。
- Debug launch 只加载符号。首装使用匹配公钥/契约的签名 `.factory.hex`，已有正确 Boot 的 SWD 维护使用 `.signed-app.hex`，USB 更新使用 `.afw`；裸 APP ELF/BIN/HEX 和测试 fixture 不代替签名安装产物。沿用既有工具与公钥，不虚构本工程发布命令。

## 构建、检查与完成条件

固件唯一构建定义为本工程 CubeIDE managed build（`.project`、`.cproject`、`.settings/`）；`Test446.ioc`、生成代码和链接脚本随源码入库。CubeMX 再生成后，先核对 GPIO/ISR，再从本工程根目录依次运行：

工程脚本使用 PowerShell 7（`pwsh`）；Windows PowerShell 5.1 无法解析部分 UTF-8 无 BOM 脚本。

```powershell
pwsh -NoProfile -ExecutionPolicy Bypass -File tools/cubeide/apply_sources.ps1
pwsh -NoProfile -ExecutionPolicy Bypass -File tools/cubeide/verify_generated.ps1
pwsh -NoProfile -ExecutionPolicy Bypass -File tools/cubeide/verify_owned_c.ps1
node tools/cubeide/verify_shared.js
pwsh -NoProfile -ExecutionPolicy Bypass -File tools/cubeide/build.ps1 -Config All
node tools/cubeide/verify_builds.js
cmake -S tests/host -B build-host -G Ninja -DCMAKE_C_COMPILER=gcc
cmake --build build-host
ctest --test-dir build-host --output-on-failure
```

`-Config All` 对本工程 Debug/Release 执行 clean build。检查失败先修复，不绕过检查烧录；固件/共享代码变更完成条件为相应生成、C 风格、hash、两配置构建/布局检查及 host 回归全部通过。共享契约变更在 `Test446-Boot` 重复其检查流程。工具可用 `STM32CUBEIDE_EXE`、`ARM_GCC`、`ARM_TOOLS` 指定位置；host 显式选 native GCC，目录生成还需 .NET/PowerShell 7。

2026-10-12 软件基线为两工程四配置 0 errors/0 warnings、APP 18/18、Boot 2/2；目标尚未烧录实板。软件通过不等于实板发布：12 MHz/USB 时序、SPI2 负载、升级/掉电、IWDG 余量仍待测；必须记录 `g_usb_min_heap ≥ 1024 B`，`g_usb_worker_stack_free` 与 `g_usb_io_stack_free` 各 `≥ 256 B`，并记录 RX 水位/溢出。源 USB-HID 的 25 MHz 板验收不能替代目标证据。
