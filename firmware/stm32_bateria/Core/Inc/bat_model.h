/*
 * bat_model.h — Subsistema BATERIA: pack Li-ion 10s5p + convertidor bidireccional + capacitor del bus
 *               + control local (lazo exterior de V_bus, lazo interior de corriente, limites, derating, modos).
 *
 * Portable (sin HAL). Mismo patron que pv_plant/pv_control, pero planta y control comparten estado:
 *
 *   ISR 10 kHz :  bat_step()   -> mide (estado del paso anterior) -> PI de V_bus -> PI de corriente -> duty
 *                                  -> avanza la planta (Euler semi-implicito).  Sin expf/powf/sqrt/lazos.
 *   lazo ppal  :  bat_slow()   -> 100 Hz: SOC (Kahan), temperatura, OCV(SOC), R0(SOC), derating, modos,
 *                                  heartbeat, EXT obsoleto, banderas.   (la ISR solo LEE lo que esto escribe)
 *                 bat_build_tel() -> arma las 4 tramas de sub_link.h a partir de una COPIA del estado.
 *   callback I2C: bat_set_mode(), bat_set_ext()  (escriben campos de 32 bits; la ISR los lee)
 *
 * Convenciones: corriente del pack con signo, + DESCARGA (entrega al bus), - CARGA.
 *
 * Modelo (promediado):
 *   V_pack = OCV(SOC) - R0(SOC)*iL - v1 ;  C1*dv1/dt = iL - v1/R1          (Thevenin de 1er orden)
 *   L*diL/dt = V_pack - (1-D)*V_bus - rL*iL                                  (puente bidireccional)
 *   C_bus*dV_bus/dt = (1-D)*iL + P_eff/V_bus                                  (P_eff>0: sobra generacion)
 *   dSOC/dt = -iL/(3600*Q) * SOC_ACCEL           (SOC_ACCEL = 60: SOC 60x mas rapido que el tiempo real)
 */
#ifndef BAT_MODEL_H
#define BAT_MODEL_H

#include <stdint.h>
#include "sub_link.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Aceleracion temporal del SOC (valor por defecto; en tiempo de ejecucion: bat_t.p.soc_accel).
 * 60 => 1 s de reloj equivale a 60 s de SOC: 7 A durante 1 s mueve el SOC 0.8 %; 1 h real a 7 A pasa en 1 minuto.
 * Poner 1 para tiempo real. SOLO afecta al integrador de SOC (la termica y la RC siguen en tiempo real). */
#ifndef BAT_SOC_ACCEL
#define BAT_SOC_ACCEL 60.0f
#endif

/* 1 = la PWM aplica el duty calculado un paso despues (como TIM1 con precarga): mas realista y conservador. */
#ifndef BAT_DUTY_DELAY
#define BAT_DUTY_DELAY 1
#endif

typedef struct {
    /* pack */
    float dt;           /* paso de la ISR [s]                                          */
    float q_ah;         /* capacidad 14.5 Ah (5p x 2.9 Ah)                              */
    float r0_hi, r0_lo; /* R0 a SOC=100 % y a SOC=0 % [ohm]; lineal en (1-SOC)          */
    float r1, c1;       /* rama RC [ohm], [F]                                            */
    float soc_accel;    /* ver BAT_SOC_ACCEL                                             */
    /* convertidor y bus */
    float L, rL, c_bus; /* [H], [ohm], [F]                                               */
    float vbus_ref;     /* 48 V                                                          */
    float d_min, d_max;
    /* lazos */
    float kp_v, ki_v, kb_v;   /* exterior: [A/V], [A/(V s)], anti-windup [1/s]           */
    float kp_i, ki_i, kb_i;   /* interior: [duty/A], [duty/(A s)], anti-windup [1/s]     */
    /* carga del resto de la microrred (potencia constante con repliegue por sub/sobretension) */
    float uv_lo, uv_hi;       /* deficit: carga plena sobre uv_hi, cero bajo uv_lo [V]   */
    float ov_lo, ov_hi;       /* superavit: generacion plena bajo ov_lo, cero sobre ov_hi [V] */
    /* limites */
    float i_nom;              /* 7.0 A (carga y descarga, modo NORMAL)                   */
    float i_safe;             /* 2.0 A (modo SAFE)                                       */
    float i_abs;              /* 15 A tope de las consignas del modo LIMITED             */
    float soc_ch0;            /* 95 %: empieza a reducir carga; 0 en 100 %               */
    float soc_dis0, soc_dis1; /* 10 % -> 5 %: descarga baja a 0                          */
    float vch0, vch1;         /* 41.8 -> 42.0 V: carga baja a 0                          */
    float vdis0, vdis1;       /* 31 -> 30 V: descarga baja a 0                           */
    float t_der0, t_der1;     /* 45 -> 60 C                                              */
    float f_alpha;            /* filtro del derating por tension (por llamada a bat_slow) */
    float v_fault_lo, v_fault_hi, t_fault;
    float band;               /* semibanda del bus [V] (2.4 = 5 % de 48)                */
    /* termica */
    float t_amb, rth, cth;    /* 25 C, 3 K/W, 200 J/K (tau = 600 s)                      */
    /* derivadas (las calcula bat_init; si cambias L, c_bus, c1, r1 o dt llama otra vez a bat_init) */
    float k_il, k_v1, inv_r1, k_vb, inv_uv, inv_ov;
} bat_params_t;

