# USB HID / MSC 移植说明

更新日期：2026-10-12。当前范围是通信移植及 APP / Boot 拆分，保留 Fake 参数语义。

本轮没有烧录目标板，也没有测量目标板的 USB、时序或内存高水位。软件证据和待实板项目见 [移植验证记录](validation/USB-PORTING-20261012.md)。参考目录中的原工程实板结论不能代替这两个目标工程的验收。

## 1. 独立工程与目录

| 工程 | 应用代码 | 驱动与库 | 运行方式 |
| --- | --- | --- | --- |
| Test446 | `App/protocol`、`catalog`、`service`、`content`、`generated`、`usb`、`firmware` | `Drivers/USB`、`TinyUSB`、`W25Q64`、`Crypto`、`MbedTLS`，以及既有 HAL / CMSIS | FreeRTOS；HID + 只读 MSC 光驱 |
| Test446-Boot | `App/protocol`、`usb`、`firmware` | `Drivers/USB`、`TinyUSB`、`Crypto`、`MbedTLS`，以及既有 HAL / CMSIS | 裸机；仅 HID |

两个工程各自拥有完整源码与 Debug / Release 配置，构建不引用原 USB-HID 工程或对方工程。原工程保留；目标修改前基线位于 `C:/Users/17907/AppData/Local/Temp/usb-hid-porting-20261012`。

协议 engine 通过 `usb_proto_backend_t` 注入 APP 或 Boot 行为；服务码与重枚举策略由各自 `App` adapter 提供。`App/firmware/fw_image.*` 和 `fw_contract.h` 是两工程各自的只读镜像验证及固件契约；APP 不携带 `fw_update.c` 或内部 Flash 擦写入口。SHA-256 / P-256 验签封装位于 `Drivers/Crypto`，隐藏 MbedTLS 实现，使用静态 16 KiB arena，验签串行执行。

共享代码是各工程内的副本。修改后在本工程运行 `node tools/cubeide/verify_shared.js`；需要比较两份副本时，显式传入另一工程目录。校验依据是 `docs/shared-source-manifest.json`；更新共享实现时应同时审查两工程及 manifest，不能只修改一份。

## 2. APP 两任务与接收门禁

`Core/Src/main.c` 中：

- `TASK_USB_IO()` 唯一调用 TinyUSB，负责栈事件、原始 64 B OUT 报告投递、IN 提交和实际完成通知。使用 `tud_task_ext(0, false)`，每轮最多处理 8 个栈事件，随后 `osDelay(1)`。
- `TASK_USB()` 唯一拥有分片重组、帧解析、服务分发、目录 / 设备光盘维护状态。每轮最多取 16 个 RX 报告；未挂载或重枚举期间仍推进 backend 的后台工作，随后 `osDelay(1)`。
- worker 初始化完成后，IO 才初始化 USB 栈。USB reset ISR 只使连接代次失效、关闭门禁与清理交接状态，不解析数据、不调用业务代码。

交接使用静态 RX16 报告槽、单槽 TX mailbox、单槽 DONE 与短控制通道；复用 worker 原有 `70 × 64 B` TX 缓冲，不新增 4 KiB 帧队列。最大完整协议帧为 4096 B，最大载荷 4088 B，最多 68 个 HID 报告。

`epoch` 隔离不同连接，`ticket` 隔离接收窗口，非零 `token` 隔离不同应答。worker 收到完整 EOF 后原子 claim 当前窗口并清除同 ticket 已排入的剩余报告；忙期请求被丢弃，不延后执行业务。reset / overflow 后旧 EOF 无法 claim，dispatch 与 DONE 也核对实时 epoch。

普通应答最后一个 IN **实际完成** 后，IO 在同一短临界区发布 DONE 并开放新 ticket；worker 在处理新窗口 RX 前先消费 DONE。入队或调用提交成功不代表主机已经收到。TX 超时及迟到 completion 不授权擦除、复位、重枚举或重新打开旧窗口。RX 溢出时关闭窗口、废弃当前半帧，后续从新 SOF 开始。

当前时间限制：RX 报告超过 200 ms 不再重组；TX 总 watchdog 为 500 ms。重枚举成功应答后等待 100 ms，再申请断连；250 ms 断连保持从 IO 实际应用断连的时刻计算。CONNECT 当前意图若被 reset 代次取消，由 IO 自行在新代次重投，包含无需查询的重新连接命令。

TinyUSB 的 endpoint mutex 只能由 IO 上下文使用；短临界区覆盖最终 generation 检查与 HID 提交。新增业务代码不得从 worker、其他任务或 ISR 直接调用 TinyUSB。

