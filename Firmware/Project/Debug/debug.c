/********************************** (C) COPYRIGHT *******************************
 * Delay compatibility wrappers and newlib printf retarget.
 * UART hardware and buffering live in Peripheral/USART/usart_async.c.
 *******************************************************************************/
#include "debug.h"
#include "time_api.h"
#include "usart_async.h"
#include <stdarg.h>
#include <stddef.h>

void Delay_Init(void)
{
    TIME_Init();
}

void Delay_Us(uint32_t n)
{
    TIME_DelayUs(n);
}

void Delay_Ms(uint32_t n)
{
    TIME_DelayMs(n);
}

uint8_t Debug_Flush(uint32_t timeout_us)
{
    return USART1_Async_Flush(timeout_us);
}

__attribute__((used))
int _write(int fd, char *buf, int size)
{
    (void)fd;

    if((buf != 0) && (size > 0))
        (void)USART1_Async_Write((const uint8_t *)buf, (uint16_t)size);

    /* Keep stdio non-fatal if a burst exceeds the bounded 256-byte TX ring.
     * The UART driver counts dropped bytes for diagnostics. */
    return size;
}

__attribute__((used))
void *_sbrk(ptrdiff_t incr)
{
    extern char _end[];
    extern char _heap_end[];
    static char *curbrk = _end;

    if((curbrk + incr < _end) || (curbrk + incr > _heap_end))
        return (void *)-1;

    curbrk += incr;
    return curbrk - incr;
}

/*******************************************************************************
 * 轻量 C 运行库补充（Flash 瘦身专用）
 *
 * CH32X035 没有 FPU、只有 62 KiB Flash，WCH 工具链默认会链进：
 *   - libc 的 rand()：MiaoUI 的溶解动画用了它，而 newlib 的 rand 引用 assert
 *     → assert 又引用 fprintf → 连带 nano-vfprintf / reent stdio / malloc /
 *     系统调用桩等一整条链（实测约 4.5 KiB）。这里用 xorshift 顶掉。
 *   - 两套 printf：WCH libprintfloat.a 的浮点 printf（~2 KiB）+ newlib 的
 *     nano vfprintf。本项目所有打印都是整型格式，所以直接换成一个纯整数的
 *     小格式化器；浮点显示统一改成“缩放整数 + 定点格式化”。
 *
 * 支持的转换：%% %d %i %u %x %X %c %s
 * 支持的标志：'0'（零填充）、'-'（左对齐）、十进制宽度
 * 长度修饰：'l'/'ll'/'h' 接受但忽略（RV32 上 long 与 int 同为 32 位）
 * 不支持：%f（用定点整数代替）、%p、'*' 宽度、字符串精度
 ******************************************************************************/

/* MiaoUI 溶解特效用的伪随机数（xorshift32）。
 * 目的：避免链接 newlib 的 rand()，它是 stdio/malloc 链的入口。 */
static uint32_t s_rand_state = 0x2545F491u;

int rand(void)
{
    s_rand_state ^= s_rand_state << 13;
    s_rand_state ^= s_rand_state >> 17;
    s_rand_state ^= s_rand_state << 5;
    return (int)(s_rand_state >> 1);
}

void srand(unsigned int seed)
{
    s_rand_state = (seed != 0u) ? (uint32_t)seed : 0x2545F491u;
}

#define MINI_PRINTF_BUF   128u

typedef struct
{
    char *buf;
    size_t size;
    size_t len;
} mini_out_t;

static void mini_putc(mini_out_t *o, char c)
{
    if((o->buf != 0) && ((o->len + 1u) < o->size))
        o->buf[o->len] = c;
    o->len++;
}

static void mini_fill(mini_out_t *o, char c, size_t n)
{
    while(n != 0u)
    {
        mini_putc(o, c);
        n--;
    }
}

static void mini_uint(mini_out_t *o, uint32_t v, uint32_t base, uint8_t upper,
                      int width, uint8_t zero, uint8_t left)
{
    char tmp[11];
    size_t n = 0u;
    size_t pad;

    if(v == 0u)
    {
        tmp[n++] = '0';
    }
    while(v != 0u)
    {
        uint32_t d = v % base;

        v /= base;
        tmp[n++] = (char)((d < 10u) ? (uint32_t)('0' + d)
                                    : ((upper ? (uint32_t)'A' : (uint32_t)'a') + (d - 10u)));
    }

    pad = ((width > 0) && ((size_t)width > n)) ? ((size_t)width - n) : 0u;
    if(!left)
        mini_fill(o, zero ? '0' : ' ', pad);
    while(n != 0u)
        mini_putc(o, tmp[--n]);
    if(left)
        mini_fill(o, ' ', pad);
}

