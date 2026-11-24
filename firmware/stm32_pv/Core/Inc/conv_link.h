/*
 * conv_link.h — Trama ESTANDAR del "convertidor universal" (igual para PV, eolico, bateria y carga).
 *
 * Idea: todos los subsistemas exponen su convertidor DC-DC con la MISMA interfaz hacia el SCADA:
 *   lado de entrada (fuente), lado de salida (bus de 48 V), duty y banderas de estado.
 * Lo que cambia entre subsistemas es solo el lazo de control que hay detras (MPPT, CC/CV, ...).
 * Hoy el convertidor vive dentro de la misma STM que la planta emulada; podría moverse a otra
 * STM (o al convertidor fisico del PEBB) sin cambiar esta trama ni el SCADA.
 *
 * Trama de 10 bytes, little-endian, checksum XOR de los 9 anteriores.
 */
#ifndef CONV_LINK_H
#define CONV_LINK_H

#include <stdint.h>

typedef struct __attribute__((packed)) {
    uint16_t vin_mV;       /* tension del lado fuente (V_pv en PV)               [mV]      */
    uint16_t vbus_mV;      /* tension del bus DC visto por el convertidor        [mV]      */
    uint16_t ibus_mA;      /* corriente entregada al bus (promedio)              [mA]      */
    uint16_t duty_1e4;     /* duty aplicado, 6000 = 0.6000                                  */
    uint8_t  flags;        /* ver CVL_F_*                                                   */
    uint8_t  checksum;     /* XOR de los 9 bytes anteriores                                 */
} cvl_tel_t;

/* Banderas = "reglas locales activas" que el SCADA puede mostrar */
#define CVL_F_DUTY_MAX    0x01u   /* duty saturado en su maximo (V_pv en el minimo permitido)      */
#define CVL_F_DUTY_MIN    0x02u   /* duty saturado en su minimo                                    */
#define CVL_F_LIMITING    0x04u   /* modo LIMITED y la potencia ya esta topada en P_max            */
#define CVL_F_VREF_LIMIT  0x08u   /* V_ref del MPPT pegada a uno de sus limites                    */
#define CVL_F_OVERRUN     0x10u   /* la ISR de control se paso de su presupuesto de tiempo         */
#define CVL_F_SAFE        0x20u   /* modo SAFE: V_ref congelada                                    */

#endif /* CONV_LINK_H */
