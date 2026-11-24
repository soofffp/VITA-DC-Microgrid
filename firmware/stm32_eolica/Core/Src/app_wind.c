/*
 * app_wind.c — Subsistema EOLICO en una STM32F303K8: modelo emulado (viento -> PMSG -> rectificador -> boost)
 *              + control local (pv_control: MPPT P&O + PI de tension + MPPT/LIMITED/SAFE).
 *
 *   TIM6 @ 10 kHz (sub_tim_isr):  wind_isr_step()  ->  control + planta en el mismo paso
 *   sub_loop (lazo principal):    viento (1 kHz) -> wind_update/wind_supervise; telemetria a 100 Hz; LED PB3
 *   ESP32 (maestro I2C):          0x11 lee [planta|convertidor|extra] (30 B);  0x21 lee el control y escribe comandos
 *
 * Mismo patron que app_pv_M.c. Contrato: app_sub.h. Mapeo de telemetria: wind_model.c (wind_build_tel).
 * El tiempo real de la ISR se lee con el depurador (g_isr_cycles_max).
 */
#include "app_sub.h"
#include "main.h"
#include "sub_link.h"
#include "sub_i2c.h"
#include "wind_model.h"
#include "pv_control.h"
#include <string.h>

extern TIM_HandleTypeDef htim6;

/* Viento: >0 = constante en m/s (bring-up; 8.0 = media anual de La Guajira, del paper).
 * 0 = sintetico: 8 + 3.5 sin(2 pi t/240 s) + turbulencia (sigma 1.2 m/s), saturado a [0, 28]. */
#ifndef WIND_FIXED_MS
#define WIND_FIXED_MS     0.0f
#endif
/* 1 = anade una rafaga > 25 m/s (27 m/s, 8 s) cada 300 s para probar la parada por sobreviento
 * (solo tiene efecto con viento sintetico, WIND_FIXED_MS = 0). */
#ifndef WIND_GUST_ENABLE
#define WIND_GUST_ENABLE  0
#endif
#define WIND_SEED         0x1234ABCDu
#define OVERRUN_HOLD_MS   2000u     /* la bandera de overrun se limpia si no hay otro en este tiempo */

static wind_t         g_w;
static pv_ctrl_t      g_pv;
static wind_profile_t g_prof;
static uint32_t       g_t0_ms;
static uint32_t       g_last_ms;

/* Diagnostico de tiempo de la ISR (mirar con el depurador). Presupuesto = SystemCoreClock / 10 kHz = 6400 ciclos. */
static volatile uint32_t g_isr_cycles_max;
static volatile uint32_t g_isr_cycles_last;
static volatile uint8_t  g_overrun;
static volatile uint32_t g_overrun_tick;

/* Comando del ESP32 (se llama desde la ISR del I2C): solo guarda modo y Pmax; el lazo principal los aplica. */
static void on_cmd(uint8_t cmd, uint8_t mode, uint16_t a, uint16_t b)
{
    (void)b;
    wind_on_cmd(&g_w, cmd, mode, a);
}

static float wind_now(uint32_t now_ms)
{
    if (WIND_FIXED_MS > 0.0f) return WIND_FIXED_MS;
    return wind_profile(&g_prof, (now_ms - g_t0_ms) + (uint32_t)((float)MICRORED_DAY_S * 1000.0f * (MICRORED_DAY_START_H / 24.0f)), WIND_GUST_ENABLE);
}

void sub_init(void)
{
    pv_params_t prm;
    wind_ctrl_params(&prm, 1.0f / SUB_CTRL_HZ);
    pv_ctrl_init(&g_pv, &prm);

    wind_init(&g_w, 1.0f / SUB_CTRL_HZ);
    wind_profile_init(&g_prof, WIND_SEED);
    g_t0_ms   = HAL_GetTick();
    g_last_ms = g_t0_ms;
    wind_update(&g_w, wind_now(g_t0_ms), 0.0f);      /* estado inicial (RUN/CALM) segun el viento de arranque */
    wind_supervise(&g_w, &g_pv);

    /* Contador de ciclos para medir cuanto tarda la ISR */
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CYCCNT = 0u;
    DWT->CTRL  |= DWT_CTRL_CYCCNTENA_Msk;

    sub_i2c_init(SUB_ADDR_A_WIND, SUB_ADDR_B_WIND, on_cmd);
    HAL_TIM_Base_Start_IT(&htim6);
}

/* ISR de 10 kHz: control y planta en el mismo paso (solo integra; sin expf ni sinf). */
void sub_tim_isr(void)
{
    uint32_t t0 = DWT->CYCCNT;

    (void)wind_isr_step(&g_w, &g_pv);

    uint32_t dt = DWT->CYCCNT - t0;
    g_isr_cycles_last = dt;
    if (dt > g_isr_cycles_max) g_isr_cycles_max = dt;
    if (dt > (SystemCoreClock / (uint32_t)SUB_CTRL_HZ)) {
        g_overrun_tick = HAL_GetTick();
        g_overrun = 1u;
    }
}

static void build_telemetry(void)
{
    wind_t    w;
    pv_ctrl_t c;
    uint8_t   ovr;

    __disable_irq();
    memcpy(&w, &g_w, sizeof(w));
    memcpy(&c, &g_pv, sizeof(c));
    ovr = g_overrun;
    __enable_irq();

    sub_plant_tel_t p;
    cvl_tel_t       cv;
    sub_extra_tel_t x;
    sub_ctrl_tel_t  k;
    wind_build_tel(&w, &c, ovr, &p, &cv, &x, &k);
    sub_i2c_publish(&p, &cv, &x, &k);                /* calcula los checksums */
}

void sub_loop(void)
{
    static uint32_t t_tel = 0u, t_led = 0u;
    uint32_t now = HAL_GetTick();

    /* 1 kHz: viento -> frecuencia -> Voc/Req; supervisor (heartbeat, modo, limites de V_ref) */
    if (now != g_last_ms) {
        float dt_s = (float)(now - g_last_ms) * 1.0e-3f;
        g_last_ms = now;

        float vw = wind_now(now);
        uint8_t hb_lost = (sub_i2c_ms_since_cmd() > SUB_HB_TIMEOUT_MS) ? 1u : 0u;

        __disable_irq();                             /* ~150 ciclos: la ISR ve un conjunto coherente */
        g_w.hb_lost = hb_lost;
        wind_update(&g_w, vw, dt_s);
        wind_supervise(&g_w, &g_pv);
        if (g_overrun && (now - g_overrun_tick) > OVERRUN_HOLD_MS) g_overrun = 0u;
        __enable_irq();
    }

    if (now - t_tel >= 10u) {                        /* 100 Hz */
        t_tel = now;
        build_telemetry();
    }
    if (now - t_led >= 500u) {
        t_led = now;
        HAL_GPIO_TogglePin(GPIOB, GPIO_PIN_3);
    }
}
