/*
 * app_pv_M.c — UNA sola STM32: planta emulada del PV + control local (MPPT + lazo de tensión).
 *
 *   TIM6 @ 10 kHz:   (V, I) de la planta --> pv_ctrl_step() --> duty --> pv_plant_step()
 *   ESP32 (maestro I2C):  0x10 lee la planta;  0x20 lee el control y escribe la consigna (modo, P_max)
 *
 * Con hardware de potencia, la "planta" se reemplazaría por lecturas reales del ADC y el duty saldría por
 * TIM1 (PWM complementaria con tiempo muerto hacia el gate driver); el lazo de control (pv_control.c)
 * no cambiaría.
 */
#include "app_pv_M.h"
#include "main.h"
#include "pv_link.h"
#include "conv_link.h"
#include "pv_plant.h"
#include "pv_control.h"
#include <math.h>
#include <string.h>

extern I2C_HandleTypeDef hi2c1;
extern TIM_HandleTypeDef htim6;

/* Irradiancia: 0 = día solar de 24 h comprimido en MICRORED_DAY_S segundos (reloj común de la demo);
 *              >0 = valor fijo en W/m2 (útil para el bring-up). */
#ifndef PV_G_FIXED_WM2
#define PV_G_FIXED_WM2  0
#endif
/* Estos dos DEBEN ser iguales a los de sub_link.h (eólica/carga). Para cambiar la duración del día, cambiar ambos. */
#ifndef MICRORED_DAY_S
#define MICRORED_DAY_S        360
#endif
#ifndef MICRORED_DAY_START_H
#define MICRORED_DAY_START_H  6.0f
#endif
#define PV_TAMB_C       25.0f

static pv_plant_t        g_plant;
static pv_ctrl_t         g_pv;
/* Lectura en 0x10: 20 bytes = [planta (10)][convertidor (10)]. Una lectura de 10 bytes sigue funcionando. */
typedef struct __attribute__((packed)) { pvl_plant_tel_t plant; cvl_tel_t conv; } pc_tel_t;
static pc_tel_t          g_tel_plant;
static pvl_ctrl_tel_t    g_tel_ctrl;
static uint8_t           g_rx[PVL_CMD_LEN];
static volatile uint8_t  g_i2c_busy;          /* 1 mientras el ESP esta leyendo/escribiendo (no tocar buffers) */
static volatile uint32_t g_i2c_busy_t0;       /* HAL_GetTick() del inicio de la transaccion                    */
static volatile uint8_t  g_addr_sel;          /* dirección I2C (7 bit) con la que hablan en este momento */
static float             g_g_last = -1.0f;

/* Diagnóstico de tiempo de la ISR (se puede mirar con el depurador). Presupuesto = 100 us. */
static volatile uint32_t g_isr_cycles_max;
static volatile uint8_t  g_overrun;

void pvM_init(void)
{
    pv_params_t prm;
    pv_params_default(&prm);
    prm.ts_ctrl = 1.0f / PVL_CTRL_HZ;
    pv_ctrl_init(&g_pv, &prm);

    pv_plant_init(&g_plant, 1.0f / PVL_CTRL_HZ);
    float g0 = (PV_G_FIXED_WM2 > 0) ? (float)PV_G_FIXED_WM2 : 1.0f;
    pv_plant_set_irradiance(&g_plant, g0, PV_TAMB_C);
    g_g_last = g0;

    /* Contador de ciclos para medir cuánto tarda la ISR */
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CYCCNT = 0u;
    DWT->CTRL  |= DWT_CTRL_CYCCNTENA_Msk;

    /* Segunda dirección I2C (0x20) en la misma periférica; la primera (0x10) viene de CubeMX. */
    hi2c1.Init.DualAddressMode = I2C_DUALADDRESS_ENABLE;
    hi2c1.Init.OwnAddress2     = (uint32_t)(PVL_I2C_ADDR_C << 1);
    hi2c1.Init.OwnAddress2Masks = I2C_OA2_NOMASK;
    if (HAL_I2C_Init(&hi2c1) != HAL_OK) {
        Error_Handler();
    }

    HAL_I2C_EnableListen_IT(&hi2c1);
    HAL_TIM_Base_Start_IT(&htim6);
}

