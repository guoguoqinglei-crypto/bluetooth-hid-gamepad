#ifndef GP_CONFIG_H
#define GP_CONFIG_H

#include <stdint.h>

#define GP_CONFIG_REPORT_ID       5u
#define GP_CONFIG_SIZE            16u
#define GP_CONFIG_MAGIC           0x47u
#define GP_CONFIG_VERSION         1u
#define GP_CONFIG_CMD_SAVE        1u
#define GP_CONFIG_CMD_DEFAULT     2u
#define GP_CONFIG_STATUS_IDLE     0u
#define GP_CONFIG_STATUS_PENDING  1u
#define GP_CONFIG_STATUS_SAVED    2u
#define GP_CONFIG_STATUS_ERROR    3u

void GPConfig_Init(void);
void GPConfig_Process(void);
uint8_t GPConfig_SetFeature(const uint8_t *data, uint16_t len);
uint16_t GPConfig_GetFeature(uint8_t *out, uint16_t capacity);
uint16_t GPConfig_AdjustAxis(uint8_t axis, uint16_t raw);

#endif /* GP_CONFIG_H */