static void mini_str(mini_out_t *o, const char *s, int width, uint8_t left)
{
    size_t n = 0u;
    size_t pad;

    if(s == 0)
        s = "(null)";
    while(s[n] != '\0')
        n++;

    pad = ((width > 0) && ((size_t)width > n)) ? ((size_t)width - n) : 0u;
    if(!left)
        mini_fill(o, ' ', pad);
    for(size_t i = 0u; i < n; i++)
        mini_putc(o, s[i]);
    if(left)
        mini_fill(o, ' ', pad);
}

int vsnprintf(char *dst, size_t size, const char *fmt, va_list ap)
{
    mini_out_t o;

    o.buf = dst;
    o.size = size;
    o.len = 0u;

    while((fmt != 0) && (*fmt != '\0'))
    {
        uint8_t zero = 0u;
        uint8_t left = 0u;
        int width = 0;

        if(*fmt != '%')
        {
            mini_putc(&o, *fmt++);
            continue;
        }

        fmt++;
        if(*fmt == '%')
        {
            mini_putc(&o, '%');
            fmt++;
            continue;
        }

        /* 标志 */
        for(;;)
        {
            if(*fmt == '0')
            {
                zero = 1u;
                fmt++;
            }
            else if(*fmt == '-')
            {
                left = 1u;
                fmt++;
            }
            else
            {
                break;
            }
        }

        /* 宽度 */
        while((*fmt >= '0') && (*fmt <= '9'))
        {
            width = (width * 10) + (int)(*fmt - '0');
            fmt++;
        }

        /* 精度：只接受并跳过（本工程不使用字符串精度） */
        if(*fmt == '.')
        {
            fmt++;
            while((*fmt >= '0') && (*fmt <= '9'))
                fmt++;
        }

        /* 长度修饰：RV32 上 long/long long 均按 32 位处理 */
        while((*fmt == 'l') || (*fmt == 'h'))
            fmt++;

        switch(*fmt)
        {
        case 'd':
        case 'i':
        {
            int v = va_arg(ap, int);
            uint8_t neg = 0u;
            uint32_t uv;

            if(v < 0)
            {
                neg = 1u;
                uv = (uint32_t)0 - (uint32_t)v;
                mini_putc(&o, '-');
            }
            else
            {
                uv = (uint32_t)v;
            }
            mini_uint(&o, uv, 10u, 0u, (neg && (width > 0)) ? (width - 1) : width, zero, left);
            break;
        }

        case 'u':
            mini_uint(&o, va_arg(ap, unsigned int), 10u, 0u, width, zero, left);
            break;

        case 'x':
            mini_uint(&o, va_arg(ap, unsigned int), 16u, 0u, width, zero, left);
            break;

        case 'X':
            mini_uint(&o, va_arg(ap, unsigned int), 16u, 1u, width, zero, left);
            break;

        case 'c':
            mini_putc(&o, (char)va_arg(ap, int));
            break;

        case 's':
            mini_str(&o, va_arg(ap, const char *), width, left);
            break;

        case '\0':
            goto done;   /* 格式串以 '%' 结尾，直接收尾 */

        default:
            /* 未知转换：原样输出，便于发现误用 */
            mini_putc(&o, '%');
            mini_putc(&o, *fmt);
            break;
        }

        if(*fmt != '\0')
            fmt++;
    }

done:
    if((dst != 0) && (size != 0u))
        dst[(o.len < size) ? o.len : (size - 1u)] = '\0';

    return (int)o.len;
}

int snprintf(char *dst, size_t size, const char *fmt, ...)
{
    va_list ap;
    int ret;

    va_start(ap, fmt);
    ret = vsnprintf(dst, size, fmt, ap);
    va_end(ap);

    return ret;
}

int printf(const char *fmt, ...)
{
    char buf[MINI_PRINTF_BUF];
    va_list ap;
    int len;
    int sent;

    va_start(ap, fmt);
    len = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    /* 与 _write 相同的输出路径；超出缓冲的部分被截断（单条日志请 <127 字符） */
    sent = (len < (int)sizeof(buf)) ? len : ((int)sizeof(buf) - 1);
    if(sent > 0)
        (void)_write(1, buf, sent);

    return len;
}

/* GCC 会把 printf("literal\n") 折叠成 puts()，这里自己提供，
 * 免得为了一条常量日志把 newlib/printfloat 的 stdio 再拉进来。 */
int puts(const char *s)
{
    size_t n = 0u;

    if(s != 0)
    {
        while(s[n] != '\0')
            n++;
        if(n != 0u)
            (void)_write(1, (char *)s, (int)n);
    }
    (void)_write(1, (char *)"\r\n", 2);

    return (int)n + 1;
}

/*******************************************************************************
 * 复位黑匣子（见 debug.h）
 ******************************************************************************/