/* ISR de 10 kHz: control y planta en el mismo paso. */
void pvM_tim_isr(void)
{
    uint32_t t0 = DWT->CYCCNT;

    float d = pv_ctrl_step(&g_pv, g_plant.v, g_plant.ipv);   /* mide (V, I) del paso anterior */
    pv_plant_step(&g_plant, d);                              /* aplica el duty a la planta     */

    uint32_t dt = DWT->CYCCNT - t0;
    if (dt > g_isr_cycles_max) g_isr_cycles_max = dt;
    if (dt > (SystemCoreClock / (uint32_t)PVL_CTRL_HZ)) g_overrun = 1u;
}

static void update_irradiance(uint32_t now_ms)
{
    float g;
    if (PV_G_FIXED_WM2 > 0) {
        g = (float)PV_G_FIXED_WM2;
    } else {
        /* hora simulada h en [0,24): sol de 6 h a 18 h (seno), noche con 2 W/m2 para no apagar el modelo */
        const uint32_t day_ms = (uint32_t)MICRORED_DAY_S * 1000u;
        uint32_t t = (now_ms + (uint32_t)((float)day_ms * (MICRORED_DAY_START_H / 24.0f))) % day_ms;
        float h = 24.0f * (float)t / (float)day_ms;
        g = (h > 6.0f && h < 18.0f) ? 1000.0f * sinf(3.14159265f * (h - 6.0f) / 12.0f) : 0.0f;
        if (g < 2.0f) g = 2.0f;
    }
    if (fabsf(g - g_g_last) > 0.5f) {
        __disable_irq();
        pv_plant_set_irradiance(&g_plant, g, PV_TAMB_C);
        __enable_irq();
        g_g_last = g;
    }
}

static void build_telemetry(void)
{
    float v, i, vref, duty, p, pmax, il, vbus;
    uint8_t mode, ovr;

    __disable_irq();
    v = g_plant.v; i = g_plant.ipv;
    vref = pv_ctrl_vref(&g_pv); duty = pv_ctrl_duty(&g_pv); p = pv_ctrl_power(&g_pv);
    pmax = g_pv.p_max; mode = (uint8_t)g_pv.mode; ovr = g_overrun;
    il = g_plant.iL; vbus = g_plant.Vbus;
    __enable_irq();

    pvl_plant_tel_t tp;
    tp.voltage_mV = (uint16_t)(v * 1000.0f);
    tp.current_mA = (uint16_t)(i * 1000.0f);
    tp.power_mW   = (uint32_t)(v * i * 1000.0f);
    tp.status     = ovr ? 3u : 0u;                 /* 0 normal, 3 = la ISR se pasó de 100 us */
    tp.checksum   = pvl_xor((const uint8_t *)&tp, sizeof(tp) - 1u);

    pvl_ctrl_tel_t tc;
    float pc = p * 100.0f;
    tc.vref_mV  = (uint16_t)(vref * 1000.0f);
    tc.duty_1e4 = (uint16_t)(duty * 10000.0f);
    tc.p_cW     = (uint16_t)((pc > 65535.0f) ? 65535.0f : (pc < 0.0f ? 0.0f : pc));
    tc.pmax_dW  = (uint16_t)(pmax * 10.0f);
    tc.mode     = mode;
    tc.checksum = pvl_xor((const uint8_t *)&tc, sizeof(tc) - 1u);

    /* Convertidor: el boost promediado entrega al bus (1-D)*iL a V_bus. */
    cvl_tel_t tv;
    float ibus = (1.0f - duty) * il;
    if (ibus < 0.0f) ibus = 0.0f;
    uint8_t fl = 0;
    if (duty >= g_pv.p.d_max - 1.0e-4f)        fl |= CVL_F_DUTY_MAX;
    if (duty <= g_pv.p.d_min + 1.0e-4f)        fl |= CVL_F_DUTY_MIN;
    if (mode == (uint8_t)PV_MODE_LIMITED && p > pmax * (1.0f - g_pv.p.lim_band)) fl |= CVL_F_LIMITING;
    if (vref <= g_pv.p.vref_min + 1.0e-3f || vref >= g_pv.p.vref_max - 1.0e-3f)  fl |= CVL_F_VREF_LIMIT;
    if (ovr)                                   fl |= CVL_F_OVERRUN;
    if (mode == (uint8_t)PV_MODE_SAFE)         fl |= CVL_F_SAFE;
    tv.vin_mV   = (uint16_t)(v * 1000.0f);
    tv.vbus_mV  = (uint16_t)(vbus * 1000.0f);
    tv.ibus_mA  = (uint16_t)(ibus * 1000.0f);
    tv.duty_1e4 = (uint16_t)(duty * 10000.0f);
    tv.flags    = fl;
    tv.checksum = pvl_xor((const uint8_t *)&tv, sizeof(tv) - 1u);

    pc_tel_t pcq;
    pcq.plant = tp;
    pcq.conv  = tv;

    /* No reescribir los buffers mientras el ESP los esta leyendo (evita lecturas "rotas" con checksum malo).
     * Con las IRQ deshabilitadas la comprobacion y la copia son atomicas respecto a la ISR del I2C.
     * Si la bandera quedara pegada (>20 ms), se ignora para no congelar la telemetria. */
    __disable_irq();
    if (!g_i2c_busy || (HAL_GetTick() - g_i2c_busy_t0) > 20u) {
        memcpy(&g_tel_plant, &pcq, sizeof(pcq));
        memcpy(&g_tel_ctrl,  &tc, sizeof(tc));
    }
    __enable_irq();
}

