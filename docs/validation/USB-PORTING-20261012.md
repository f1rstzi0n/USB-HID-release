# USB 双工程移植验证记录（2026-10-12）

软件检查已通过；未烧录实板，未测 USB 时序、heap 或任务水位。保留 Fake 行为：参数只写 RAM，OWO 校验应答但不执行动作。RS485 未完成函数及调用保留原文并注释。

## 完整构建与固定布局

APP、Boot 使用各自 CubeIDE 工程执行 Debug / Release clean build，四配置均为 0 errors、0 warnings。Boot 两配置均为 -Os，bin 为 42,196 B，小于 65,536 B。

本工程完整日志：[build-All-20261012-142049.log](build-All-20261012-142049.log)；同名 stderr 文件为空。

实际 ELF 布局和符号：[build-layout.json](build-layout.json)、[Debug-symbols.txt](Debug-symbols.txt)、[Release-symbols.txt](Release-symbols.txt)。ELF / bin / map 在各自 Debug、Release 目录。

Boot 向量 0x08000000；元数据 0x08010000 共 512 B；APP 向量 0x08010200，最大镜像 0x6FE00 B。初始 MSP 均为 0x2001FFF8；该地址的 .boot_request 为 8 B、ELF SHT_NOBITS / 链接 NOLOAD。

实际链接结果确认：Boot 无 RTOS、MSC、W25Q64、目录/业务依赖；APP 无 fw_update 擦写状态机、HAL_FLASH_Program、HAL_FLASHEx_Erase。两工程均只有一个 OTG_FS_IRQHandler，未链接 HAL PCD 初始化或 IRQ 接管。

## RAM 核算

单位为字节，取实际 ELF/map。可用 SRAM 为 131,064 B，顶部 8 B mailbox 另留。地址余量不能代替 FreeRTOS heap 或任务水位测量。

| 工程 | 配置 | Flash bin | .data | .bss | 静态 RAM | 链接保留 | 地址余量 |
|---|---|---:|---:|---:|---:|---:|---:|
| Test446 | Debug | 257588 | 3560 | 108500 | 112060 | 3076 | 15928 |
| Test446 | Release | 158704 | 3008 | 107736 | 110744 | 3072 | 17248 |
| Test446-Boot | Debug | 42196 | 140 | 32892 | 33032 | 3076 | 94956 |
| Test446-Boot | Release | 42196 | 140 | 32892 | 33032 | 3076 | 94956 |

APP .bss 已含 FreeRTOS ucHeap 20,480 B；TASK_USB 3,072 B、TASK_USB_IO 2,048 B 及其他动态任务栈从此 heap 分配，不能重复计入。RX 静态队列 16 × 76 B = 1,216 B（64 B 数据加 epoch、ticket、时间戳）；复用两个 4 KiB 协议缓冲及 70 × 64 B TX 缓冲；密码 arena 静态 16 KiB。

启动检查全部任务创建结果；媒体、SPI2 和 TinyUSB 交接对象静态分配。实际 heap 最低值和栈水位尚未测量。

## 主机回归

APP 18/18 通过。见 [host-tests.log](host-tests.log)、[host-build.log](host-build.log)。两工程合计 20 项，直接编译各自目标 App / Drivers 源码。

| 范围 | 软件结果 |
|---|---|
| CRC16、帧、分片、目录与服务、紧凑参数初值 | 通过 |
| 实际 MSC 回调、只读光盘、元数据、单槽覆盖及掉电恢复 | 通过 |
| 实际 W25Q64 / SPI2：锁争用、异步擦除、超时、I/O 故障、维护预留 | 通过 |
| ACAT 签名预检、真实目录解析、生成数据一致性及提交恢复 | 通过 |
| APP 只读镜像：有效签名、篡改签名/载荷、向量、提交标志、FWINFO | 通过 |
| Boot 实际升级：签名、范围、擦写/回读、最后提交、掉电及 ACK 授权 | 通过 |
| RX 复制、16 槽溢出后等新 SOF、最大 4 KiB 帧 | 通过 |
| 忙期请求丢弃、最终 IN 完成后立即请求、新窗口 ticket | 通过 |
| 超时/迟到完成不授权重枚举、复位/重连隔离、旧完成不授权动作 | 通过 |
| IO 命令遇复位自重投、实际断连计时、未挂载仍推进后台 | 通过 |
| 媒体锁忙返回、容量/事件快照、SPI2 交错 | 主机边界通过；板上负载待测 |

Native stub 只替换 HAL、RTOS、TinyUSB 外设边界；USB 总线时序、真实调度优先级和 SPI 波形仍需实测。签名 fixture 不作为生产公钥或发布产物。

## 配置、复用与原文件保护

