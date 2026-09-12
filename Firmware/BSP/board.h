#ifndef __BOARD_H
#define __BOARD_H

#include "main.h"

#ifdef __cplusplus
extern "C" {
#endif

void Board_Init(void);
uint8_t Board_Key1Pressed(void);
uint8_t Board_Key2Pressed(void);

/* Enter the CH32X035 factory USB ISP on the next reset. Never returns. */
void Board_RebootToISP(void) __attribute__((noreturn));

#ifdef __cplusplus
}
#endif

#endif /* __BOARD_H */