#define DBG_BB_MAGIC    0xD06F5A5Au

volatile uint32_t g_dbg_isr[DBG_ISR_COUNT] __attribute__((section(".noinit"), used));
volatile uint32_t g_dbg_isr_snap[DBG_ISR_COUNT] __attribute__((section(".noinit"), used));
volatile uint32_t g_dbg_tick __attribute__((section(".noinit"), used));
volatile uint32_t g_dbg_tick_snap __attribute__((section(".noinit"), used));
volatile uint32_t g_dbg_fault_count __attribute__((section(".noinit"), used));
volatile uint32_t g_dbg_i2c_star1 __attribute__((section(".noinit"), used));
volatile uint32_t g_dbg_i2c_star2 __attribute__((section(".noinit"), used));
volatile uint32_t g_dbg_i2c_state __attribute__((section(".noinit"), used));
volatile uint32_t g_dbg_i2c_aborts __attribute__((section(".noinit"), used));
static volatile uint32_t s_bb_magic __attribute__((section(".noinit"), used));

void DBG_RecordI2cStorm(uint16_t star1, uint16_t star2, uint32_t state)
{
    g_dbg_i2c_star1 = (uint32_t)star1;
    g_dbg_i2c_star2 = (uint32_t)star2;
    g_dbg_i2c_state = state;
    g_dbg_i2c_aborts++;
}

void DBG_TickHook(void)
{
    uint32_t i;

    g_dbg_tick++;

    /* 每 ~1 s 把中断计数快照一次：复位后可算出“最后 1 秒的中断量”。 */
    if((g_dbg_tick & 0x3FFu) == 0u)
    {
        for(i = 0u; i < (uint32_t)DBG_ISR_COUNT; i++)
            g_dbg_isr_snap[i] = g_dbg_isr[i];
        g_dbg_tick_snap = g_dbg_tick;
    }
}

void DBG_BlackboxReport(void)
{
    if(s_bb_magic != DBG_BB_MAGIC)
        return;

    printf("[DBG] bb: uptime=%lu ms last_snap=%lu fault=%lu\r\n",
           (unsigned long)g_dbg_tick, (unsigned long)g_dbg_tick_snap,
           (unsigned long)g_dbg_fault_count);
    printf("[DBG] bb isr1s: pd=%lu spi=%lu ev=%lu er=%lu\r\n",
           (unsigned long)(g_dbg_isr[DBG_ISR_USBPD] - g_dbg_isr_snap[DBG_ISR_USBPD]),
           (unsigned long)(g_dbg_isr[DBG_ISR_SPI_TX] - g_dbg_isr_snap[DBG_ISR_SPI_TX]),
           (unsigned long)(g_dbg_isr[DBG_ISR_I2C_EV] - g_dbg_isr_snap[DBG_ISR_I2C_EV]),
           (unsigned long)(g_dbg_isr[DBG_ISR_I2C_ER] - g_dbg_isr_snap[DBG_ISR_I2C_ER]));
    printf("[DBG] bb isr1s: i2ctx=%lu i2crx=%lu uart=%lu/%lu\r\n",
           (unsigned long)(g_dbg_isr[DBG_ISR_I2C_TX] - g_dbg_isr_snap[DBG_ISR_I2C_TX]),
           (unsigned long)(g_dbg_isr[DBG_ISR_I2C_RX] - g_dbg_isr_snap[DBG_ISR_I2C_RX]),
           (unsigned long)(g_dbg_isr[DBG_ISR_UART_TX] - g_dbg_isr_snap[DBG_ISR_UART_TX]),
           (unsigned long)(g_dbg_isr[DBG_ISR_UART_RX] - g_dbg_isr_snap[DBG_ISR_UART_RX]));

    if(g_dbg_i2c_aborts != 0u)
        printf("[DBG] bb i2c: aborts=%lu star1=%04lX star2=%04lX state=%lu\r\n",
               (unsigned long)g_dbg_i2c_aborts, (unsigned long)g_dbg_i2c_star1,
               (unsigned long)g_dbg_i2c_star2, (unsigned long)g_dbg_i2c_state);
}

void DBG_BlackboxReset(void)
{
    uint32_t i;

    for(i = 0u; i < (uint32_t)DBG_ISR_COUNT; i++)
    {
        g_dbg_isr[i] = 0u;
        g_dbg_isr_snap[i] = 0u;
    }
    g_dbg_tick = 0u;
    g_dbg_tick_snap = 0u;
    g_dbg_fault_count = 0u;
    g_dbg_i2c_star1 = 0u;
    g_dbg_i2c_star2 = 0u;
    g_dbg_i2c_state = 0u;
    g_dbg_i2c_aborts = 0u;
    s_bb_magic = DBG_BB_MAGIC;
}
