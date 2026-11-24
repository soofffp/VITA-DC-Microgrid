#include "sub_i2c.h"
#include "app_sub.h"
#include "main.h"
#include <string.h>

extern I2C_HandleTypeDef hi2c1;
extern TIM_HandleTypeDef htim6;

static uint8_t           g_addrA, g_addrB;
static sub_cmd_cb_t      g_cb;
static sub_block_a_t     g_blk_a;           /* lo que se lee en la direccion A */
static sub_ctrl_tel_t    g_blk_b;           /* lo que se lee en la direccion B */
static uint8_t           g_rx[SUB_CMD_LEN];
static volatile uint8_t  g_addr_sel;
static volatile uint8_t  g_busy;
static volatile uint32_t g_busy_t0;
static volatile uint32_t g_last_cmd_ms;

void sub_i2c_init(uint8_t addrA, uint8_t addrB, sub_cmd_cb_t cb)
{
    g_addrA = addrA; g_addrB = addrB; g_cb = cb;
    g_last_cmd_ms = HAL_GetTick();

    hi2c1.Init.OwnAddress1      = (uint32_t)(addrA << 1);   /* no depende de lo que diga CubeMX */
    hi2c1.Init.AddressingMode   = I2C_ADDRESSINGMODE_7BIT;
    hi2c1.Init.DualAddressMode  = I2C_DUALADDRESS_ENABLE;
    hi2c1.Init.OwnAddress2      = (uint32_t)(addrB << 1);
    hi2c1.Init.OwnAddress2Masks = I2C_OA2_NOMASK;
    if (HAL_I2C_Init(&hi2c1) != HAL_OK) {
        Error_Handler();
    }
    HAL_I2C_EnableListen_IT(&hi2c1);
}

void sub_i2c_publish(const sub_plant_tel_t *p, const cvl_tel_t *c, const sub_extra_tel_t *x,
                     const sub_ctrl_tel_t *k)
{
    sub_block_a_t a;
    sub_ctrl_tel_t b;
    a.plant = *p; a.conv = *c; a.extra = *x; b = *k;
    a.plant.checksum = sub_xor((const uint8_t *)&a.plant, sizeof(a.plant) - 1u);
    a.conv.checksum  = sub_xor((const uint8_t *)&a.conv,  sizeof(a.conv)  - 1u);
    a.extra.checksum = sub_xor((const uint8_t *)&a.extra, sizeof(a.extra) - 1u);
    b.checksum       = sub_xor((const uint8_t *)&b,       sizeof(b)       - 1u);

    /* Atomico respecto al inicio de una transaccion; si la bandera se quedara pegada (>20 ms) se ignora. */
    __disable_irq();
    if (!g_busy || (HAL_GetTick() - g_busy_t0) > 20u) {
        memcpy(&g_blk_a, &a, sizeof(a));
        memcpy(&g_blk_b, &b, sizeof(b));
    }
    __enable_irq();
}

uint32_t sub_i2c_ms_since_cmd(void)
{
    return HAL_GetTick() - g_last_cmd_ms;
}

static void handle_cmd(const uint8_t *b)
{
    if (sub_xor(b, SUB_CMD_LEN - 1u) != b[SUB_CMD_LEN - 1u]) return;
    uint16_t a = (uint16_t)(((uint16_t)b[2] << 8) | b[3]);
    uint16_t c = (uint16_t)(((uint16_t)b[4] << 8) | b[5]);
    g_last_cmd_ms = HAL_GetTick();                        /* cualquier comando valido = heartbeat */
    if (b[0] == SUB_CMD_HEARTBEAT) return;
    if (b[0] != SUB_CMD_SETMODE && b[0] != SUB_CMD_EXT) return;
    if (g_cb) g_cb(b[0], b[1], a, c);
}

void HAL_I2C_AddrCallback(I2C_HandleTypeDef *hi2c, uint8_t TransferDirection, uint16_t AddrMatchCode)
{
    g_addr_sel = (uint8_t)(AddrMatchCode >> 1);
    g_busy_t0 = HAL_GetTick();
    g_busy = 1u;
    if (TransferDirection == I2C_DIRECTION_TRANSMIT) {
        HAL_I2C_Slave_Seq_Receive_IT(hi2c, g_rx, SUB_CMD_LEN, I2C_FIRST_AND_LAST_FRAME);
    } else if (g_addr_sel == g_addrB) {
        HAL_I2C_Slave_Seq_Transmit_IT(hi2c, (uint8_t *)&g_blk_b, sizeof(g_blk_b), I2C_FIRST_AND_LAST_FRAME);
    } else {
        HAL_I2C_Slave_Seq_Transmit_IT(hi2c, (uint8_t *)&g_blk_a, sizeof(g_blk_a), I2C_FIRST_AND_LAST_FRAME);
    }
}

void HAL_I2C_SlaveRxCpltCallback(I2C_HandleTypeDef *hi2c)
{
    (void)hi2c;
    if (g_addr_sel == g_addrB) handle_cmd(g_rx);          /* los comandos solo se aceptan en la direccion B */
}

void HAL_I2C_ListenCpltCallback(I2C_HandleTypeDef *hi2c)
{
    g_busy = 0u;
    HAL_I2C_EnableListen_IT(hi2c);
}

void HAL_I2C_ErrorCallback(I2C_HandleTypeDef *hi2c)
{
    g_busy = 0u;
    if (HAL_I2C_GetError(hi2c) != HAL_I2C_ERROR_AF) {
        HAL_I2C_EnableListen_IT(hi2c);
    }
}

#ifndef SUB_NO_TIM_CALLBACK
void HAL_TIM_PeriodElapsedCallback(TIM_HandleTypeDef *htim)
{
    if (htim->Instance == TIM6) {
        sub_tim_isr();
    }
}
#endif
