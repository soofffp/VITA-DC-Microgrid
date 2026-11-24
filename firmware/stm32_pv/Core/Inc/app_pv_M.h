/*
 * app_pv_M.h — VERSIÓN UNIFICADA: UNA sola STM32 con planta + control del subsistema PV.
 *
 * REEMPLAZA a firmware/M/app_pv_M.h y app_pv_M.c en el proyecto microred_pv_M (mismo nombre y mismas
 * funciones, así main.c y el CMake no cambian; solo hay que agregar pv_control.c/.h al proyecto).
 *
 * Una sola placa responde en DOS direcciones I2C (la periférica I2C del F3 admite dos):
 *   0x10 -> telemetría de la planta  (V, I, P)           [lo que el ESP ya lee de "M"]
 *   0x20 -> telemetría del control (vref, duty, modo...) y recibe la consigna (modo, P_max) [lo de "C"]
 * Así el sketch del ESP32 NO cambia.
 */
#ifndef APP_PV_M_H
#define APP_PV_M_H

#ifdef __cplusplus
extern "C" {
#endif

void pvM_init(void);
void pvM_loop(void);
void pvM_tim_isr(void);

#ifdef __cplusplus
}
#endif

#endif /* APP_PV_M_H */
