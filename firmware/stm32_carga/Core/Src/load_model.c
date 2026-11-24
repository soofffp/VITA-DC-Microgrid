/*
 * load_model.c — Emulador de carga DC con perfiles dinamicos (ver load_model.h). Sin HAL.
 *
 * ---------------------------------------------------------------------------------------------
 * PERFILES SINTETICOS (pu, un nodo por hora 0..23; el nodo 24 = nodo 0; max = 1.00 pu)
 * ---------------------------------------------------------------------------------------------
 *  h :  0    1    2    3    4    5    6    7    8    9   10   11
 *  RES: .30  .26  .23  .21  .21  .24  .38  .62  .52  .40  .34  .32
 *  IND: .18  .16  .15  .15  .16  .30  1.00 .88  .84  .86  .85  .87
 *
 *  h : 12   13   14   15   16   17   18   19   20   21   22   23
 *  RES: .36  .35  .30  .28  .30  .40  .58  .78  1.00 .98  .70  .45
 *  IND: .80  .84  .86  .85  .84  .83  .85  .84  .82  .80  .78  .30
 *
 *  Residencial: madrugada baja (0-6 h), pico moderado a las 7 h (electrodomesticos), valle de tarde
 *  (15 h), pico MAXIMO nocturno a las 20-21 h.
 *  Industrial: pico de arranque a las 6 h, meseta alta hasta las 22 h (dos turnos), base minima de noche.
 *
 * FACTOR DE POTENCIA (lado AC del inversor), mismos nodos horarios:
 *  h :  0    1    2    3    4    5    6    7    8    9   10   11
 *  RES: .88  .89  .90  .90  .90  .92  .94  .95  .95  .94  .93  .92
 *  IND: .90  .90  .90  .90  .90  .85  .70  .78  .85  .88  .90  .90
 *  h : 12   13   14   15   16   17   18   19   20   21   22   23
 *  RES: .92  .92  .91  .91  .90  .89  .87  .86  .85  .85  .86  .87
 *  IND: .90  .90  .90  .90  .90  .90  .90  .90  .90  .90  .90  .90
 *
 * EFECTO DEL FP EN EL BUS DC (paper: FP bajo => mas corriente AC => mas perdidas en el inversor):
 *     eta(FP) = 0.96 - 0.30 * (1 - FP)^2          FP=1: 0.960 | 0.85: 0.953 | 0.7: 0.933 | 0.5: 0.885
 *     I_dc    = nivel * P_perfil / (eta(FP) * V_bus)          (potencia constante)
 *  P_perfil = p_pu * P_MAX es la potencia entregada a la carga (lado AC); el bus ve P_perfil/eta.
 *  "Potencia pedida por el perfil" en el bus (lim_dW) = P_perfil / eta.
 *
 * MODO SAFE ("perfil minimo"): P = max(20 % de P_perfil, 10 W), nunca mayor que P_perfil. En pu del
 *  perfil: nivel = min(1, max(0.20, 10 W / P_perfil)). (El piso de 10 W mantiene las cargas esenciales
 *  cuando el perfil esta en su valle.)
 * ---------------------------------------------------------------------------------------------
 */
#include "load_model.h"
#include <math.h>

static const float k_pu[2][24] = {
    { .30f,.26f,.23f,.21f,.21f,.24f,.38f,.62f,.52f,.40f,.34f,.32f,
      .36f,.35f,.30f,.28f,.30f,.40f,.58f,.78f,1.00f,.98f,.70f,.45f },
    { .18f,.16f,.15f,.15f,.16f,.30f,1.00f,.88f,.84f,.86f,.85f,.87f,
      .80f,.84f,.86f,.85f,.84f,.83f,.85f,.84f,.82f,.80f,.78f,.30f }
};
static const float k_fp[2][24] = {
    { .88f,.89f,.90f,.90f,.90f,.92f,.94f,.95f,.95f,.94f,.93f,.92f,
      .92f,.92f,.91f,.91f,.90f,.89f,.87f,.86f,.85f,.85f,.86f,.87f },
    { .90f,.90f,.90f,.90f,.90f,.85f,.70f,.78f,.85f,.88f,.90f,.90f,
      .90f,.90f,.90f,.90f,.90f,.90f,.90f,.90f,.90f,.90f,.90f,.90f }
};

static float interp24(const float *tab, float pos)
{
    int h = (int)pos;
    if (h < 0) h = 0;
    if (h > 23) h = 23;
    float f = pos - (float)h;
    int h1 = (h == 23) ? 0 : h + 1;          /* periodico: 24 h -> 0 h */
    return tab[h] + f * (tab[h1] - tab[h]);
}

float load_profile_pu(uint8_t scn, float pos_h) { return interp24(k_pu[scn ? 1 : 0], pos_h); }
float load_profile_fp(uint8_t scn, float pos_h) { return interp24(k_fp[scn ? 1 : 0], pos_h); }

float load_eta(float fp)
{
    float d = 1.0f - fp;
    return 0.96f - 0.30f * d * d;
}

static float clampf(float x, float lo, float hi) { return x < lo ? lo : (x > hi ? hi : x); }

