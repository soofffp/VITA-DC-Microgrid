/*
 * pv_control.h — Control local del subsistema PV (convertidor boost hacia bus DC de 48 V)
 *
 * Módulo portable: NO depende de HAL ni de ningún periférico. El firmware de la STM32
 * solo debe (1) leer V_pv e I_pv en unidades físicas, (2) llamar pv_ctrl_step() con
 * periodo fijo ts_ctrl, y (3) escribir el duty devuelto en el timer de PWM.
 *
 * Estructura (un solo lazo rápido + MPPT lento):
 *   MPPT (P&O, cada ts_mppt) --> V_ref --> PI de V_pv (cada ts_ctrl) --> duty D
 *
 * Convención boost: V_pv = (1 - D) * V_bus  =>  subir D BAJA V_pv.
 * Por eso el error se define como (V_pv - V_ref): error positivo => subir D.
 *
 * Consignas que vienen del SCADA (vía ESP32, I2C):
 *   - modo:  PV_MODE_MPPT / PV_MODE_LIMITED / PV_MODE_SAFE
 *   - p_max: potencia máxima [W] (solo se usa en PV_MODE_LIMITED)
 */
#ifndef PV_CONTROL_H
#define PV_CONTROL_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PV_MODE_MPPT    = 0,  /* seguimiento del punto de máxima potencia            */
    PV_MODE_LIMITED = 1,  /* MPPT con tope de potencia p_max (curtailment)       */
    PV_MODE_SAFE    = 2   /* congela V_ref (último valor válido), solo regula V  */
} pv_mode_t;

typedef struct {
    float ts_ctrl;     /* periodo del lazo de tensión [s]                         */
    float ts_mppt;     /* periodo de actualización del MPPT [s] (>= ts_ctrl)      */
    float kp;          /* ganancia P del lazo de V  [duty / V]                    */
    float ki;          /* ganancia I del lazo de V  [duty / (V*s)]                */
    float v_bus_nom;   /* tensión nominal del bus para el feedforward [V]         */
    float d_min;       /* límites de duty                                         */
    float d_max;
    float vref_min;    /* límites de la referencia de tensión del panel [V]       */
    float vref_max;
    float vref_step;   /* paso del P&O [V]                                        */
    float vref_init;   /* referencia de arranque [V] (típico 0.8 * Voc)           */
    float lim_gain;    /* PV_MODE_LIMITED: dV_ref = lim_gain * (P - p_max) [V/W]  */
    float lim_band;    /* LIMITED regula potencia si P > p_max*(1-lim_band)       */
} pv_params_t;

typedef struct {
    pv_params_t p;

    /* consignas */
    pv_mode_t mode;
    float     p_max;

    /* estado del lazo de tensión */
    float vref;
    float integ;
    float duty;

    /* estado del MPPT */
    float    p_prev;
    float    dir;          /* +1: subir V_ref, -1: bajarla */
    float    acc_v, acc_i; /* promedios sobre la ventana del MPPT */
    uint32_t acc_n;
    uint32_t mppt_div;     /* ts_mppt / ts_ctrl */

    /* última medición promediada (útil para telemetría) */
    float p_avg;
} pv_ctrl_t;

/* Parámetros por defecto para el módulo Renogy RNG-100D-SS (36 celdas, Voc ~24.3 V)
 * con boost hacia 48 V. Los valores de kp/ki están sintonizados en simulación para
 * L = 470 uH y C_in = 220 uF: DEBEN revisarse con los componentes reales. */
void pv_params_default(pv_params_t *p);

void  pv_ctrl_init(pv_ctrl_t *c, const pv_params_t *p);

/* Cambia modo y tope de potencia. Seguro de llamar desde el handler del I2C
 * (solo escribe dos campos). */
void  pv_ctrl_set_mode(pv_ctrl_t *c, pv_mode_t mode, float p_max_w);

/* Un paso del lazo. Llamar cada ts_ctrl con V_pv [V] e I_pv [A] medidos.
 * Devuelve el duty a aplicar (0..1), ya saturado en [d_min, d_max]. */
float pv_ctrl_step(pv_ctrl_t *c, float v_pv, float i_pv);

/* Acceso de solo lectura para telemetría */
static inline float pv_ctrl_vref(const pv_ctrl_t *c)  { return c->vref;  }
static inline float pv_ctrl_duty(const pv_ctrl_t *c)  { return c->duty;  }
static inline float pv_ctrl_power(const pv_ctrl_t *c) { return c->p_avg; }

#ifdef __cplusplus
}
#endif

#endif /* PV_CONTROL_H */