- HSE bypass 12 MHz、HSE PLL 源、CPU 180 MHz；USB 48 MHz PLLSAIP 初始化由 Drivers/USB/board.c 执行。
- APP 使用 FreeRTOS OSAL；Boot OSAL NONE。HAL 毫秒时基 TIM7；旧 FreeRTOS 的 pdTICKS_TO_MS 兼容定义已补。
- PE4 输入无上下拉高阻；Flash/Motor CS 访问前取消选中。USB ISR 不取媒体/SPI2 锁、不访问 Flash；SPI2 拒绝 ISR 调用。
- 锁顺序媒体 → SPI2；TinyUSB 每轮最多 8 个事件、worker 最多 16 个 RX；双任务 osDelay(1)，断线时仍推进后台。
- IWDG P32 / R4095 保留；正常调度、256 B 摘要及 64 操作 P-256 分片、Boot 逐扇区擦除前后和逐字编程有喂狗点。
- 生产 C 块首变量检查 APP 27 / Boot 13 文件通过。第三方未批量改写。
- 原全局存储迁入已有 App/global.c，原名/类型/初值保留；头文件 extern。
- 调试 launch 仅加载符号；源码、include、VTOR、链接、IOC 与源清单各自独立。

[integrity.json](integrity.json)：原工程 106 个迁移前基线文件 SHA-256 未变；265 个共享文件两份一致；242 个第三方文件与原版逐字相同；3 个派生文件与 YAML 重新生成结果相同。生产公钥与原版、头文件、public-key.bin 一致。

版本清单：[shared-source-manifest.json](../shared-source-manifest.json)。运行 node tools/cubeide/verify_shared.js；可显式传入另一工程目录做比较，不是编译依赖。共享代码变更需同时同步、回归、审核并更新清单。第三方许可保留在 Drivers 下。

目标说明使用 v4.3（wire major 4）、BVC4 布局 4、单槽更新，修正旧协议版本/双槽表述；源工程说明保持原状。

## 实板验收（全部待执行）

| 场景 | 所需证据与通过条件 | 状态 |
|---|---|---|
| 12 MHz 新板时钟 / USB / IWDG | CLK48/SOF、TIM7 精度；验证/擦除最长耗时和喂狗余量 | 待测 |
| APP 枚举与 Ready | HID + 只读 CD-ROM；UID/PID 一致，签名目录后 Ready | 待测 |
| 最大帧及压力 | 最大 GET/SET、TX 阻塞、RX 溢出；忙期写不延后执行 | 待测 |
| 并发读盘 / 更新 | MSC、目录/光盘更新、SPI2 Motor 仲裁，无死锁或错误快照 | 待测 |
| 重连与进入 Boot | 旧事件失效，等最终实际 IN，实际断连 >=250 ms | 待测 |
| 签名升级 | 固定 UID、Boot PID、有效包升级、篡改拒绝、身份及目录 Ready | 待测 |
| 中断恢复 | 分阶段断电；无效/未提交镜像留 Boot，单槽可重新灌装 | 待测 |
| 内存水位 | g_usb_min_heap >=1,024 B；两 USB 栈剩余各 >=256 B | 待测 |

用本次 ELF/map 定位 g_usb_min_heap、g_usb_worker_stack_free、g_usb_io_stack_free，并记录 g_usb_rx_high_water / g_usb_rx_overflows。未用源工程 25 MHz 板结果替代本轮验收；未生成或安装签名工厂镜像。

## 复现与交付

在各自工程根目录：

```powershell
powershell -ExecutionPolicy Bypass -File tools/cubeide/verify_generated.ps1
powershell -ExecutionPolicy Bypass -File tools/cubeide/build.ps1 -Config All
node tools/cubeide/verify_builds.js
node tools/cubeide/verify_shared.js
powershell -ExecutionPolicy Bypass -File tools/cubeide/verify_owned_c.ps1
cmake -S tests/host -B build-host -G Ninja -DCMAKE_C_COMPILER=gcc
cmake --build build-host
ctest --test-dir build-host --output-on-failure
```

本机 native GCC/CMake/Ninja 来自 StrawberryPerl；显式选择 native gcc，避免其他嵌入式编译器被自动选中。APP 目录生成回归需已安装 .NET/PowerShell，输出在 build-host。可设置 ARM_GCC / ARM_TOOLS / STM32CUBEIDE_EXE 调整工具位置。

CubeMX 再生成后执行 tools/cubeide/apply_sources.ps1，配置核验，再四配置完整构建；自动检查不能替代查看生成 GPIO/ISR。

移植说明：[PORTING.md](../PORTING.md)。研究依据：[USB-PORTING-RESEARCH.md](../USB-PORTING-RESEARCH.md)。目标修改前备份：C:/Users/17907/AppData/Local/Temp/usb-hid-porting-20261012 下 Test446-before、Test446-Boot-before。临时架构图：用户 Temp 下 architecture-review-20261012-usb-porting.html。
