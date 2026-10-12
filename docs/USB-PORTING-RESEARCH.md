# USB APP / Boot 移植一手来源核对

记录日期：2026-10-12。来源工程为 USB-HID，目标为 Test446 与 Test446-Boot。本文区分已核对的官方约束、已经落地的源码和待实板验证事项；本文不把源码检查等同于构建或实板验收。移植范围先保留 Fake 参数业务，再独立接入成熟工程的真实业务。

## 1. 工程边界与必须一致的契约

APP 是 HID + 只读 MSC 复合设备；Boot 是独立 PID 的单 HID，现有 Boot 构建不包含 MSC、W25Q64、SPI2 总线驱动、外部目录数据或业务服务。目标 Boot 已采用裸机 main 循环；仅初始化 HAL、目标时钟、GPIO 安全初态、IWDG 和 USB，不初始化业务 SPI。落地依据：[Boot main.c](https://github.com/f1rstzi0n/USB-Bootloader/blob/usb-porting-baseline-20261012/Core/Src/main.c) 的 main；源工程依据： [TinyUSB 配置](https://github.com/f1rstzi0n/USB-HID/blob/usb-hid-archive-20261012/firmware/tusb_config.h)、[描述符](https://github.com/f1rstzi0n/USB-HID/blob/usb-hid-archive-20261012/src/usb/usb_descriptors.c)、[Boot 构建排除](https://github.com/f1rstzi0n/USB-HID/blob/usb-hid-archive-20261012/.cproject)、[SPI2 禁用分支](https://github.com/f1rstzi0n/USB-HID/blob/usb-hid-archive-20261012/Core/Src/main.c)。

| 契约 | 固定值 / 规则 |
|---|---|
| Boot | 0x08000000 起，64 KiB，扇区 0–3 |
| APP 校验区 | 0x08010000 起，512 B |
| APP 正文及向量表 | 0x08010200 起，最多 458240 B |
| SRAM 复位请求 | 0x2001FFF8 起，8 B，NOLOAD；普通 RAM 和栈不得覆盖 |
| APP / Boot 身份 | 同一芯片 UID 序列号、不同 PID |
| AFW | 256 B 头 + 正文；头前 192 B 为签名域，P-256 签名为 64 B 大端 r||s |
| 目标 / 契约 / 升级块 | BV500-F446 / Boot contract 1 / 1024 B |
| 提交字 | 0x080101FC，0xA55AC33C，最后写入并回读 |
| 信任公钥 | APP、Boot 和配套主机使用同一公钥；全零公钥拒绝镜像 |

这些值来自 [fw_update.h](https://github.com/f1rstzi0n/USB-HID/blob/usb-hid-archive-20261012/src/boot/fw_update.h)、[APP linker](https://github.com/f1rstzi0n/USB-HID/blob/usb-hid-archive-20261012/STM32F446VETX_FLASH.ld)、[Boot linker](https://github.com/f1rstzi0n/USB-HID/blob/usb-hid-archive-20261012/STM32F446VETX_BOOT.ld)、[Boot 设计](https://github.com/f1rstzi0n/USB-HID/blob/usb-hid-archive-20261012/docs/BOOTLOADER_DESIGN.md) 与 [USB 身份](https://github.com/f1rstzi0n/USB-HID/blob/usb-hid-archive-20261012/firmware/usb_identity.h)。工程解耦不表示可以独立改变这些契约；双方必须独立编译，不能互相链接对方的工程产物。

ST PM0214 §4.4.4（p.227）规定 VTOR 偏移最低 9 位为零，最小对齐为 512 B；0x08010200 满足。APP linker 和 SystemInit 的 VTOR 必须同时指向此地址。F446 的扇区粒度为 4×16 KiB、1×64 KiB、3×128 KiB，64 KiB Boot 正好占扇区 0–3。依据：[ST PM0214](https://www.st.com/resource/en/programming_manual/pm0214-stm32-cortexm4-mcus-and-mpus-programming-manual-stmicroelectronics.pdf)、[ST RM0390](https://www.st.com/resource/en/reference_manual/rm0390-stm32f446xx-advanced-armbased-32bit-mcus-stmicroelectronics.pdf)、[现有地址检查](https://github.com/f1rstzi0n/USB-HID/blob/usb-hid-archive-20261012/tools/cubeide/verify_generated.ps1)。PM0214 已读官方正文；RM0390 官方索引及既有研究已核对，正文获取曾超时。

## 2. TinyUSB、双任务与接收/解析分离

TinyUSB 常规 HID/MSC 回调运行在 USB core task 中，USB 中断通过事件队列交给该 task。RTOS 模式下初始化与反初始化必须在运行 TinyUSB task 的同一 task/context 中。tud_task() 默认使用无限等待，tud_task_ext() 可以设有限等待；这些是当前本地源码和上游 API 的一致约束。依据：[官方并发说明](https://docs.tinyusb.org/en/latest/reference/concurrency.html)、[官方 usbd.h](https://raw.githubusercontent.com/hathach/tinyusb/master/src/device/usbd.h)、[本地 usbd.h](https://github.com/f1rstzi0n/USB-HID/blob/usb-hid-archive-20261012/third_party/tinyusb/src/device/usbd.h)。

源码已按以下双任务边界拆分：

- main.c 的 TASK_USB_IO 拥有 TinyUSB 的初始化、pump、类回调和 HID IN 提交。HID OUT 回调只检查接口、报表类型和固定长度，然后复制报表到接收队列。
- main.c 的 TASK_USB 拥有分片重组、帧校验、事务状态、服务分发、目录/光盘后台状态及 APP 维护处理。接收回调不执行这些处理。
- IN 完成、断连和总线复位同样交给协议所有者处理；重连前后的代际信息必须阻止旧应答和旧报表进入新连接。
- 不能把排队发送当作实际 IN 完成；只有真实完成事件才能授权进入 Boot、擦除或提交后复位。
- 队列满不能拼接丢失片前后的报表；保留分片 SEQ / MSGID / 超时防护，测试队列溢出后能重新同步。
- usb_io_task 遇到连接代际 epoch 改变时取消旧代次的控制提交，保留当前最新连接意图并自行在下一轮重投；usb_io_connect_applied 用请求 serial 返回控制已实际应用的时刻。业务任务不需要重新提交同一控制意图。
- 两工程均设置 CFG_TUD_TASK_EVENTS_PER_RUN=8，每轮 TinyUSB pump 最多消费 8 个事件；usb_proto_task 使用 USB_IO_RX_SLOTS=16 的预算，每轮最多消费 16 个 RX 报表。重新枚举期间或 USB 尚未挂载时仍调用 backend->task，避免后台状态因没有主机连接而停滞。

这些边界已经体现在 [APP main.c](../Core/Src/main.c) 的 TASK_USB / TASK_USB_IO、[usb_io.c](../Drivers/USB/usb_io.c) 的 usb_io_task / usb_io_connect_request / usb_io_connect_applied / HID 回调 / epoch 与 overflow 状态，以及 [usb_proto.c](../App/usb/usb_proto.c) 的 usb_proto_task。处理预算依据为 [APP tusb_config.h](../App/usb/tusb_config.h)、[Boot tusb_config.h](https://github.com/f1rstzi0n/USB-Bootloader/blob/usb-porting-baseline-20261012/App/usb/tusb_config.h)、[usb_io.h](../Drivers/USB/usb_io.h) 与 [本地 TinyUSB usbd.c](../Drivers/TinyUSB/src/device/usbd.c) 的 tud_task_ext。这些是应用自身实现的线程边界和每轮数量限制，不是 TinyUSB 自动提供的业务线程安全或墙钟时限保证；溢出、重连和处理延迟仍需要软件回归和实板验证。

FreeRTOS OSAL 的中断发送使用 xQueueSendToBackFromISR 并执行 ISR yield。调用 FromISR API 的 IRQ，其数值优先级必须大于或等于允许调用系统 API 的阈值。OTG_FS 的数值优先级必须与目标 configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY 保持相容，不能依赖旧板常量。当前目标配置值和 board_usb_hw_init 的中断优先级均为 5；以后改变 RTOS 阈值时须同步核对。依据：[TinyUSB 官方 OSAL](https://raw.githubusercontent.com/hathach/tinyusb/master/src/osal/osal_freertos.h)、[FreeRTOS 官方 ARM_CM4F port.c](https://raw.githubusercontent.com/FreeRTOS/FreeRTOS-Kernel/main/portable/GCC/ARM_CM4F/port.c)、[目标 FreeRTOSConfig.h](../Core/Inc/FreeRTOSConfig.h)。

目标 FreeRTOS 配置已启用静态分配、mutex 和 xTaskGetCurrentTaskHandle，tick 为 1000 Hz；原始 projdefs.h 未定义 pdTICKS_TO_MS，而当前 TinyUSB OSAL 用它转换毫秒。APP 的 App/usb/tusb_config.h 已补最小兼容定义，未为此升级 RTOS。HAL 毫秒时基由 TIM7 回调推进，保留该归属，不能再用 Boot 的 SysTick_Handler 覆盖 APP 的 RTOS SysTick。依据：[目标配置](../Core/Inc/FreeRTOSConfig.h)、[目标 projdefs.h](../Middlewares/Third_Party/FreeRTOS/Source/include/projdefs.h)、[APP tusb_config.h](../App/usb/tusb_config.h)、[目标 HAL timebase](../Core/Src/stm32f4xx_hal_timebase_tim.c)、[本地 OSAL 毫秒转换](https://github.com/f1rstzi0n/USB-HID/blob/usb-hid-archive-20261012/third_party/tinyusb/src/osal/osal_freertos.h)。

## 3. 新板 12 MHz / 180 MHz 时钟适配

目标基线使用 12 MHz HSE bypass，主 PLL M=6、N=180、P=2，得到 180 MHz SYSCLK；Q=3 得到 120 MHz，不能给 USB 直接使用主 PLLQ。原工程 25 MHz / 168 MHz 板的 board_usb_hw_init 强制 PLLQ=48 MHz，不能原样复制其时钟检查。依据：[目标 main.c](../Core/Src/main.c) 的 SystemClock_Config、[目标 ioc](../Test446.ioc) 的 RCC 配置、[旧板检查](https://github.com/f1rstzi0n/USB-HID/blob/usb-hid-archive-20261012/firmware/board.c)。

目标使用独立 PLLSAI：12 MHz / 6 × 96 / 4 = 48 MHz，CLK48 来源为 PLLSAIP。该配置在目标基线中位于 HAL_PCD_MspInit；现在两工程的 Drivers/USB/board.c 已由 board_usb_hw_init 配置同一 PLLSAI/CLK48，USB 不再初始化 HAL PCD。否则删除 PCD 会同时丢掉 USB 时钟。保留目标主 PLL、180 MHz、OverDrive、APB 分频和既有引脚，不套用旧板的主时钟参数。落地依据：[APP board.c](../Drivers/USB/board.c) 与 [Boot board.c](https://github.com/f1rstzi0n/USB-Bootloader/blob/usb-porting-baseline-20261012/Drivers/USB/board.c) 的 board_usb_hw_init；配置来源为目标基线 HAL_PCD_MspInit 与 ioc。官方/平台定义依据：[目标 ST RCC HAL 定义](../Drivers/STM32F4xx_HAL_Driver/Inc/stm32f4xx_hal_rcc_ex.h)、[ST RM0390 RCC 章节](https://www.st.com/resource/en/reference_manual/rm0390-stm32f446xx-advanced-armbased-32bit-mcus-stmicroelectronics.pdf)。

两工程 Drivers/USB/board.c 已定义 OTG_FS_IRQHandler 并只转交 TinyUSB；其他强中断入口及 HAL PCD 接管必须由构建和符号检查继续排除。仍需 PA9 VBUS 检测、TinyUSB 内部 D+ 上拉和 PE4 高阻要求。新板振荡器实际频率、USB 48 MHz 精度、电压与物理接线必须实测，ioc 配置值不构成实板证据。依据：[源工程硬件指南](https://github.com/f1rstzi0n/USB-HID/blob/usb-hid-archive-20261012/CLAUDE.md)、[现有唯一中断入口](https://github.com/f1rstzi0n/USB-HID/blob/usb-hid-archive-20261012/firmware/board.c)、[ST 数据手册 PLL / 时钟参数](https://www.st.com/resource/en/datasheet/stm32f446ve.pdf)。

## 4. IWDG、内部 Flash 与签名验证

两目标基线在 main.c 初始化 IWDG，Prescaler=32、Reload=4095，原先未调用 refresh。本次源码保留这些配置，已增加下述主流程和验证进展刷新。HAL_IWDG_Init 会启动并刷新看门狗，不是仅配置一个未启用的外设。依据：[目标 IWDG HAL](../Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_iwdg.c) 与 [目标 main.c](../Core/Src/main.c)。

已读取 ST DS10693 Rev 11：表 42（p.106）给出 LSI 17/32/47 kHz，条件为 VDD=3 V、TA=−40～105°C；当前最短窗口按最大 LSI 计算为 4096×32/47000≈2.789 s。§6.3.13、表 48（p.111–112）给出 x32 擦除 64 KiB 最大 1.1 s、128 KiB 最大 2 s，word 编程最大 100 μs；x32 编程供电必须 2.7–3.6 V，Flash 温度范围为 −40～105°C。依据：[ST DS10693 Rev 11](https://www.st.com/resource/en/datasheet/stm32f446ve.pdf)。这是有温压条件的规格预算，不能当作任意供电下的测量结果。

扇区 4–7 最坏总擦除时间为 1.1+3×2=7.1 s，超过看门狗窗口。Boot 的 flash_erase_sector 已逐扇区调用 HAL，NbSectors=1，并在擦除前及 HAL 返回成功后刷新；擦除前刷新可给每个扇区完整窗口，单扇区理论余量约 0.789 s。无需修改 HAL 库、加 RAM 擦除程序或扩大 IWDG 配置。Flash 擦写期间从同一 Flash 取指/读数据可能停顿，所以不能依赖中断定时刷新。源码依据：[Boot fw_platform.c](https://github.com/f1rstzi0n/USB-Bootloader/blob/usb-porting-baseline-20261012/App/firmware/fw_platform.c) 的 flash_erase_sector；官方依据：[ST RM0390 §3.5](https://www.st.com/resource/en/reference_manual/rm0390-stm32f446xx-advanced-armbased-32bit-mcus-stmicroelectronics.pdf)。

已落地的喂狗位置（源码事实，不表示新板已验收）：

- Boot main 的 USB / 协议 / 升级 pump 完整返回后刷新，允许正常空闲；jump_application 在跳转 APP 前刷新。
- flash_erase_sector 每个已授权单扇区擦除前刷新，HAL 返回成功后刷新；flash_program 在成功完成一个 word 编程并运行 USB pump 后刷新。不在 HAL BUSY 循环或常驻 ISR 无条件刷新。
- boot_disconnect 在擦除和复位前提交断连意图，等待 usb_io_connect_applied 对同一 serial 确认已应用，并从其返回的实际断连时刻起等待至少 250 ms；等待循环运行 USB pump 并刷新。擦除后 boot_wait(250) 另从调用时刻等待 250 ms，再提交重连意图。两种等待均依赖 HAL TIM7 毫秒时基，因此 TIM7 正常推进必须实板验证。
- fw_image_check 每次成功处理最多 256 B 摘要块后调用 fw_validation_progress；fw_p256_verify 每次 restartable poll 返回后调用同一进展钩子。P-256 保留 64 ops 分片预算，未实施 2 秒硬超时。若卡在单次 poll 内，进展钩子不会再次运行；实际分片和总耗时仍需新板测量，不能宣称满足固定墙钟期限。
- 两工程 fw_validation_progress 的强定义均位于各自 App/firmware/fw_platform.c；Drivers/Crypto 只保留默认空的弱定义，Drivers/USB/board.c 提供 board_watchdog_refresh。APP 的进展钩子只喂狗；Boot 的钩子先喂狗，且 tud_inited() 为真时运行 usb_io_task，因此 USB 初始化后的摘要块和 P-256 poll 之间仍维护 USB。Boot 启动校验时 USB 尚未初始化，钩子只喂狗。
- APP 的 TASK_USB 在自己的处理循环返回且 TASK_USB_IO 最近 1000 ms 有进展时刷新。初始化镜像验证使用进展钩子；该初始化路径发生在 USB 栈任务正式运行之前。

Crypto 已拆到 Drivers/Crypto，Mbed TLS 位于 Drivers/MbedTLS；AFW 固定契约、只读镜像验证、FW_INFO 和信任公钥政策位于各自 App/firmware。APP 使用 fw_image_t，不链接 Boot 的升级状态机；目录改为直接包含 fw_crypto.h。Crypto 仍使用单一静态验签上下文和 16 KiB arena，APP 由 TASK_USB、Boot 由裸机主流程串行推进，不能并发调用。源码依据：[fw_crypto.c](../Drivers/Crypto/fw_crypto.c) 的 fw_p256_start / fw_p256_poll / fw_p256_verify、[fw_image.c](../App/firmware/fw_image.c) 的 fw_image_init / fw_image_check / fw_image_info、[目录 store](../App/content/bvpv_catalog_store.c)。喂狗及 USB 进展适配依据：[APP fw_platform.c](../App/firmware/fw_platform.c) 的 fw_validation_progress、[Boot fw_platform.c](https://github.com/f1rstzi0n/USB-Bootloader/blob/usb-porting-baseline-20261012/App/firmware/fw_platform.c) 的 flash_erase_sector / flash_program / boot_disconnect / boot_wait / jump_application / fw_validation_progress、[Boot board.c](https://github.com/f1rstzi0n/USB-Bootloader/blob/usb-porting-baseline-20261012/Drivers/USB/board.c) 的 board_watchdog_refresh，以及 APP main.c 的 TASK_USB / TASK_USB_IO。

## 5. 发布、验收和未核实项

BEGIN 必须等实际 HID IN 完成才允许擦除；FINISH 必须校验正文摘要、向量、签名头和最终提交字，实际 IN 完成后才复位。APP 进入 Boot 仍需维护条件检查，缺失时拒绝。SRAM magic 仅在软件复位、无 POR/BOR 且 magic 与反码匹配时接受并立即消费。依据：[Boot 协议实现](https://github.com/f1rstzi0n/USB-HID/blob/usb-hid-archive-20261012/src/boot/fw_update.c)、[平台请求与跳转](https://github.com/f1rstzi0n/USB-HID/blob/usb-hid-archive-20261012/firmware/fw_platform.c)、[Boot 设计](https://github.com/f1rstzi0n/USB-HID/blob/usb-hid-archive-20261012/docs/BOOTLOADER_DESIGN.md)。

继续使用既有 AFW 发布工具、新 APP BIN 和 Boot HEX；版本必须与目录 FirmwareVersion 一致。首装使用签名 factory HEX，不能用普通 APP HEX 代替；Boot/公钥更新走 SWD。发布工具能检查 Boot 地址范围，但不能证明其内嵌公钥匹配，制造流程必须核对。依据：[发布工具说明](https://github.com/f1rstzi0n/USB-HID/blob/usb-hid-archive-20261012/tools/firmware/README.md)。

软件验收应覆盖两个工程独立 Debug/Release 构建、Boot 64 KiB 上限、APP 地址与容量、mailbox/public key 一致、Boot 无 RTOS/MSC/W25Q64 symbols、APP 无内部 Flash 擦写入口；复用现有 boot_update / boot_usb / usb_proto / catalog 回归及发布工具 self-test，增加双任务溢出、断连、旧连接事件与 IN gate 检查。

实板尚未验证：12 MHz 振荡器及 USB 48 MHz、180 MHz 的电压/OverDrive条件、IWDG 实际窗口、P-256 单次分片及总验签耗时、TIM7 等待推进、单扇区擦除余量、HID + MSC 枚举/写保护、W25Q64 与电机 SPI2 仲裁、签名升级/断电恢复、WRP/BOR、EEPROM/校准兼容与真实维护条件。源工程历史验收不能替代新板验收。

注意源工程 docs/PORTING.md 开头仍写协议 v2 与 MSC 双槽，已与当前指南冲突。本次协议与分区采用 CLAUDE.md、当前 PROTOCOL.md 的 v4.3 / BVC4 布局及 BOOTLOADER_DESIGN.md；不自动迁移或格式化已有外部 Flash。
