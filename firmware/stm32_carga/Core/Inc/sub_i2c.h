/*
 * sub_i2c.h / sub_i2c.c — Esclavo I2C de doble direccion comun a eolica, bateria y carga.
 * Evita lecturas "rotas": no reescribe los buffers mientras el ESP32 esta leyendo.
 */
#ifndef SUB_I2C_H
#define SUB_I2C_H

#include <stdint.h>
#include "sub_link.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*sub_cmd_cb_t)(uint8_t cmd, uint8_t mode, uint16_t a, uint16_t b);

/* Configura las dos direcciones (A en OwnAddress1, B en OwnAddress2), arranca el modo listen.
 * No arranca TIM6 (lo hace la app). Requiere hi2c1 generado por CubeMX. */
void     sub_i2c_init(uint8_t addrA, uint8_t addrB, sub_cmd_cb_t cb);

/* Publica la telemetria (calcula los checksums). Llamar desde el lazo principal, no desde la ISR. */
void     sub_i2c_publish(const sub_plant_tel_t *p, const cvl_tel_t *c, const sub_extra_tel_t *x,
                         const sub_ctrl_tel_t *k);

/* ms desde el ultimo comando valido (cualquier CMD cuenta como heartbeat). */
uint32_t sub_i2c_ms_since_cmd(void);

#ifdef __cplusplus
}
#endif

#endif /* SUB_I2C_H */