## 3. MSC、媒体状态与 SPI2

锁顺序固定为 **media mutex → SPI2 mutex**，禁止反向取得。

worker 持有 media mutex 推进内容 / 目录状态，释放前发布 ready / block-count 快照。TinyUSB MSC 读取及介质事件只尝试取锁，不等待 worker；锁忙时读取返回 BUSY，由 TinyUSB 稍后重试。容量和事件使用一致快照，不能直接跨任务读取可变的内容状态。

SPI2 是真实 FreeRTOS mutex；维护会话通过 reserve 排除 Motor，reserve 不跨会话持有 mutex。会话之间也由内容 owner 排他；Flash 事务取得并释放总线。ISR 不得 reserve、release 或调用 W25Q64。W25 async 状态的检查和修改与 SPI2 事务一起受 mutex 保护；同步 read / write / erase 遇到在途异步操作返回 BUSY。

MSC 是只读 CD-ROM，逻辑块 2048 B，FS 传输块最多 64 B；更新走 HID，不通过 SCSI WRITE_10。空白、旧布局或坏记录保持空光驱，启动不自动擦写或装入工厂镜像。

## 4. 当前协议和单槽布局

协议文档版本为 **v4.3**，线级版本为 **4**；不是早期 v2 或双内容槽设计。

| W25Q64 区域 | 地址（含末地址） | 容量 |
| --- | --- | --- |
| 设备光盘，单槽 | `0x000000–0x3FFFFF` | 4 MiB |
| 签名参数目录，单槽 | `0x400000–0x4FFFFF` | 1 MiB |
| MCU 程序预留 | `0x500000–0x7EFFFF` | 3008 KiB |
| 管理元数据 | `0x7F0000–0x7FFFFF` | 64 KiB |

BVC4 光盘提交记录位于 `0x7F0000 / 0x7F1000`；ACC1 目录记录位于 `0x7F2000 / 0x7F3000`。两份交替记录管理同一内容槽，不代表两份镜像。光盘 BEGIN 先提交无介质记录，FINISH 完整 CRC 通过后最后提交有效记录。参数目录先做零擦写预检，第二遍先提交无效目录记录再覆盖，最终验签与运行契约检查通过后提交有效记录。单槽中断后需整包恢复，不回退旧记录或旧内容。

## 5. 真实业务接入点

当前 2183 项参数使用冻结生成的 compact 运行契约和 RAM 值；普通写入仅改变 RAM，USB 拔插保留，MCU 复位恢复编译初值。OWO 命令校验后 ACK，不执行电机、学习、EEPROM 保存或业务重启。设备光盘与参数目录使用真实 W25Q64 后端。

接入成熟工程时，改 `App/usb/usb_app.c` 的应用 adapter：

- 将 `bvpv_catalog_data_ctx()` 提供的 value 映射到受控运行值或明确的同步缓冲；保留地址、类型、执行尺寸和硬权限 / 范围，不能把外部参数目录变成执行脚本。
- 将 `fake_command()` 替换为成熟工程的命令入口，定义业务完成及错误语义；耗时动作交给对应业务任务，不能阻塞 IO 或无限占用 worker。
- 将 `fake_maintenance_ready()` 替换为设备静止及安全维护判定，并用于目录覆盖及 ENTER_BOOT；真实 Motor 的 SPI2 访问必须接入同一仲裁。
- 动态属性 / pending / revision 使用目录模型的 provider 和变化记录接口；与其他业务任务共享状态时，增加明确的数据同步，不允许多个任务直接修改协议上下文。

本轮未将这些 Fake 接入点替换为成熟工程业务。

## 6. 时基、引脚、看门狗与内存

两工程保留 12 MHz HSE bypass 和 180 MHz 主时钟。USB 使用板级 PLLSAIP 的独立 48 MHz：M=6、N=96、P=4。HAL 毫秒时基为 TIM7；`usb_port_millis()` 与 TinyUSB 时间函数使用 `HAL_GetTick()`。APP SysTick 提供 1000 Hz FreeRTOS tick；不得为 USB 重复接管 SysTick。

TinyUSB 独占 OTG_FS，唯一 `OTG_FS_IRQHandler` 在 `Drivers/USB/board.c`，不启用 HAL PCD。PA11 / PA12 为 USB DM / DP，PA9 检测 VBUS。PE4 经 1.5 kΩ 直连 D+，保持无上下拉输入高阻，连接控制使用 TinyUSB 软断连。Flash / Motor CS 初始取消选中。

