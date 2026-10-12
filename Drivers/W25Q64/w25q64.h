/* W25Q64FV（8MB SPI NOR Flash）驱动 —— MSC 光盘内容的存储仓。
 *
 * 本层只管"在 8MB 平坦地址空间上读/写/擦"，不知道镜像分区布局；
 * 分区语义（光盘4MiB、参数表1MiB、MCU及元数据3MiB）见
 * docs/PROTOCOL.md §8，由 src/content/content_store.c 承担。
 *
 * 并发：所有函数内部自带 SPI2 总线仲裁（spi2_bus_try_acquire），**调用方不需要
 * 自己加锁、也不应直接碰 SPI**。拿不到总线返回 W25Q64_BUSY，调用方稍后重试即可
 * （MSC 侧正是这个语义：返回 TUD_MSC_RET_BUSY，TinyUSB 会用同样参数重试）。
 *
 * MSC 读取采用同步短事务；运行期更新使用非阻塞 start/poll，启动阶段可同步等待。
 * tud_msc_read10_cb 由 tud_task() 在主循环上下文派发
 * （TinyUSB 的 MSC 驱动 xfer_isr 为 NULL，XFER_COMPLETE 走 queue_event），
 * 不是中断上下文；一次 64B 读约 52µs（10.5MHz），不侵占中断。亦因此不需要
 * TUD_MSC_RET_ASYNC 那套异步机制。
 *
 * 无扇区缓存：MSC 每次回调最多 64B，直读的命令开销 4B/68B ≈ 6%，
 * 总 SPI 时间与带缓存相当，但最坏延迟从 3.1ms 降到 52µs 且不额外占 RAM。 */
#ifndef W25Q64_H
#define W25Q64_H

#include <stdbool.h>
#include <stdint.h>

/* W25Q64FV（8MB）JEDEC ID = EF 40 17 */
#define W25Q64_JEDEC_ID      0x00EF4017u
#define W25Q64_SIZE_BYTES    0x00800000u   /* 8MB */
#define W25Q64_PAGE_SIZE     256u          /* 页编程粒度 */
#define W25Q64_SECTOR_SIZE   4096u         /* 扇区擦除粒度 */

typedef enum {
    W25Q64_OK    = 0,    /* 成功 */
    W25Q64_BUSY  = -1,   /* SPI2 被电机侧占用，本次未执行，调用方稍后重试 */
    W25Q64_ERROR = -2    /* 器件无应答 / 参数非法 / 超时 */
} w25q64_status_t;

/* 供 J-Link 现场观察的诊断量（Debug 构建下地址取自 Debug/usb-hid.map）：
 *   g_w25q64_jedec_id：探测结果，0x00EF4017 = W25Q64FV 在位；0x000000 = 无应答。
 *   g_w25q64_status  ：最近一次操作的 w25q64_status_t。
 * volatile 是必需的——否则 -O0 之外会被优化掉，调试器就看不到。 */
extern volatile uint32_t g_w25q64_jedec_id;
extern volatile int32_t  g_w25q64_status;

/* 探测器件（读 JEDEC ID 0x9F）。ID 匹配 W25Q64_JEDEC_ID 才算成功。
 * 结果同时存入 g_w25q64_jedec_id，无论成败——这是本步骤唯一不依赖主机的真机观测手段。 */
bool w25q64_init(void);

/* 读任意长度（0x03 READ，无 dummy 周期，跨页无限制）。CS 由本函数管理。 */
w25q64_status_t w25q64_read(uint32_t addr, uint8_t *buf, uint32_t len);

/* 写任意长度（0x06 WREN + 0x02 PP，自动按 256B 页边界切分，每页前重新 WREN）。
 * **不做擦除**：NOR Flash 只能把 1 写成 0，目标区域须已擦除，否则写入结果按位与。
 * 擦除时间上限（页编程典型 0.7ms/上限 3ms）内轮询 RDSR1.BUSY。 */
w25q64_status_t w25q64_write(uint32_t addr, const uint8_t *buf, uint32_t len);

/* 擦 4KB 扇区（0x06 WREN + 0x20 SE）。addr 低 12 位被忽略（器件只看扇区号）。
 * 典型 45ms、上限 400ms，内部按 1s 超时轮询 BUSY。 */
w25q64_status_t w25q64_erase_sector_4k(uint32_t addr);

/* 运行期更新使用这些非阻塞入口；一次只允许一个在途操作。 */
w25q64_status_t w25q64_erase_start(uint32_t addr);
w25q64_status_t w25q64_program_start(uint32_t addr, const uint8_t *buf, uint16_t len);
/* 0=完成、1=忙碌、-1=失败；超时后保持故障，复位才能重新使用。 */
int w25q64_poll(void);
#endif /* W25Q64_H */
