/*
 * wind_model.h — Modelo del subsistema EOLICO (banco AVAN-300W: variador + motor + PMSG + puente de diodos)
 *                mas la etapa de potencia que anade el EMS (boost hacia el bus rigido de 48 V).
 *
 *   viento v_w --> f_ref (curva cut-in/nominal/cut-out) --> f real (1er orden, tau ~ 2 s)
 *     --> fuente Thevenin del rectificador:  V_dc = Voc(f) - Req(f)*I,   I >= 0 (diodos)
 *     --> C_in --> boost promediado (CCM) --> bus de 48 V
 *
 * Modulo PORTABLE (sin HAL). Reutiliza pv_control (P&O + PI de tension + MPPT/LIMITED/SAFE) con otros
 * parametros: una fuente Thevenin tiene P(V) = V*(Voc-V)/Req, concava igual que un panel.
 *
 * Reparto de trabajo (igual que PV):
 *   - ISR 10 kHz:  wind_isr_step()  -> solo integra (lineal, sin expf ni divisiones en el modelo)
 *   - lazo principal (1 kHz): wind_update() -> viento->f_ref->f, Voc, 1/Req, maquina de estados
 */
#ifndef WIND_MODEL_H
#define WIND_MODEL_H

#include <stdint.h>
#include "pv_control.h"
#include "sub_link.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- curva del aerogenerador (paper) ---- */
#define WIND_CUT_IN_MS     3.0f     /* [m/s] */
#define WIND_NOM_MS       11.0f
#define WIND_CUT_OUT_MS   25.0f
#define WIND_F_NOM_HZ     25.5f     /* [Hz] frecuencia a velocidad nominal y mas */
#define WIND_RPM_PER_HZ   56.0f     /* banco: motor 2 polos, 3360 rpm a 60 Hz acoplado rigido */
#ifndef WIND_HYST_MS
#define WIND_HYST_MS       0.3f     /* histeresis de reconexion tras parada (evita castañeteo en el umbral) */
#endif

/* ---- ajuste del PMSG + puente de diodos (sim/fit_pmsg.py, 22 puntos: Tabla VI + Tabla IV) ----
 *   V_dc = Ke*f - (R0 + K_R*f)*I - Vd      (f en Hz, I en A)                                         */
#define WIND_KE        2.77651f   /* [V/Hz]  */
#define WIND_R0        8.75047f   /* [ohm]   */
#define WIND_KR        0.243509f  /* [ohm/Hz]*/
#define WIND_VD       (-2.0140f)  /* [V] (negativo: offset empirico de la Tabla IV; ver fit_pmsg.py)    */
#define WIND_VREF_HI_FRAC 0.9f    /* V_ref_max dinamica = 0.9*Voc (el MPP esta en 0.5*Voc)  */
#define WIND_VREF_MAX    44.0f    /* limite estatico de V_ref [V] (D >= 0.083)              */
#define WIND_F_KNEE    5.0f       /* [Hz] bajo este valor el offset se lleva linealmente a 0 (Voc(0)=0)  */

typedef enum {
    WIND_RUN       = 0,   /* conectado, MPPT activo                   */
    WIND_CALM      = 1,   /* parado: viento < cut-in (CVL_F_IDLE)      */
    WIND_OVERSPEED = 2    /* parado: viento > cut-out (CVL_F_PROT)     */
} wind_state_t;

