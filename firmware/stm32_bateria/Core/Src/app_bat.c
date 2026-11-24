/*
 * app_bat.c — Subsistema BATERIA en una STM32F303K8: pack 10s5p emulado + convertidor bidireccional
 *             + capacitor del bus + control local (V_bus exterior / corriente interior / limites / derating).
 *
 *   TIM6 @ 10 kHz :  sub_tim_isr() -> bat_step()        (medicion -> PI de V_bus -> PI de corriente -> duty -> planta)
 *   lazo principal:  sub_loop()    -> 100 Hz: bat_slow() (SOC, T, OCV, derating, modos) + telemetria + publicacion I2C
 *   ESP32 (maestro): direccion A (0x12) lee 30 B de telemetria; direccion B (0x22) lee 10 B del control y escribe
 *                    SETMODE / EXT / HEARTBEAT (ver sub_link.h).  Cualquier comando valido cuenta como heartbeat.
 *
 * Con hardware de potencia, la planta se reemplazaría por ADC reales (V_pack, V_bus, iL) y el duty saldría por TIM1;
 * el control no cambiaría.
 */
#include "app_sub.h"
#include "main.h"
#include "sub_i2c.h"
#include "bat_model.h"
#include <string.h>

extern TIM_HandleTypeDef htim6;

static bat_t             g_bat;
static volatile uint32_t g_isr_cycles_max;     /* diagnostico: maximo de ciclos de la ISR (presupuesto 6400 @ 64 MHz) */
static volatile uint8_t  g_overrun;

/* Callback del esclavo I2C (contexto de interrupcion): solo escribe campos de 32 bits del modelo. */
static void on_cmd(uint8_t cmd, uint8_t mode, uint16_t a, uint16_t b)
{
    if (cmd == SUB_CMD_SETMODE) {
        bat_set_mode(&g_bat, mode, a, b);                       /* a = Ich_max [0.1 A], b = Idis_max [0.1 A] (modo 1) */
    } else if (cmd == SUB_CMD_EXT) {
        bat_set_ext(&g_bat, (int16_t)a, HAL_GetTick());         /* a = P_net del resto de la microrred [0.1 W] */
    }
}

void sub_init(void)
{
    bat_params_default(&g_bat.p);
    bat_init(&g_bat, 50.0f);                                    /* SOC inicial 50 % */

    /* Contador de ciclos para medir la ISR */
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CYCCNT = 0u;
    DWT->CTRL  |= DWT_CTRL_CYCCNTENA_Msk;

    sub_i2c_init(SUB_ADDR_A_BAT, SUB_ADDR_B_BAT, on_cmd);       /* antes de arrancar el timer: on_cmd ya puede tocar g_bat */
    HAL_TIM_Base_Start_IT(&htim6);
}

/* ISR de 10 kHz: control y planta en el mismo paso. */
void sub_tim_isr(void)
{
    uint32_t t0 = DWT->CYCCNT;

    bat_step(&g_bat);

    uint32_t dt = DWT->CYCCNT - t0;
    if (dt > g_isr_cycles_max) g_isr_cycles_max = dt;
    if (dt > (SystemCoreClock / (uint32_t)SUB_CTRL_HZ)) g_overrun = 1u;
}

void sub_loop(void)
{
    static uint32_t t_tel = 0u, t_led = 0u;
    uint32_t now = HAL_GetTick();

    if (now - t_tel >= 10u) {                                   /* 100 Hz */
        t_tel = now;

        /* 1) tomar y poner a cero los acumuladores de la ISR (seccion critica corta) */
        bat_acc_t acc;
        __disable_irq();
        acc = g_bat.s.acc;
        g_bat.s.acc.sum_i = 0.0f; g_bat.s.acc.sum_i2 = 0.0f; g_bat.s.acc.n = 0u;
        g_bat.s.acc.vbus_min = g_bat.s.vbus; g_bat.s.acc.vbus_max = g_bat.s.vbus;
        __enable_irq();

        /* 2) tareas lentas (fuera de la ISR; solo escriben campos que la ISR unicamente lee) */
        bat_slow(&g_bat, &acc, now, sub_i2c_ms_since_cmd());

        /* 3) copia coherente del estado -> tramas -> publicar */
        bat_state_t snap;
        uint8_t ovr;
        __disable_irq();
        snap = g_bat.s;
        ovr  = g_overrun;
        __enable_irq();

        sub_plant_tel_t pl; cvl_tel_t cv; sub_extra_tel_t ex; sub_ctrl_tel_t ct;
        bat_build_tel(&snap, ovr, &pl, &cv, &ex, &ct);
        sub_i2c_publish(&pl, &cv, &ex, &ct);
    }
    if (now - t_led >= 500u) {
        t_led = now;
        HAL_GPIO_TogglePin(GPIOB, GPIO_PIN_3);
    }
}
