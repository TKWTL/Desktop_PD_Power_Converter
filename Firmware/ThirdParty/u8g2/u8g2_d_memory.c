/* u8g2_d_memory.c - trimmed full-frame allocator for SH1107 TK078F288 80x128 native panel. */
#include "u8g2.h"

uint8_t *u8g2_m_10_16_f(uint8_t *page_cnt)
{
#ifdef U8G2_USE_DYNAMIC_ALLOC
    *page_cnt = 16U;
    return 0;
#else
    static uint8_t buf[1280]; /* 80 * 128 / 8 */
    *page_cnt = 16U;
    return buf;
#endif
}