void pvM_loop(void)
{
    static uint32_t t_tel = 0, t_led = 0;
    uint32_t now = HAL_GetTick();

    if (now - t_tel >= 10u) {                  /* 100 Hz */
        t_tel = now;
        update_irradiance(now);
        build_telemetry();
    }
    if (now - t_led >= 500u) {
        t_led = now;
        HAL_GPIO_TogglePin(GPIOB, GPIO_PIN_3);
    }
}

/* Comando del ESP32 a 0x20: [CMD, modo, pmax_dW_hi, pmax_dW_lo, xor]. Se descarta si algo no cuadra. */
static void handle_cmd(const uint8_t *b)
{
    if (pvl_xor(b, PVL_CMD_LEN - 1u) != b[PVL_CMD_LEN - 1u]) return;
    if (b[0] != PVL_CMD_SETMODE) return;
    if (b[1] > (uint8_t)PV_MODE_SAFE) return;
    uint16_t pmax_dW = (uint16_t)(((uint16_t)b[2] << 8) | b[3]);
    pv_ctrl_set_mode(&g_pv, (pv_mode_t)b[1], (float)pmax_dW * 0.1f);
}

/* ------------------------------------------------------------------------------------------ */
/* Esclavo I2C por interrupciones (modo "listen"), dos direcciones                              */
/* ------------------------------------------------------------------------------------------ */
void HAL_I2C_AddrCallback(I2C_HandleTypeDef *hi2c, uint8_t TransferDirection, uint16_t AddrMatchCode)
{
    g_addr_sel = (uint8_t)(AddrMatchCode >> 1);             /* el HAL entrega la dirección desplazada */
    g_i2c_busy_t0 = HAL_GetTick();
    g_i2c_busy = 1u;
    if (TransferDirection == I2C_DIRECTION_TRANSMIT) {      /* el maestro escribe */
        HAL_I2C_Slave_Seq_Receive_IT(hi2c, g_rx, PVL_CMD_LEN, I2C_FIRST_AND_LAST_FRAME);
    } else if (g_addr_sel == PVL_I2C_ADDR_C) {              /* el maestro lee el control */
        HAL_I2C_Slave_Seq_Transmit_IT(hi2c, (uint8_t *)&g_tel_ctrl, sizeof(g_tel_ctrl), I2C_FIRST_AND_LAST_FRAME);
    } else {                                                /* el maestro lee la planta */
        HAL_I2C_Slave_Seq_Transmit_IT(hi2c, (uint8_t *)&g_tel_plant, sizeof(g_tel_plant), I2C_FIRST_AND_LAST_FRAME);
    }
}

void HAL_I2C_SlaveRxCpltCallback(I2C_HandleTypeDef *hi2c)
{
    (void)hi2c;
    if (g_addr_sel == PVL_I2C_ADDR_C) {                     /* la consigna solo se acepta en 0x20 */
        handle_cmd(g_rx);
    }
}

void HAL_I2C_ListenCpltCallback(I2C_HandleTypeDef *hi2c)
{
    g_i2c_busy = 0u;
    HAL_I2C_EnableListen_IT(hi2c);
}

void HAL_I2C_ErrorCallback(I2C_HandleTypeDef *hi2c)
{
    g_i2c_busy = 0u;
    if (HAL_I2C_GetError(hi2c) != HAL_I2C_ERROR_AF) {
        HAL_I2C_EnableListen_IT(hi2c);
    }
}

#ifndef PV_NO_TIM_CALLBACK
void HAL_TIM_PeriodElapsedCallback(TIM_HandleTypeDef *htim)
{
    if (htim->Instance == TIM6) {
        pvM_tim_isr();
    }
}
#endif
