/*
 * app_sub.h — Contrato comun de las apps de subsistema (eolica, bateria, carga).
 * Cada proyecto compila UNO de: app_wind.c, app_bat.c, app_load.c  + sub_i2c.c
 * main.c (3 cambios respecto al proyecto PV):
 *   #include "app_sub.h"      en USER CODE Includes
 *   sub_init();               en USER CODE 2   (despues de los MX_..._Init)
 *   sub_loop();               en el while(1), entre USER CODE 3 y la llave
 */
#ifndef APP_SUB_H
#define APP_SUB_H

#ifdef __cplusplus
extern "C" {
#endif

void sub_init(void);      /* modelo + control + I2C + TIM6 */
void sub_loop(void);      /* telemetria a 100 Hz, LED, heartbeat */
void sub_tim_isr(void);   /* ISR de 10 kHz (llamada desde sub_i2c.c) */

#ifdef __cplusplus
}
#endif

#endif /* APP_SUB_H */
