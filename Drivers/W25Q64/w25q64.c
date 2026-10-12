#include "w25q64.h"

#include "main.h"        /* SPI2_CS_FLASH_Pin / SPI2_CS_FLASH_GPIO_Port（CubeMX 生成，单一真源） */
#include "spi2_bus.h"

/* W25Q64 指令集（本驱动用到的子集） */
#define CMD_WREN   0x06u   /* 写使能 */
#define CMD_RDSR1  0x05u   /* 读状态寄存器 1（bit0=BUSY、bit1=WEL） */
#define CMD_READ   0x03u   /* 读数据（无 dummy 周期） */
#define CMD_PP     0x02u   /* 页编程（≤256B，不跨页） */
#define CMD_SE     0x20u   /* 4KB 扇区擦除 */
#define CMD_JEDEC  0x9Fu   /* 读 JEDEC ID（3 字节） */

#define SR1_BUSY   0x01u

/* 超时（ms）。10.5MHz 下一次 64B 传输约 52µs，几百 ms 已是宽容上限。 */
#define TIMEOUT_XFER_MS    100u    /* 单次 SPI 传输 */
#define TIMEOUT_PROGRAM_MS 100u    /* 页编程：典型 0.7ms、上限 3ms */
#define TIMEOUT_ERASE_MS   1000u   /* 扇区擦除：典型 45ms、上限 400ms */

/* 读操作分块长度：HAL 的 Size 是 uint16_t，分块后 len 可任意大（含旧单槽迁移）。 */
#define READ_CHUNK         4096u

volatile uint32_t g_w25q64_jedec_id;
volatile int32_t  g_w25q64_status;
static bool s_async_active, s_async_fault;
static uint32_t s_async_start, s_async_timeout;

/* ---- 片选 ---- */
static void cs_low(void)
{
    HAL_GPIO_WritePin(SPI2_CS_FLASH_GPIO_Port, SPI2_CS_FLASH_Pin, GPIO_PIN_RESET);
}

static void cs_high(void)
{
    HAL_GPIO_WritePin(SPI2_CS_FLASH_GPIO_Port, SPI2_CS_FLASH_Pin, GPIO_PIN_SET);
}

/* SPI 出错后 HAL 会把句柄留在非 READY 态，之后每次调用都被 `HAL_BUSY` 挡回，
 * 表现为"Flash 突然永久失效"。失败时复位一次状态机，避免这种误导性故障。 */
static void spi_recover(void)
{
    SPI_HandleTypeDef *h = spi2_bus_handle();
    if (h->State != HAL_SPI_STATE_READY) {
        (void)HAL_SPI_Abort(h);
    }
}

static bool spi_tx(const uint8_t *tx, uint16_t len)
{
    if (HAL_SPI_Transmit(spi2_bus_handle(), (uint8_t *)(uintptr_t)tx, len, TIMEOUT_XFER_MS) != HAL_OK) {
        spi_recover();
        return false;
    }
    return true;
}

static bool spi_rx(uint8_t *rx, uint16_t len)
{
    if (HAL_SPI_Receive(spi2_bus_handle(), rx, len, TIMEOUT_XFER_MS) != HAL_OK) {
        spi_recover();
        return false;
    }
    return true;
}

/* 组装 [cmd][addr24] 命令头。 */
static void cmd_addr(uint8_t *b, uint8_t cmd, uint32_t addr)
{
    b[0] = cmd;
    b[1] = (uint8_t)(addr >> 16);
    b[2] = (uint8_t)(addr >> 8);
    b[3] = (uint8_t)addr;
}

/* 独立的 WREN 片选周期（CS 低→06→CS 高）。必须在每次 PP/SE 前调用。 */
static bool wren(void)
{
    const uint8_t c = CMD_WREN;
    bool ok;
    cs_low();
    ok = spi_tx(&c, 1u);
    cs_high();
    return ok;
}

/* 轮询 RDSR1.BUSY 直至就绪，或超时。
 * 每次轮询是一个独立片选周期——这是 W25Q64 的常规用法（擦除/编程期间不接受新指令）。 */
static bool wait_ready(uint32_t timeout_ms)
{
    const uint8_t c = CMD_RDSR1;
    const uint32_t start = HAL_GetTick();

    for (;;) {
        uint8_t sr = 0xFFu;
        cs_low();
        if (!spi_tx(&c, 1u) || !spi_rx(&sr, 1u)) {
            cs_high();
            return false;
        }
        cs_high();
        if ((sr & SR1_BUSY) == 0u) {
            return true;
        }
        if ((uint32_t)(HAL_GetTick() - start) >= timeout_ms) {
            return false;
        }
    }
}

