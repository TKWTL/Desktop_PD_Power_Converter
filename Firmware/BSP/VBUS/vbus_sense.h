#ifndef VBUS_SENSE_H_
#define VBUS_SENSE_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void VBUS_Sense_Init(void);
uint16_t VBUS_Sense_ReadRaw(void);
uint16_t VBUS_Sense_ReadMillivolts(void);

#ifdef __cplusplus
}
#endif

#endif /* VBUS_SENSE_H_ */
