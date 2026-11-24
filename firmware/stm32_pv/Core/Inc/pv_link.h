/*
 * pv_link.h — Interfaz entre M (main/planta), C (control) y el ESP32 para el subsistema PV.
 *
 * FUENTE ÚNICA DE VERDAD: lo incluyen el firmware de M, el de C y la simulación en PC.
 * Si cambias una escala aquí, cámbiala en el sketch del ESP32 solo si afecta a las tramas I2C.
 *
 *   C --PWM (duty D, 20 kHz)--------------> M     (C.PA6/TIM3_CH1  ->  M.PA6/TIM3_CH1, modo captura PWM)
 *   M --DAC1_OUT1 = V_pv  (0..V_FS)-------> C     (M.PA4 -> C.PA0/ADC1_IN1)
 *   M --DAC1_OUT2 = I_pv  (0..I_FS)-------> C     (M.PA5 -> C.PA1/ADC1_IN2)
 *   ESP32 (maestro I2C) <-> M (0x10): telemetría de la planta (V, I, P)
 *   ESP32 (maestro I2C) <-> C (0x20): telemetría del control + consigna (modo, P_max)
 */
#ifndef PV_LINK_H
#define PV_LINK_H

#include <stdint.h>

/* ---- Escalas analógicas M -> C: 3.3 V de fondo de escala en ambos canales ---- */
#define PVL_V_FULLSCALE   36.0f      /* V_pv: 0..36 V  <->  0..3.3 V */
#define PVL_I_FULLSCALE   10.0f      /* I_pv: 0..10 A  <->  0..3.3 V */
#define PVL_ADC_MAX       4095.0f    /* 12 bit en DAC (M) y ADC (C)   */

/* ---- Tiempos ---- */
#define PVL_PWM_HZ        20000.0f   /* portadora de la PWM de C                       */
#define PVL_CTRL_HZ       10000.0f   /* ISR de control (C) e ISR de planta (M)         */

/* ---- Direcciones I2C (7 bit; en CubeMX "Primary slave address" va este valor tal cual) ---- */
#define PVL_I2C_ADDR_M    0x10u      /* 16 decimal */
#define PVL_I2C_ADDR_C    0x20u      /* 32 decimal */

/* ---- Trama de telemetría de la planta (M -> ESP32), 10 bytes. Igual que la que ya usaban. ---- */
typedef struct __attribute__((packed)) {
    uint16_t voltage_mV;
    uint16_t current_mA;
    uint32_t power_mW;
    uint8_t  status;       /* 0 normal, 1 sin PWM de C, 2 apagado */
    uint8_t  checksum;     /* XOR de los 9 bytes anteriores        */
} pvl_plant_tel_t;

/* ---- Trama de telemetría del control (C -> ESP32), 10 bytes ---- */
typedef struct __attribute__((packed)) {
    uint16_t vref_mV;      /* referencia de tensión del MPPT [mV]           */
    uint16_t duty_1e4;     /* duty en diezmilésimas (6000 = 0.6000)         */
    uint16_t p_cW;         /* potencia medida [0.01 W]                      */
    uint16_t pmax_dW;      /* tope de potencia vigente [0.1 W]              */
    uint8_t  mode;         /* 0 MPPT, 1 LIMITED, 2 SAFE                     */
    uint8_t  checksum;     /* XOR de los 9 bytes anteriores                 */
} pvl_ctrl_tel_t;

/* ---- Comando (ESP32 -> C), 5 bytes: [CMD, modo, pmax_dW_hi, pmax_dW_lo, xor de los 4 anteriores] ---- */
#define PVL_CMD_SETMODE   0x01u
#define PVL_CMD_LEN       5u

static inline uint8_t pvl_xor(const uint8_t *p, uint32_t n)
{
    uint8_t x = 0;
    for (uint32_t i = 0; i < n; i++) x ^= p[i];
    return x;
}

#endif /* PV_LINK_H */