bool w25q64_init(void)
{
    const uint8_t c = CMD_JEDEC;
    uint8_t id[3] = { 0u, 0u, 0u };
    bool ok = false;

    g_w25q64_jedec_id = 0u;
    if (!spi2_bus_try_acquire(SPI2_BUS_OWNER_FLASH)) {
        g_w25q64_status = W25Q64_BUSY;
        return false;
    }
    /* MCU 单独复位时 Flash 可能还在完成上一条指令。 */
    if(!wait_ready(TIMEOUT_ERASE_MS)) {
        spi2_bus_release();
        g_w25q64_status=W25Q64_ERROR;
        return false;
    }
    cs_low();
    ok = spi_tx(&c, 1u) && spi_rx(id, 3u);
    cs_high();
    spi2_bus_release();

    if (!ok) {
        g_w25q64_status = W25Q64_ERROR;
        return false;
    }
    g_w25q64_jedec_id = ((uint32_t)id[0] << 16) | ((uint32_t)id[1] << 8) | (uint32_t)id[2];
    if (g_w25q64_jedec_id != W25Q64_JEDEC_ID) {
        g_w25q64_status = W25Q64_ERROR;   /* 有应答但不是 W25Q64FV：贴错料或容量不符 */
        return false;
    }
    g_w25q64_status = W25Q64_OK;
    return true;
}

w25q64_status_t w25q64_read(uint32_t addr, uint8_t *buf, uint32_t len)
{
    uint8_t  cmd[4];
    uint32_t left = len;
    uint8_t *p = buf;
    w25q64_status_t st = W25Q64_OK;

    if ((buf == 0) || (len == 0u) ||
        (addr >= W25Q64_SIZE_BYTES) || (len > (W25Q64_SIZE_BYTES - addr))) {
        return W25Q64_ERROR;
    }
    if (!spi2_bus_try_acquire(SPI2_BUS_OWNER_FLASH)) {
        return W25Q64_BUSY;
    }
    if (s_async_fault || s_async_active) {
        w25q64_status_t state = s_async_fault ? W25Q64_ERROR : W25Q64_BUSY;
        spi2_bus_release();
        return state;
    }

    cmd_addr(cmd, CMD_READ, addr);
    cs_low();
    if (!spi_tx(cmd, 4u)) {
        st = W25Q64_ERROR;
    } else {
        while (left > 0u) {
            uint16_t n = (uint16_t)((left > READ_CHUNK) ? READ_CHUNK : left);
            if (!spi_rx(p, n)) {
                st = W25Q64_ERROR;
                break;
            }
            p += n;
            left -= n;
        }
    }
    cs_high();
    spi2_bus_release();

    g_w25q64_status = st;
    return st;
}

w25q64_status_t w25q64_write(uint32_t addr, const uint8_t *buf, uint32_t len)
{
    uint8_t  cmd[4];
    uint32_t left = len;
    const uint8_t *p = buf;
    w25q64_status_t st = W25Q64_OK;

    if ((buf == 0) || (len == 0u) ||
        (addr >= W25Q64_SIZE_BYTES) || (len > (W25Q64_SIZE_BYTES - addr))) {
        return W25Q64_ERROR;
    }
    if (!spi2_bus_try_acquire(SPI2_BUS_OWNER_FLASH)) {
        return W25Q64_BUSY;
    }
    if (s_async_fault || s_async_active) {
        w25q64_status_t state = s_async_fault ? W25Q64_ERROR : W25Q64_BUSY;
        spi2_bus_release();
        return state;
    }

    while (left > 0u) {
        /* 页编程不得跨 256B 页边界：本页还能写多少 */
        uint32_t page_left = W25Q64_PAGE_SIZE - (addr % W25Q64_PAGE_SIZE);
        uint16_t n = (uint16_t)((left < page_left) ? left : page_left);

        cmd_addr(cmd, CMD_PP, addr);
        if (!wren()) {
            st = W25Q64_ERROR;
            break;
        }
        cs_low();
        if (!spi_tx(cmd, 4u) || !spi_tx(p, n)) {
            cs_high();
            st = W25Q64_ERROR;
            break;
        }
        cs_high();
        if (!wait_ready(TIMEOUT_PROGRAM_MS)) {
            st = W25Q64_ERROR;
            break;
        }
        addr += n;
        p += n;
        left -= n;
    }

    spi2_bus_release();
    g_w25q64_status = st;
    return st;
}

