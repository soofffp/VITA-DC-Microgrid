/*
 * sub_link.h — Interfaz comun de los subsistemas EOLICO, BATERIA y CARGA hacia el ESP32.
 * (PV conserva su interfaz propia, pv_link.h; ambas comparten conv_link.h.)
 *
 * Cada subsistema vive en UNA STM32F303K8 que responde en DOS direcciones I2C:
 *   direccion A (0x11 / 0x12 / 0x13):  LECTURA de 30 bytes = [planta 10][convertidor 10][extra 10]
 *                                       (una lectura de 10 o 20 bytes tambien funciona)
 *   direccion B (0x21 / 0x22 / 0x23):  LECTURA de 10 bytes = telemetria del control
 *                                       ESCRITURA de 7 bytes = comando (ver SUB_CMD_LEN)
 * Todas las tramas: little-endian, packed, checksum = XOR de los bytes anteriores.
 */
#ifndef SUB_LINK_H
#define SUB_LINK_H

#include <stdint.h>
#include "conv_link.h"

#define SUB_ADDR_A_WIND   0x11u
#define SUB_ADDR_B_WIND   0x21u
#define SUB_ADDR_A_BAT    0x12u
#define SUB_ADDR_B_BAT    0x22u
#define SUB_ADDR_A_LOAD   0x13u
#define SUB_ADDR_B_LOAD   0x23u

/* ---- RELOJ COMUN DE LA DEMO: 1 dia de 24 h comprimido en MICRORED_DAY_S segundos ----
 * Todos los subsistemas (PV, eolica, carga) usan este dia. Cambiar a 300 o 720 = cambiar solo este numero
 * (y reflashear). MICRORED_DAY_START_H = hora simulada a la que arranca el dia (6 = amanecer).
 * Para que las placas queden en fase: encenderlas / resetearlas JUNTAS (mismo hub USB). */
#ifndef MICRORED_DAY_S
#define MICRORED_DAY_S        360
#endif
#ifndef MICRORED_DAY_START_H
#define MICRORED_DAY_START_H  6.0f
#endif

#define SUB_CTRL_HZ       10000.0f   /* ISR de modelo+control */
#define SUB_HB_TIMEOUT_MS 5000u      /* sin comandos/heartbeat durante este tiempo -> regla de modo seguro */

/* ---- estado de la planta (campo status) ---- */
#define SUB_ST_OK         0u
#define SUB_ST_IDLE       1u
#define SUB_ST_FAULT      2u
#define SUB_ST_OVERRUN    3u    /* la ISR se paso de su presupuesto de 100 us */
#define SUB_ST_NOHB       4u    /* heartbeat perdido */

/* ---- tramas (10 bytes cada una) ---- */
typedef struct __attribute__((packed)) {
    uint16_t v_mV;          /* tension propia del subsistema (panel/bus DC del rectificador/pack/bus)  */
    int16_t  i_mA;          /* corriente propia (con signo)                                            */
    int32_t  p_mW;          /* potencia (con signo; bateria: + descarga, - carga)                      */
    uint8_t  status;        /* SUB_ST_*                                                                */
    uint8_t  checksum;
} sub_plant_tel_t;

/* convertidor universal: cvl_tel_t en conv_link.h (vin, vbus, ibus, duty/actuacion, flags) */

typedef struct __attribute__((packed)) {
    int16_t  x1, x2, x3, x4;  /* significado segun subsistema (ver tablas abajo)                       */
    uint8_t  xflags;
    uint8_t  checksum;
} sub_extra_tel_t;
/*  EOLICA:  x1 = viento [cm/s]   x2 = f_estator [cHz]   x3 = rpm        x4 = V_rectificador [dV]
 *  BATERIA: x1 = SOC [0.1 %]     x2 = T_celda [0.1 C]   x3 = Ich_max [dA] x4 = Idis_max [dA]
 *  CARGA:   x1 = nivel [0.1 %]   x2 = FP [1e-3]         x3 = fase del perfil [0.1 %]  x4 = escenario (0 res, 1 ind) */

typedef struct __attribute__((packed)) {
    uint16_t ref_mV;        /* referencia del lazo exterior (V_ref MPPT, V_bus ref, ...) [mV]; carga: 0 */
    uint16_t act_1e4;       /* actuacion (duty) en diezmilesimas                                        */
    int16_t  p_dW;          /* potencia vigente [0.1 W], con signo                                      */
    uint16_t lim_dW;        /* limite vigente (potencia [0.1 W] o corriente [0.1 A], segun subsistema)  */
    uint8_t  mode;          /* 0 normal/MPPT, 1 limitado/reducido, 2 seguro                             */
    uint8_t  checksum;
} sub_ctrl_tel_t;

typedef struct __attribute__((packed)) {
    sub_plant_tel_t plant;
    cvl_tel_t       conv;
    sub_extra_tel_t extra;
} sub_block_a_t;            /* 30 bytes */

/* ---- comando ESP32 -> direccion B: [CMD, modo, a_hi, a_lo, b_hi, b_lo, xor de los 6 anteriores] ---- */
#define SUB_CMD_LEN       7u
#define SUB_CMD_SETMODE   0x01u  /* modo + limites a,b (segun subsistema)                             */
#define SUB_CMD_EXT       0x02u  /* valor externo con signo en a (int16). BATERIA: potencia neta del resto de la microrred [0.1 W] (+ sobra generacion) */
#define SUB_CMD_HEARTBEAT 0x03u  /* solo refresca el heartbeat                                        */
/*  SETMODE por subsistema:
 *  EOLICA : modo 0 MPPT | 1 LIMITED (a = Pmax [0.1 W]) | 2 SAFE (congela V_ref)
 *  BATERIA: modo 0 NORMAL (limites nominales) | 1 LIMITED (a = Ich_max [0.1 A], b = Idis_max [0.1 A]) | 2 SAFE (limites conservadores)
 *  CARGA  : modo 0 NORMAL | 1 REDUCIDA (a = nivel [0.1 %]) | 2 SAFE (perfil minimo); b = escenario (0 res, 1 ind, 0xFFFF = sin cambio) */

static inline uint8_t sub_xor(const uint8_t *p, uint32_t n)
{
    uint8_t x = 0;
    for (uint32_t i = 0; i < n; i++) x ^= p[i];
    return x;
}

#endif /* SUB_LINK_H */
