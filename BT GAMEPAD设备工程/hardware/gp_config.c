#include "gp_config.h"

#include <string.h>
#include <stm32f10x.h>
#include <FreeRTOS.h>
#include <task.h>
#include <joystick.h>

/* 配置页与最后一页 0x0803F800 的蓝牙配对信息分开。 */
#define GP_CFG_FLASH_ADDR     0x0803F000u
#define GP_CFG_RECORD_MAGIC   0x47504346u   /* "GPCF" */

typedef struct
{
    uint32_t magic;
    uint32_t sequence;
    uint8_t  data[GP_CONFIG_SIZE];
    uint32_t crc;
} gp_record_t;

static uint8_t s_data[GP_CONFIG_SIZE];
static volatile uint8_t s_savePending;
static uint8_t s_status;
static uint32_t s_sequence;

static uint32_t crc32(const uint8_t *data, uint16_t len)
{
    uint32_t crc = 0xFFFFFFFFu;
    uint16_t i;
    uint8_t bit;
    for (i = 0u; i < len; ++i)
    {
        crc ^= data[i];
        for (bit = 0u; bit < 8u; ++bit)
            crc = (crc >> 1) ^ ((crc & 1u) ? 0xEDB88320u : 0u);
    }
    return ~crc;
}

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void wr16(uint8_t *p, uint16_t value)
{
    p[0] = (uint8_t)value; p[1] = (uint8_t)(value >> 8);
}

static void wr32(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)value; p[1] = (uint8_t)(value >> 8);
    p[2] = (uint8_t)(value >> 16); p[3] = (uint8_t)(value >> 24);
}

static void seal(uint8_t *data)
{
    wr32(&data[12], 0u); wr32(&data[12], crc32(data, 12u));
}

static void set_defaults(void)
{
    memset(s_data, 0, sizeof(s_data));
    s_data[0] = GP_CONFIG_MAGIC; s_data[1] = GP_CONFIG_VERSION;
    s_data[4] = 100u; s_data[5] = 100u;
    wr16(&s_data[6], 0u); wr16(&s_data[8], 0u);
    seal(s_data);
}

static uint8_t valid(const uint8_t *data)
{
    return (data[0] == GP_CONFIG_MAGIC) && (data[1] == GP_CONFIG_VERSION) &&
           (rd32(&data[12]) == crc32(data, 12u)) &&
           (data[4] >= 25u) && (data[4] <= 200u) &&
           (data[5] >= 25u) && (data[5] <= 200u) &&
           (rd16(&data[6]) <= 24000u) && (rd16(&data[8]) <= 24000u);
}

static uint8_t record_valid(const gp_record_t *record)
{
    const uint32_t crc = crc32((const uint8_t *)&record->sequence,
                               (uint16_t)(sizeof(record->sequence) + GP_CONFIG_SIZE));
    return (record->magic == GP_CFG_RECORD_MAGIC) && (record->crc == crc) && valid(record->data);
}

void GPConfig_Init(void)
{
    const gp_record_t *record = (const gp_record_t *)GP_CFG_FLASH_ADDR;
    set_defaults();
    if (record_valid(record) != 0u)
    {
        memcpy(s_data, record->data, GP_CONFIG_SIZE);
        s_data[2] = 0u; s_data[3] = GP_CONFIG_STATUS_SAVED;
        s_sequence = record->sequence;
        s_status = GP_CONFIG_STATUS_SAVED;
        seal(s_data);
    }
}

uint8_t GPConfig_SetFeature(const uint8_t *data, uint16_t len)
{
    if ((data == 0) || (len < GP_CONFIG_SIZE) || (valid(data) == 0u) ||
        ((data[2] != GP_CONFIG_CMD_SAVE) && (data[2] != GP_CONFIG_CMD_DEFAULT)))
    {
        return 0u;
    }
    if (data[2] == GP_CONFIG_CMD_DEFAULT) { set_defaults(); }
    else { memcpy(s_data, data, GP_CONFIG_SIZE); s_data[2] = 0u; }
    s_status = GP_CONFIG_STATUS_PENDING;
    s_data[3] = s_status; seal(s_data);
    s_savePending = 1u;                 /* 由 BT_Task 在控制请求返回后保存 */
    return 1u;
}

