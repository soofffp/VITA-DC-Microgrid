/*
 * load_model.h — Emulador de CARGA DC con perfiles dinamicos de demanda (portable, SIN HAL).
 *
 * Basado en "Metodologia de Emulacion de Perfiles de Demanda en Microrredes DC" (Typhoon HIL + RIGOL DL3021):
 *   - perfil sintetico de 24 h en pu (interpolacion lineal sobre nodos horarios + ruido gaussiano ~1.1 %),
 *   - dia comprimido (LOAD_DAY_S, 720 s = 12 min por defecto),
 *   - factor de potencia variable que modifica la corriente DC (perdidas del inversor),
 *   - emulador de corriente con slew limitado (0.5 A/ms) y filtro de primer orden.
 *
 * Reparto de trabajo (igual que el PV):
 *   load_model_slow()  lazo principal, 100 Hz : perfil, FP, eta, nivel, proteccion, consigna de corriente.
 *   load_model_fast()  ISR a 10 kHz           : SOLO slew + filtro + capacitor de entrada (sin div/expf/sqrt).
 *
 * Carga de POTENCIA CONSTANTE: I_dc = nivel * P_perfil / (eta(FP) * V_bus). No regula el bus; solo consume.
 */
#ifndef LOAD_MODEL_H
#define LOAD_MODEL_H

#include <stdint.h>
#include "sub_link.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Duracion del dia comprimido [s]. 0 => perfil congelado en LOAD_FIXED_PU (bring-up). */
#ifndef LOAD_DAY_S
#define LOAD_DAY_S      MICRORED_DAY_S
#endif
#ifndef LOAD_FIXED_PU
#define LOAD_FIXED_PU   0.5f
#endif
#ifndef LOAD_P_MAX_W
#define LOAD_P_MAX_W    150.0f      /* potencia de 1 pu en el lado AC del inversor [W] */
#endif

#define LOAD_SCN_RES    0u
#define LOAD_SCN_IND    1u

typedef struct {
    float    p_max_w;          /* potencia de 1 pu [W]                                         */
    float    day_s;            /* dia comprimido [s]; 0 = congelado                            */
    float    fixed_pu;         /* nivel fijo si day_s == 0                                     */
    float    fixed_fp;         /* FP fijo si day_s == 0                                        */
    float    sigma_pu;         /* desviacion del ruido gaussiano [pu]                          */
    uint32_t noise_ms;         /* periodo de renovacion del ruido [ms]                         */
    uint32_t seed;             /* semilla del xorshift32 (determinista)                        */
    float    ts;               /* paso de la ISR [s]                                           */
    float    slew_a_per_ms;    /* slew de la corriente [A/ms]                                  */
    float    tau_i_s;          /* constante del filtro de corriente [s]                        */
    float    c_in_f;           /* capacitor de entrada [F]                                     */
    float    tau_v_s;         /* constante del filtro de V_bus visto por el capacitor [s]      */
    float    level_rate;       /* rampa del nivel [1/s] (0.5 = 50 %/s)                         */
    float    v_nom;            /* V_bus por defecto [V]                                        */
    float    v_open;           /* abre si V_bus < v_open [V]                                   */
    float    v_close;          /* cierra si V_bus >= v_close [V]                               */
    uint32_t ext_timeout_ms;   /* sin EXT en este tiempo => v_nom                              */
    float    safe_frac;        /* SAFE: fraccion de la potencia del perfil                     */
    float    safe_min_w;       /* SAFE: potencia minima [W] (se usa la mayor de las dos)       */
} load_params_t;

typedef struct {
    load_params_t p;
    /* constantes derivadas (init) */
    float    di_step;          /* slew por paso [A]            */
    float    k_i, k_v;         /* coeficientes de los filtros  */
    float    c_over_ts;        /* C / Ts [F/s] para i_cap      */
    uint32_t day_ms;
    /* estado lento (lazo principal) */
    uint32_t t_ms, noise_t_ms, ext_age_ms, rng;
    uint8_t  scenario, mode_cmd, mode_eff, hb_lost, open, prot;
    float    level_max;        /* tope de nivel ordenado en modo REDUCIDA (0..1)  */
    float    level;            /* nivel aplicado (0..1), con rampa                */
    float    noise, ext_v, p_pu, fp, eta, p_prof_w, p_req_dc_w, phase;
    volatile float vbus;       /* V_bus vigente (lo lee la ISR)                   */
    volatile float i_cmd;      /* consigna de corriente (la lee la ISR)           */
    /* estado rapido (ISR) */
    float    i_slew, i_load, i_cap, vbus_f;
} load_model_t;

void  load_params_default(load_params_t *p);
void  load_model_init(load_model_t *m, const load_params_t *p, uint8_t scenario);

/* Comandos (llamar SOLO desde el lazo principal) */
void  load_model_set_mode(load_model_t *m, uint8_t mode, uint16_t a_pm, uint16_t b_scn);
void  load_model_set_vbus_dV(load_model_t *m, uint16_t dV_raw);   /* int16 en [0.1 V] reinterpretado como uint16 */
void  load_model_set_hb_lost(load_model_t *m, uint8_t lost);

void  load_model_slow(load_model_t *m, uint32_t dt_ms);           /* ~100 Hz */
void  load_model_fast(load_model_t *m);                           /* 10 kHz  */

static inline float load_model_i_in(const load_model_t *m) { return m->i_load + m->i_cap; }

/* Perfiles sin ruido (para pruebas): pos_h en [0,24) */
float load_profile_pu(uint8_t scn, float pos_h);
float load_profile_fp(uint8_t scn, float pos_h);
float load_eta(float fp);

/* Mapeo OBLIGATORIO de telemetria (lo consume el sketch del ESP32). i_in en A (corriente consumida). */
void  load_model_fill_tel(const load_model_t *m, float i_in, uint8_t overrun,
                          sub_plant_tel_t *pl, cvl_tel_t *cv, sub_extra_tel_t *ex, sub_ctrl_tel_t *ct);

#ifdef __cplusplus
}
#endif
#endif /* LOAD_MODEL_H */