static uint32_t xorshift32(uint32_t *s)
{
    uint32_t x = *s;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    *s = x;
    return x;
}

/* Gaussiana N(0,1) aprox. por Irwin-Hall: suma de 4 uniformes [0,1): media 2, desviacion 0.5774 */
static float gauss(uint32_t *s)
{
    float acc = 0.0f;
    for (int k = 0; k < 4; k++) acc += (float)(xorshift32(s) >> 8) * (1.0f / 16777216.0f);
    return (acc - 2.0f) * 1.7320508f;
}

void load_params_default(load_params_t *p)
{
    p->p_max_w        = LOAD_P_MAX_W;
    p->day_s          = (float)LOAD_DAY_S;
    p->fixed_pu       = LOAD_FIXED_PU;
    p->fixed_fp       = 0.90f;
    p->sigma_pu       = 0.011f;
    p->noise_ms       = 100u;
    p->seed           = 0x1234ABCDu;
    p->ts             = 1.0f / SUB_CTRL_HZ;
    p->slew_a_per_ms  = 0.5f;
    p->tau_i_s        = 0.2e-3f;
    p->c_in_f         = 47.0e-6f;
    p->tau_v_s        = 0.5e-3f;
    p->level_rate     = 0.5f;
    p->v_nom          = 48.0f;
    p->v_open         = 40.0f;
    p->v_close        = 44.0f;
    p->ext_timeout_ms = 3000u;
    p->safe_frac      = 0.20f;
    p->safe_min_w     = 10.0f;
}

void load_model_init(load_model_t *m, const load_params_t *p, uint8_t scenario)
{
    m->p = *p;
    m->di_step   = p->slew_a_per_ms * (p->ts * 1000.0f);        /* A/ms * Ts[ms] = 0.05 A/paso */
    m->k_i       = 1.0f - expf(-p->ts / p->tau_i_s);
    m->k_v       = 1.0f - expf(-p->ts / p->tau_v_s);
    m->c_over_ts = p->c_in_f / p->ts;
    m->day_ms    = (uint32_t)(p->day_s * 1000.0f + 0.5f);
    m->t_ms      = (uint32_t)((float)m->day_ms * (MICRORED_DAY_START_H / 24.0f));   /* hora simulada de arranque */
    m->noise_t_ms = 0; m->ext_age_ms = p->ext_timeout_ms + 1u;   /* sin EXT al arrancar */
    m->rng = p->seed ? p->seed : 1u;
    m->scenario = scenario ? 1u : 0u;
    m->mode_cmd = 0; m->mode_eff = 0; m->hb_lost = 0; m->open = 0; m->prot = 0;
    m->level_max = 1.0f; m->level = 1.0f;
    m->noise = 0.0f; m->ext_v = p->v_nom;
    m->vbus = p->v_nom;
    m->i_cmd = 0.0f;
    m->i_slew = 0.0f; m->i_load = 0.0f; m->i_cap = 0.0f; m->vbus_f = p->v_nom;
    m->phase = 0.0f; m->p_pu = 0.0f; m->fp = p->fixed_fp; m->eta = load_eta(m->fp);
    m->p_prof_w = 0.0f; m->p_req_dc_w = 0.0f;
    load_model_slow(m, 0u);       /* deja p_pu, FP, i_cmd coherentes desde el primer instante */
}

void load_model_set_mode(load_model_t *m, uint8_t mode, uint16_t a_pm, uint16_t b_scn)
{
    if (mode <= 2u) {
        m->mode_cmd = mode;
        if (mode == 1u) m->level_max = clampf((float)a_pm * 0.001f, 0.0f, 1.0f);
    }
    if (b_scn != 0xFFFFu && b_scn <= 1u) m->scenario = (uint8_t)b_scn;
}

void load_model_set_vbus_dV(load_model_t *m, uint16_t dV_raw)
{
    float v = (float)(int16_t)dV_raw * 0.1f;
    m->ext_v = clampf(v, 0.0f, 60.0f);       /* 60 V: limite de v_mV (uint16) con margen */
    m->ext_age_ms = 0u;
}

void load_model_set_hb_lost(load_model_t *m, uint8_t lost) { m->hb_lost = lost ? 1u : 0u; }

