/*
 * app_load.c — Subsistema CARGA DC (emulador de perfiles dinamicos) para la STM32F303K8.
 * Mismo patron que app_pv_M.c:  TIM6 @ 10 kHz -> sub_tim_isr() ; telemetria a 100 Hz desde sub_loop().
 *
 *   ISR (10 kHz)      : load_model_fast()  -> slew 0.5 A/ms + filtro + capacitor de entrada
 *   sub_loop (100 Hz) : perfil, FP, eta, nivel, proteccion, heartbeat, publicacion I2C
 *   I2C (ESP32)       : A = 0x13 (lee 30 B), B = 0x23 (lee control / escribe comandos)
 */
#include "app_sub.h"
#include "sub_i2c.h"
#include "load_model.h"
#include "main.h"

extern TIM_HandleTypeDef htim6;

static load_model_t      g_m;
static volatile uint8_t  g_overrun;
static volatile uint32_t g_isr_cycles_max;     /* diagnostico (depurador): ciclos max de la ISR */

/* Comandos recibidos: el callback corre en contexto de interrupcion I2C; solo se anotan aqui y el
 * lazo principal los aplica al modelo (asi el modelo lento tiene un unico "dueno"). */
static volatile uint8_t  g_pend;               /* bit0 = SETMODE, bit1 = EXT */
static volatile uint8_t  g_p_mode;
static volatile uint16_t g_p_a, g_p_b, g_p_ext;

static void on_cmd(uint8_t cmd, uint8_t mode, uint16_t a, uint16_t b)
{
    if (cmd == SUB_CMD_SETMODE) {
        if (mode > 2u) return;
        g_p_mode = mode; g_p_a = a; g_p_b = b;
        g_pend |= 1u;
    } else if (cmd == SUB_CMD_EXT) {
        g_p_ext = a;                           /* V_bus medido por la bateria [0.1 V] */
        g_pend |= 2u;
    }
}

static void publish(void)
{
    float i_in;
    uint8_t ovr;
    __disable_irq();
    i_in = load_model_i_in(&g_m);
    ovr  = g_overrun;
    __enable_irq();

    sub_plant_tel_t pl; cvl_tel_t cv; sub_extra_tel_t ex; sub_ctrl_tel_t ct;
    load_model_fill_tel(&g_m, i_in, ovr, &pl, &cv, &ex, &ct);
    sub_i2c_publish(&pl, &cv, &ex, &ct);
}

void sub_init(void)
{
    load_params_t prm;
    load_params_default(&prm);
    load_model_init(&g_m, &prm, LOAD_SCN_RES);

    /* Contador de ciclos para medir la ISR (presupuesto: 6400 ciclos) */
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CYCCNT = 0u;
    DWT->CTRL  |= DWT_CTRL_CYCCNTENA_Msk;

    sub_i2c_init(SUB_ADDR_A_LOAD, SUB_ADDR_B_LOAD, on_cmd);
    publish();                                  /* primera trama valida antes de arrancar */
    HAL_TIM_Base_Start_IT(&htim6);
}

/* ISR de 10 kHz */
void sub_tim_isr(void)
{
    uint32_t t0 = DWT->CYCCNT;

    load_model_fast(&g_m);

    uint32_t dt = DWT->CYCCNT - t0;
    if (dt > g_isr_cycles_max) g_isr_cycles_max = dt;
    if (dt > (SystemCoreClock / (uint32_t)SUB_CTRL_HZ)) g_overrun = 1u;
}

void sub_loop(void)
{
    static uint32_t t_tel = 0, t_led = 0, t_init = 0;
    uint32_t now = HAL_GetTick();
    if (!t_init) { t_init = 1u; t_tel = now; t_led = now; }

    if (now - t_tel >= 10u) {                   /* 100 Hz */
        uint32_t dt_ms = now - t_tel;
        t_tel = now;

        uint8_t pend, mode; uint16_t a, b, ext;
        __disable_irq();
        pend = g_pend; g_pend = 0u;
        mode = g_p_mode; a = g_p_a; b = g_p_b; ext = g_p_ext;
        __enable_irq();
        if (pend & 1u) load_model_set_mode(&g_m, mode, a, b);
        if (pend & 2u) load_model_set_vbus_dV(&g_m, ext);

        load_model_set_hb_lost(&g_m, (sub_i2c_ms_since_cmd() > SUB_HB_TIMEOUT_MS) ? 1u : 0u);
        load_model_slow(&g_m, dt_ms);
        publish();
    }
    if (now - t_led >= 500u) {
        t_led = now;
        HAL_GPIO_TogglePin(GPIOB, GPIO_PIN_3);
    }
}