w25q64_status_t w25q64_erase_sector_4k(uint32_t addr)
{
    uint8_t cmd[4];
    w25q64_status_t st = W25Q64_OK;

    if (addr >= W25Q64_SIZE_BYTES) {
        return W25Q64_ERROR;
    }
    addr &= ~(W25Q64_SECTOR_SIZE - 1u);   /* 器件只看扇区号，这里显式对齐便于调用方推理 */

    if (!spi2_bus_try_acquire(SPI2_BUS_OWNER_FLASH)) {
        return W25Q64_BUSY;
    }
    if (s_async_fault || s_async_active) {
        w25q64_status_t state = s_async_fault ? W25Q64_ERROR : W25Q64_BUSY;
        spi2_bus_release();
        return state;
    }

    cmd_addr(cmd, CMD_SE, addr);
    if (!wren()) {
        st = W25Q64_ERROR;
    } else {
        cs_low();
        if (!spi_tx(cmd, 4u)) {
            cs_high();
            st = W25Q64_ERROR;
        } else {
            cs_high();
            if (!wait_ready(TIMEOUT_ERASE_MS)) {
                st = W25Q64_ERROR;
            }
        }
    }

    spi2_bus_release();
    g_w25q64_status = st;
    return st;
}

/* 运行期发出指令即返回；每轮主循环只读一次 BUSY，避免阻塞 USB。 */
static w25q64_status_t async_start(uint32_t addr, const uint8_t *buf, uint16_t len, bool erase)
{
    uint8_t cmd[4];
    bool ok;
    if (addr >= W25Q64_SIZE_BYTES || (erase && addr % W25Q64_SECTOR_SIZE) ||
        (!erase && (!buf || !len || len > W25Q64_PAGE_SIZE || len > W25Q64_SIZE_BYTES - addr ||
                    len > W25Q64_PAGE_SIZE - addr % W25Q64_PAGE_SIZE)))
        return W25Q64_ERROR;
    if (!spi2_bus_try_acquire(SPI2_BUS_OWNER_FLASH))
        return W25Q64_BUSY;
    if (s_async_fault || s_async_active) {
        w25q64_status_t state = s_async_fault ? W25Q64_ERROR : W25Q64_BUSY;
        spi2_bus_release();
        return state;
    }
    cmd_addr(cmd, erase ? CMD_SE : CMD_PP, addr);
    ok = wren();
    if (ok)
    {
        cs_low();
        ok = spi_tx(cmd, 4u) && (erase || spi_tx(buf, len));
        cs_high();
    }
    if (!ok)
    {
        s_async_fault = true;
        spi2_bus_release();
        g_w25q64_status = W25Q64_ERROR;
        return W25Q64_ERROR;
    }
    s_async_start = HAL_GetTick();
    s_async_timeout = erase ? TIMEOUT_ERASE_MS : TIMEOUT_PROGRAM_MS;
    s_async_active = true;
    spi2_bus_release();
    return W25Q64_OK;
}
w25q64_status_t w25q64_erase_start(uint32_t addr)
{
    return async_start(addr, 0, 0, true);
}
w25q64_status_t w25q64_program_start(uint32_t addr, const uint8_t *buf, uint16_t len)
{
    return async_start(addr, buf, len, false);
}
int w25q64_poll(void)
{
    const uint8_t cmd = CMD_RDSR1;
    uint8_t sr = 0xFF;
    bool ok;
    int result = 1;
    if (!spi2_bus_try_acquire(SPI2_BUS_OWNER_FLASH)) return 1;
    if (s_async_fault) result = -1;
    else if (!s_async_active) result = 0;
    else {
        cs_low();
        ok = spi_tx(&cmd, 1) && spi_rx(&sr, 1);
        cs_high();
        if (ok && !(sr & SR1_BUSY)) {
            s_async_active = false; g_w25q64_status = W25Q64_OK; result = 0;
        } else if (!ok || (uint32_t)(HAL_GetTick() - s_async_start) >= s_async_timeout) {
            s_async_active = false; s_async_fault = true;
            g_w25q64_status = W25Q64_ERROR; result = -1;
        }
    }
    spi2_bus_release();
    return result;
}