uint16_t GPConfig_GetFeature(uint8_t *out, uint16_t capacity)
{
    if ((out == 0) || (capacity < GP_CONFIG_SIZE + 1u)) { return 0u; }
    out[0] = GP_CONFIG_REPORT_ID;
    memcpy(&out[1], s_data, GP_CONFIG_SIZE);
    out[1 + 2] = 0u; out[1 + 3] = s_status;
    seal(&out[1]);
    return GP_CONFIG_SIZE + 1u;
}

void GPConfig_Process(void)
{
    gp_record_t record;
    const uint32_t *words = (const uint32_t *)&record;
    uint32_t i;
    uint8_t ok = 1u;

    if (s_savePending == 0u) { return; }
    s_savePending = 0u;

    memset(&record, 0xFF, sizeof(record));
    record.magic = GP_CFG_RECORD_MAGIC;
    record.sequence = s_sequence + 1u;
    memcpy(record.data, s_data, GP_CONFIG_SIZE);
    record.data[2] = 0u; record.data[3] = GP_CONFIG_STATUS_SAVED; seal(record.data);
    record.crc = crc32((const uint8_t *)&record.sequence,
                       (uint16_t)(sizeof(record.sequence) + GP_CONFIG_SIZE));

    taskENTER_CRITICAL();             /* 不与配对信息 Flash 操作交叉 */
    FLASH_Unlock();
    FLASH_ClearFlag(FLASH_FLAG_EOP | FLASH_FLAG_PGERR | FLASH_FLAG_WRPRTERR);
    if (FLASH_ErasePage(GP_CFG_FLASH_ADDR) != FLASH_COMPLETE) { ok = 0u; }
    for (i = 0u; (ok != 0u) && (i < sizeof(record) / 4u); ++i)
    {
        if (FLASH_ProgramWord(GP_CFG_FLASH_ADDR + i * 4u, words[i]) != FLASH_COMPLETE) ok = 0u;
    }
    FLASH_Lock();
    taskEXIT_CRITICAL();

    if ((ok != 0u) && (memcmp((const void *)GP_CFG_FLASH_ADDR, &record, sizeof(record)) == 0))
    {
        s_sequence = record.sequence; s_status = GP_CONFIG_STATUS_SAVED;
    }
    else { s_status = GP_CONFIG_STATUS_ERROR; }
    s_data[3] = s_status; seal(s_data);
}

static uint8_t stick_default(uint8_t stick)
{
    return (s_data[4u + stick] == 100u) && (rd16(&s_data[6u + stick * 2u]) == 0u);
}

uint16_t GPConfig_AdjustAxis(uint8_t axis, uint16_t raw)
{
    const uint8_t stick = (axis >= JOYSTICK_RX) ? 1u : 0u;
    const int32_t sensitivity = s_data[4u + stick];
    const int32_t deadzone = rd16(&s_data[6u + stick * 2u]);
    const int32_t mapped = Joystick_MapRawValue(axis, raw);
    int32_t value = mapped;
    int32_t magnitude;
    const int32_t range = 32767 - deadzone;

    if (stick_default(stick) != 0u)
    {
        return (uint16_t)((uint32_t)raw * 16u); /* 默认路径与旧固件逐字节一致 */
    }
    magnitude = (mapped < 0) ? -mapped : mapped;
    if ((magnitude <= deadzone) || (range <= 0)) { value = 0; }
    else
    {
        value = ((magnitude - deadzone) * 32767) / range;
        value = (value * sensitivity) / 100;
        if (value > 32767) { value = 32767; }
        if (mapped < 0) { value = -value; }
    }
    value += 32768;
    if (value < 0) { value = 0; }
    if (value > 65535) { value = 65535; }
    return (uint16_t)value;
}