typedef struct {                 /* acumuladores ISR -> lazo principal (copiar y poner a cero con IRQ deshabilitadas) */
    float    sum_i;              /* suma de iL por paso [A]  */
    float    sum_i2;             /* suma de iL^2             */
    float    vbus_min, vbus_max; /* extremos del bus en la ventana */
    uint32_t n;                  /* pasos acumulados         */
} bat_acc_t;

typedef struct {
    /* --- escritos por la ISR --- */
    float iL, v1, vpack, vbus;   /* estado de la planta (vpack = tension en bornes)  */
    float xi_v, xi_i;            /* integradores                                     */
    float iref;                  /* referencia de corriente tras saturar [A]         */
    float duty;                  /* duty aplicado en la ultima actualizacion de planta */
    float d_cmd;                 /* duty calculado en el ultimo paso                 */
    uint8_t sat;                 /* 1 = I_ref saturada por el limite vigente         */
    bat_acc_t acc;
    /* --- escritos por bat_slow (lazo principal) --- */
    float ocv, r0;               /* OCV(SOC) y R0(SOC) del pack                      */
    float ich_eff, idis_eff;     /* limites vigentes tras modo y derating [A, >=0]   */
    float soc, soc_c;            /* [%] y compensacion de Kahan                      */
    float temp;                  /* [C]                                              */
    float fv_ch, fv_dis;         /* derating por tension filtrado (0..1)             */
    float lim_nom_dis;           /* limite NOMINAL de descarga del modo efectivo [A] */
    float vref;
    uint8_t mode_eff;            /* 0/1/2 efectivo                                   */
    uint8_t fault, hb_lost, ext_stale;
    uint8_t cvl_flags, xflags;
    /* --- escritos por callbacks / ISR de I2C (campos de 32 bits) --- */
    volatile uint32_t cmd_pack;  /* modo<<16 | a<<8 | b   (a,b en [0.1 A], 0..150)   */
    volatile float    p_eff;     /* potencia neta del resto de la microrred [W]      */
    volatile uint32_t ext_ms;    /* instante del ultimo EXT                          */
} bat_state_t;

typedef struct { bat_params_t p; bat_state_t s; } bat_t;

void  bat_params_default(bat_params_t *p);
void  bat_init(bat_t *m, float soc0_pct);              /* usa m->p ya cargado; deja el bus en V_ref y el pack en reposo */

void  bat_step(bat_t *m);                              /* ISR a 10 kHz */
void  bat_slow(bat_t *m, const bat_acc_t *acc, uint32_t now_ms, uint32_t ms_since_cmd);   /* ~100 Hz, lazo principal */

void  bat_set_mode(bat_t *m, uint8_t mode, uint16_t a_dA, uint16_t b_dA);   /* SUB_CMD_SETMODE */
void  bat_set_ext(bat_t *m, int16_t p_net_dW, uint32_t now_ms);             /* SUB_CMD_EXT     */

/* Arma las tramas (sin checksum: lo pone sub_i2c_publish). 's' debe ser una COPIA coherente del estado. */
void  bat_build_tel(const bat_state_t *s, uint8_t overrun, sub_plant_tel_t *pl, cvl_tel_t *cv,
                    sub_extra_tel_t *ex, sub_ctrl_tel_t *ct);

float bat_ocv_pack(float soc_pct);                     /* interpolacion lineal, pack de 10 celdas */

#ifdef __cplusplus
}
#endif

#endif /* BAT_MODEL_H */