typedef struct {
    /* parametros (wind_init) */
    float dt;                  /* periodo de la ISR [s]                         */
    float L, C, rL, Vbus;      /* boost                                         */
    float tau_f;               /* constante de tiempo de la frecuencia [s]      */
    float dt_L, dt_C;          /* dt/L, dt/C precalculados (ISR sin divisiones) */

    /* entorno (lazo principal) */
    float vw;                  /* viento [m/s]                                  */
    float f_ref, f;            /* frecuencia pedida y real del estator [Hz]     */
    float voc, inv_req;        /* Voc(f) [V] y 1/Req(f) [1/ohm]                  */
    volatile uint8_t state;    /* wind_state_t                                  */

    /* estado electrico (ISR) */
    float v;                   /* V_in del boost = V_dc del rectificador [V]    */
    float iL;                  /* corriente del inductor [A]                    */
    float duty;                /* ultimo duty aplicado (0 si esta parado)        */
    float isrc;                /* corriente de la fuente (rectificador) [A]     */
    uint8_t active;            /* 1 = conectado y controlando (solo lo toca la ISR) */

    /* consignas del EMS (lazo principal) */
    volatile uint8_t  cmd_mode;     /* ultimo modo ordenado (0/1/2)             */
    volatile uint16_t cmd_pmax_dW;  /* Pmax ordenada [0.1 W]                    */
    volatile uint8_t  hb_lost;      /* 1 = heartbeat perdido                    */
} wind_t;

/* ---- funciones puras de la fuente ---- */
float wind_fref(float vw);                       /* viento [m/s] -> f pedida [Hz]            */
float wind_voc(float f_hz);                      /* tension en vacio [V]                     */
float wind_req(float f_hz);                      /* resistencia equivalente [ohm]            */
float wind_pmax_ideal(float f_hz);               /* Voc^2/(4 Req) [W]                        */

/* ---- modelo ---- */
void  wind_init(wind_t *w, float dt);            /* L=1 mH, C=470 uF, rL=50 mohm, Vbus=48 V, tau=2 s */
void  wind_update(wind_t *w, float vw, float dt_s);   /* lazo principal: llamar cada ~1 ms   */
void  wind_plant_step(wind_t *w, float duty);    /* solo la planta (boost + fuente) con un duty dado          */
float wind_isr_step(wind_t *w, pv_ctrl_t *c);    /* ISR: control + planta; devuelve el duty  */
/* Comando del EMS (callback de sub_i2c, puede correr en ISR del I2C): solo guarda modo y Pmax. */
void  wind_on_cmd(wind_t *w, uint8_t cmd, uint8_t mode, uint16_t a);    /* ISR: control + planta; devuelve el duty  */

/* Parametros de pv_control ajustados al eolico (kp/ki/limites/paso) */
void  wind_ctrl_params(pv_params_t *p, float ts_ctrl);

/* Supervisor (lazo principal, ~1 kHz): (1) modo efectivo = SAFE si heartbeat perdido, si no el ultimo ordenado;
 * (2) V_ref_max dinamica = min(WIND_VREF_MAX, WIND_VREF_HI_FRAC*Voc(f)): evita que el P&O se escape hacia
 *     V_ref > Voc (circuito abierto: P = 0 y el P&O no ve pendiente). */
void  wind_supervise(wind_t *w, pv_ctrl_t *c);
pv_mode_t wind_mode_eff(const wind_t *w);

/* ---- generador de viento sintetico (lazo principal) ---- */
typedef struct {
    uint32_t rng;       /* xorshift32 */
    float    turb;      /* turbulencia filtrada [m/s] */
    uint32_t t_ms;
    uint32_t next_n_ms; /* proximo muestreo de ruido (10 Hz) */
} wind_profile_t;
void  wind_profile_init(wind_profile_t *p, uint32_t seed);
/* Devuelve el viento [m/s] en el instante t_ms. gust_on != 0 anade el episodio periodico de rafaga (>25 m/s). */
float wind_profile(wind_profile_t *p, uint32_t t_ms, int gust_on);

/* ---- telemetria (mapeo obligatorio hacia el ESP32; checksums los calcula sub_i2c_publish) ---- */
uint8_t wind_conv_flags(const wind_t *w, const pv_ctrl_t *c, uint8_t overrun);
void    wind_build_tel(const wind_t *w, const pv_ctrl_t *c, uint8_t overrun,
                       sub_plant_tel_t *p, cvl_tel_t *cv, sub_extra_tel_t *x, sub_ctrl_tel_t *k);

#ifdef __cplusplus
}
#endif
#endif /* WIND_MODEL_H */