APP worker 每轮更新内存诊断，只有 IO 最近 1000 ms 有推进才刷新 IWDG；Boot 主循环及长擦写路径刷新 IWDG。强 `fw_validation_progress()` 位于各自 `App/firmware/fw_platform.c`：APP 刷新看门狗；Boot 在栈已初始化时额外 pump `usb_io_task()`，不递归解析请求。IWDG 既有配置为 prescaler 32、reload 4095，实际复位时间需测量 LSI 与实板时序。

| 项目 | 当前配置 / 变量 |
| --- | --- |
| FreeRTOS heap | 20 KiB；`g_usb_min_heap` 是历史最小空闲字节数 |
| worker 栈 | 3072 B；`g_usb_worker_stack_free` 是历史最小剩余字节数 |
| IO 栈 | 2048 B；`g_usb_io_stack_free` 是历史最小剩余字节数 |
| RX 诊断 | `g_usb_rx_high_water` 为报告槽数；`g_usb_rx_overflows` 为溢出次数 |
| Flash 诊断 | `g_w25q64_jedec_id`、`g_w25q64_status` |

软件布局测量：

| 配置 | Flash（B） | 静态 RAM（B） | 链接 heap / stack 保留（B） | 剩余 RAM（B） |
| --- | ---: | ---: | ---: | ---: |
| APP Debug | 257588 | 112060 | 3076 | 15928 |
| APP Release | 158704 | 110744 | 3072 | 17248 |

APP 静态 RAM 已包含 20 KiB RTOS heap，剩余 RAM 不等于 heap 空闲。Boot Debug / Release 为 42196 B Flash，静态 RAM 33032 B。实际报告以 [验证记录](validation/USB-PORTING-20261012.md) 和最终 map 为准。

实板必须确认最小空闲 heap ≥ 1 KiB、两个 USB 栈各 ≥ 256 B，并测量高负载 HID / MSC、12 MHz 输入及 USB 48 MHz、watchdog、擦写与断连时序。当前这些项目尚未测量。

## 7. CubeMX、构建、首装与验证

固件唯一构建定义是每个工程自己的 CubeIDE managed build。CubeMX 再生成后，在对应工程根目录依次运行：

工程脚本使用 PowerShell 7（`pwsh`）；Windows PowerShell 5.1 无法解析部分 UTF-8 无 BOM 脚本。

```powershell
pwsh -NoProfile -ExecutionPolicy Bypass -File tools/cubeide/apply_sources.ps1
pwsh -NoProfile -ExecutionPolicy Bypass -File tools/cubeide/verify_generated.ps1
pwsh -NoProfile -ExecutionPolicy Bypass -File tools/cubeide/verify_owned_c.ps1
node tools/cubeide/verify_shared.js
```

随后重新构建该工程 Debug / Release 并检查 Flash / RAM 布局。恢复脚本修复 source / include / 宏 / linker / 优化配置；调试 launch 保持仅符号加载并在再生成后复核；检查脚本确认无 HAL PCD、单一 USB ISR、时基、引脚、任务、地址和依赖。生成检查失败时先修复生成配置，不能绕过检查继续烧录。自有 C 遵守块首先声明变量、后赋值使用；第三方代码保留原样。

Boot 在 `0x08000000–0x0800FFFF`，APP 的 512 B 元数据区从 `0x08010000` 开始，向量与正文从 `0x08010200` 开始。APP VTOR 为 `0x08010200`，Boot VTOR 为 `0x08000000`；SRAM 顶部 `0x2001FFF8` 保留 8 B NOLOAD boot-request mailbox。Boot 只擦写内部 Flash sector4–7，先验证签名，再按实际 ACK 授权擦除；正文、manifest 和读回均通过后最后写 commit。

Debug launch 默认仅加载符号，不下载裸 ELF。首次 SWD 安装必须使用匹配公钥及契约的签名 `.factory.hex`（Boot + 签名 APP）；已装正确 Boot 的 SWD 维护使用 `.signed-app.hex`，USB 更新使用 `.afw`。裸 APP ELF / BIN / HEX 缺少有效提交身份，不能替代签名首装。发布公钥必须与两个固件及主机一致；测试 fixture 正文不是可执行设备固件。

当前软件回归 APP 18 / 18、Boot 2 / 2 通过；块首声明检查覆盖 APP 27、Boot 13 个生产 C 文件。共享 265 文件一致，242 个第三方文件逐字相同，源基线 106 文件未改。具体命令、四配置构建与实板待验项目集中记录在 [移植验证记录](validation/USB-PORTING-20261012.md)。