void load_model_slow(load_model_t *m, uint32_t dt_ms)
{
    const load_params_t *p = &m->p;
    float dt = (float)dt_ms * 1.0e-3f;

    /* 1) tiempo del dia comprimido y edad del ultimo EXT */
    if (m->ext_age_ms < 0x7FFFFFFFu) m->ext_age_ms += dt_ms;
    float pu, fp;
    if (m->day_ms > 0u) {
        m->t_ms += dt_ms;
        while (m->t_ms >= m->day_ms) m->t_ms -= m->day_ms;
        m->phase = (float)m->t_ms / (float)m->day_ms;            /* 0..1 */
        float pos = m->phase * 24.0f;
        /* ruido gaussiano renovado cada noise_ms (determinista) */
        m->noise_t_ms += dt_ms;
        if (m->noise_t_ms >= p->noise_ms || dt_ms == 0u) {
            m->noise_t_ms = 0u;
            m->noise = p->sigma_pu * gauss(&m->rng);
        }
        pu = clampf(interp24(k_pu[m->scenario], pos) + m->noise, 0.02f, 1.0f);
        fp = interp24(k_fp[m->scenario], pos);
    } else {
        m->phase = 0.0f;
        pu = p->fixed_pu;
        fp = p->fixed_fp;
    }
    m->p_pu = pu; m->fp = fp; m->eta = load_eta(fp);
    m->p_prof_w   = pu * p->p_max_w;
    m->p_req_dc_w = m->p_prof_w / m->eta;

    /* 2) V_bus vigente (EXT con caducidad) */
    float v = (m->ext_age_ms > p->ext_timeout_ms) ? p->v_nom : m->ext_v;
    m->vbus = v;

    /* 3) proteccion de subtension con histeresis */
    if (!m->prot) { if (v < p->v_open) m->prot = 1u; }
    else          { if (v >= p->v_close) m->prot = 0u; }
    m->open = m->prot;

    /* 4) modo efectivo y tope de nivel */
    m->mode_eff = m->hb_lost ? 2u : m->mode_cmd;
    float cap = 1.0f;
    if (m->mode_eff == 1u) {
        cap = m->level_max;
    } else if (m->mode_eff == 2u) {
        cap = p->safe_frac;
        if (m->p_prof_w > 1.0e-3f) {
            float c2 = p->safe_min_w / m->p_prof_w;
            if (c2 > cap) cap = c2;
        }
        if (cap > 1.0f) cap = 1.0f;
    }

    /* 5) rampa del nivel (50 %/s) */
    float st = p->level_rate * dt;
    float e = cap - m->level;
    m->level += clampf(e, -st, st);
    if (dt_ms == 0u) m->level = cap;          /* init: sin rampa */

    /* 6) consigna de corriente: potencia constante */
    float vd = (v < 10.0f) ? 10.0f : v;
    m->i_cmd = m->open ? 0.0f : (m->level * m->p_req_dc_w) / vd;
}

/* ISR 10 kHz: slew + filtro + capacitor. Solo +,-,*, comparaciones. Sin div / expf / sqrt. */
void load_model_fast(load_model_t *m)
{
    float e = m->i_cmd - m->i_slew;
    if (e >  m->di_step) e =  m->di_step;
    if (e < -m->di_step) e = -m->di_step;
    m->i_slew += e;
    m->i_load += m->k_i * (m->i_slew - m->i_load);

    float dv = m->k_v * (m->vbus - m->vbus_f);       /* cambio de la tension en el capacitor este paso */
    m->vbus_f += dv;
    m->i_cap = m->c_over_ts * dv;                    /* i = C dv/dt (se suma a la corriente de entrada) */
}

static uint16_t u16r(float x) { return (uint16_t)(clampf(x, 0.0f, 65535.0f) + 0.5f); }

void load_model_fill_tel(const load_model_t *m, float i_in, uint8_t overrun,
                         sub_plant_tel_t *pl, cvl_tel_t *cv, sub_extra_tel_t *ex, sub_ctrl_tel_t *ct)
{
    if (i_in < 0.0f) i_in = 0.0f;
    float v   = m->vbus;
    float lvl = m->open ? 0.0f : m->level;
    int16_t i_mA = (int16_t)(clampf(i_in * 1000.0f, 0.0f, 32000.0f) + 0.5f);
    float p_W = v * i_in;

    pl->v_mV = u16r(v * 1000.0f);
    pl->i_mA = i_mA;
    pl->p_mW = (int32_t)(p_W * 1000.0f + 0.5f);
    pl->status = overrun ? SUB_ST_OVERRUN : (m->hb_lost ? SUB_ST_NOHB : (m->open ? SUB_ST_IDLE : SUB_ST_OK));

    uint8_t fl = 0;
    if (!m->open && (1.0f - m->level) > 0.01f) fl |= CVL_F_LIMITING;
    if (m->mode_eff == 2u) fl |= CVL_F_SAFE;
    if (m->open)           fl |= CVL_F_IDLE;
    if (m->prot)           fl |= CVL_F_PROT;
    if (overrun)           fl |= CVL_F_OVERRUN;
    cv->vin_mV   = pl->v_mV;
    cv->vbus_mV  = pl->v_mV;
    cv->ibus_mA  = (int16_t)(-i_mA);
    cv->duty_1e4 = u16r(lvl * 10000.0f);
    cv->flags    = fl;

    ex->x1 = (int16_t)(lvl * 1000.0f + 0.5f);
    ex->x2 = (int16_t)(m->fp * 1000.0f + 0.5f);
    ex->x3 = (int16_t)(m->phase * 1000.0f + 0.5f);
    ex->x4 = (int16_t)m->scenario;
    ex->xflags = 0;

    ct->ref_mV  = 0;
    ct->act_1e4 = cv->duty_1e4;
    ct->p_dW    = (int16_t)(clampf(p_W * 10.0f, 0.0f, 32000.0f) + 0.5f);
    ct->lim_dW  = u16r(m->p_req_dc_w * 10.0f);
    ct->mode    = m->mode_eff;
}
