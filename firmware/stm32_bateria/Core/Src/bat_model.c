#include "bat_model.h"

/* ------------------------------------------------------------------------------------------ */
/* Utilidades                                                                                   */
/* ------------------------------------------------------------------------------------------ */
static inline float clampf(float x, float lo, float hi)
{
    return (x < lo) ? lo : ((x > hi) ? hi : x);
}

static inline float minf(float a, float b) { return (a < b) ? a : b; }

/* OCV por celda [V] vs SOC 0,10,...,100 % (datos del banco TRC18650 NMC) */
static const float k_ocv_cell[11] = {
    3.30f, 3.55f, 3.62f, 3.68f, 3.74f, 3.80f, 3.88f, 3.96f, 4.05f, 4.13f, 4.20f
};

float bat_ocv_pack(float soc_pct)
{
    float s = clampf(soc_pct, 0.0f, 100.0f) * 0.1f;     /* 0..10 */
    int   k = (int)s;
    if (k > 9) k = 9;
    float fr = s - (float)k;
    return 10.0f * (k_ocv_cell[k] + fr * (k_ocv_cell[k + 1] - k_ocv_cell[k]));
}

/* ------------------------------------------------------------------------------------------ */
/* Parametros e inicializacion                                                                  */
/* ------------------------------------------------------------------------------------------ */
void bat_params_default(bat_params_t *p)
{
    p->dt        = 1.0f / SUB_CTRL_HZ;
    p->q_ah      = 14.5f;
    p->r0_hi     = 0.08f;
    p->r0_lo     = 0.14f;
    p->r1        = 0.04f;
    p->c1        = 1000.0f;                 /* tau = 40 s */
    p->soc_accel = BAT_SOC_ACCEL;

    p->L         = 470.0e-6f;
    p->rL        = 0.05f;
    p->c_bus     = 2200.0e-6f;
    p->vbus_ref  = 48.0f;
    p->d_min     = 0.01f;
    p->d_max     = 0.98f;

    /* Exterior: planta ~ integrador (C_bus*dV/dt = k*I, k = V_pack/V_bus ~ 0.65..0.87).
     * wn = sqrt(k*ki/C) ~ 2*pi*93 Hz, zeta ~ 1.1.  Interior: wc = kp_i*V_bus/L ~ 2*pi*650 Hz (~7x el exterior).
     * Margenes medidos en test_bat/sweep: el exterior oscila al duplicar sus ganancias, el interior al duplicar kp_i. */
    p->kp_v      = 3.2f;
    p->ki_v      = 1000.0f;
    p->kb_v      = 100.0f;
    p->kp_i      = 0.04f;
    p->ki_i      = 20.0f;
    p->kb_i      = 2000.0f;

    p->uv_lo     = 43.5f;
    p->uv_hi     = 45.3f;
    p->ov_lo     = 51.0f;
    p->ov_hi     = 58.0f;

    p->i_nom     = 7.0f;
    p->i_safe    = 2.0f;
    p->i_abs     = 15.0f;
    p->soc_ch0   = 95.0f;
    p->soc_dis0  = 10.0f;
    p->soc_dis1  = 5.0f;
    p->vch0      = 41.8f;
    p->vch1      = 42.0f;
    p->vdis0     = 31.0f;
    p->vdis1     = 30.0f;
    p->t_der0    = 45.0f;
    p->t_der1    = 60.0f;
    p->f_alpha   = 0.15f;
    p->v_fault_lo = 28.0f;
    p->v_fault_hi = 43.0f;
    p->t_fault   = 60.0f;
    p->band      = 2.4f;

    p->t_amb     = 25.0f;
    p->rth       = 3.0f;
    p->cth       = 200.0f;                  /* tau = rth*cth = 600 s */
}

static void derive(bat_params_t *p)
{
    p->k_il   = p->dt / p->L;
    p->k_v1   = p->dt / p->c1;
    p->inv_r1 = 1.0f / p->r1;
    p->k_vb   = p->dt / p->c_bus;
    p->inv_uv = 1.0f / (p->uv_hi - p->uv_lo);
    p->inv_ov = 1.0f / (p->ov_hi - p->ov_lo);
}

static void acc_clear(bat_acc_t *a, float vb)
{
    a->sum_i = 0.0f; a->sum_i2 = 0.0f; a->n = 0u;
    a->vbus_min = vb; a->vbus_max = vb;
}

void bat_init(bat_t *m, float soc0_pct)
{
    bat_params_t *p = &m->p;
    bat_state_t  *s = &m->s;
    derive(p);

    s->soc   = clampf(soc0_pct, 0.0f, 100.0f);
    s->soc_c = 0.0f;
    s->temp  = p->t_amb;
    s->ocv   = bat_ocv_pack(s->soc);
    s->r0    = p->r0_hi + (p->r0_lo - p->r0_hi) * (1.0f - s->soc * 0.01f);

    s->iL = 0.0f; s->v1 = 0.0f;
    s->vpack = s->ocv;
    s->vbus  = p->vbus_ref;
    s->xi_v = 0.0f; s->xi_i = 0.0f;
    s->iref = 0.0f;
    s->duty  = 1.0f - s->vpack / s->vbus;      /* duty de equilibrio sin corriente */
    s->d_cmd = s->duty;
    s->sat = 0u;
    acc_clear(&s->acc, s->vbus);

    s->ich_eff = p->i_nom; s->idis_eff = p->i_nom;
    s->fv_ch = 1.0f; s->fv_dis = 1.0f;
    s->lim_nom_dis = p->i_nom;
    s->vref = p->vbus_ref;
    s->mode_eff = 0u;
    s->fault = 0u; s->hb_lost = 0u; s->ext_stale = 0u;
    s->cvl_flags = 0u; s->xflags = 0u;
    s->cmd_pack = 0u;
    s->p_eff = 0.0f;
    s->ext_ms = 0u;
}

/* ------------------------------------------------------------------------------------------ */
/* ISR 10 kHz: control + planta. Solo + - * / y comparaciones sobre floats (FPU de precision simple). */
/* ------------------------------------------------------------------------------------------ */
void bat_step(bat_t *m)
{
    const bat_params_t *p = &m->p;
    bat_state_t        *s = &m->s;
    const float dt = p->dt;

    /* ---- "mediciones" (estado del paso anterior; en hardware serian las lecturas del ADC) ---- */
    const float vbus_m = s->vbus;
    const float vpk_m  = s->vpack;
    const float il_m   = s->iL;

    /* ---- (1) lazo exterior PI de V_bus -> I_bat_ref (+ descarga), con saturacion y anti-windup por retrocalculo ---- */
    const float e_v  = p->vbus_ref - vbus_m;
    const float u    = p->kp_v * e_v + s->xi_v;
    const float lo   = -s->ich_eff;
    const float hi   =  s->idis_eff;
    float us = u;
    if (us > hi) us = hi;
    if (us < lo) us = lo;
    s->xi_v += dt * (p->ki_v * e_v + p->kb_v * (us - u));
    s->sat   = (uint8_t)((u > hi + 1.0e-3f) || (u < lo - 1.0e-3f));
    s->iref  = us;

    /* ---- (4) lazo interior PI de corriente del lado pack + feedforward D_ff = 1 - V_pack/V_bus ---- */
    const float vb  = (vbus_m < 10.0f) ? 10.0f : vbus_m;
    const float dff = 1.0f - vpk_m / vb;
    const float e_i = us - il_m;
    const float ud  = dff + p->kp_i * e_i + s->xi_i;
    float d = ud;
    if (d > p->d_max) d = p->d_max;
    if (d < p->d_min) d = p->d_min;
    s->xi_i += dt * (p->ki_i * e_i + p->kb_i * (d - ud));
    s->d_cmd = d;

#if BAT_DUTY_DELAY
    const float da = s->duty;          /* el duty calculado en este paso se aplica en el siguiente */
    s->duty = d;
#else
    const float da = d;
    s->duty = d;
#endif
    const float k1 = 1.0f - da;

    /* ---- planta: Euler semi-implicito (primero iL, luego v1 y V_bus con la iL nueva) ---- */
    const float vt = s->ocv - s->r0 * s->iL - s->v1;                 /* tension en bornes con la iL anterior */
    s->iL += p->k_il * (vt - k1 * s->vbus - p->rL * s->iL);
    s->v1 += p->k_v1 * (s->iL - s->v1 * p->inv_r1);
    s->vpack = s->ocv - s->r0 * s->iL - s->v1;

    /* resto de la microrred: potencia constante P_eff/V_bus con repliegue (los convertidores del resto
     * dejan de consumir con subtension y de inyectar con sobretension) */
    const float pe = s->p_eff;
    float fl;
    if (pe < 0.0f) fl = (s->vbus - p->uv_lo) * p->inv_uv;
    else           fl = (p->ov_hi - s->vbus) * p->inv_ov;
    if (fl < 0.0f) fl = 0.0f;
    if (fl > 1.0f) fl = 1.0f;
    const float vbg = (s->vbus < 10.0f) ? 10.0f : s->vbus;
    const float ig  = pe * fl / vbg;

    s->vbus += p->k_vb * (k1 * s->iL + ig);
    if (s->vbus < 1.0f)   s->vbus = 1.0f;                           /* solo proteccion numerica */
    if (s->vbus > 100.0f) s->vbus = 100.0f;

    /* ---- acumuladores para el lazo principal ---- */
    s->acc.sum_i  += s->iL;
    s->acc.sum_i2 += s->iL * s->iL;
    s->acc.n++;
    if (s->vbus < s->acc.vbus_min) s->acc.vbus_min = s->vbus;
    if (s->vbus > s->acc.vbus_max) s->acc.vbus_max = s->vbus;
}

/* ------------------------------------------------------------------------------------------ */
/* Comandos                                                                                     */
/* ------------------------------------------------------------------------------------------ */
void bat_set_mode(bat_t *m, uint8_t mode, uint16_t a_dA, uint16_t b_dA)
{
    if (mode > 2u) return;
    uint32_t a = (a_dA > 150u) ? 150u : a_dA;       /* [0, 15 A] */
    uint32_t b = (b_dA > 150u) ? 150u : b_dA;
    m->s.cmd_pack = ((uint32_t)mode << 16) | (a << 8) | b;     /* una sola escritura de 32 bits */
}

void bat_set_ext(bat_t *m, int16_t p_net_dW, uint32_t now_ms)
{
    m->s.p_eff  = (float)p_net_dW * 0.1f;
    m->s.ext_ms = now_ms;
}

/* ------------------------------------------------------------------------------------------ */
/* Lazo principal (~100 Hz)                                                                     */
/* ------------------------------------------------------------------------------------------ */
void bat_slow(bat_t *m, const bat_acc_t *acc, uint32_t now_ms, uint32_t ms_since_cmd)
{
    const bat_params_t *p = &m->p;
    bat_state_t        *s = &m->s;
    uint8_t xf = 0u;

    /* ---- SOC (Coulomb counting con suma de Kahan: los incrementos son ~1e-6 %, menores que el ulp de un float en 50 %) ---- */
    if (acc->n > 0u) {
        const float inv_n = 1.0f / (float)acc->n;
        const float dq    = acc->sum_i * p->dt;                                  /* A*s, + descarga */
        const float dsoc  = -dq * p->soc_accel * (100.0f / (3600.0f * p->q_ah));
        const float y = dsoc - s->soc_c;
        const float t = s->soc + y;
        s->soc_c = (t - s->soc) - y;
        s->soc   = t;
        if (s->soc > 100.0f) { s->soc = 100.0f; s->soc_c = 0.0f; }
        if (s->soc < 0.0f)   { s->soc = 0.0f;   s->soc_c = 0.0f; }

        /* termica de 1er orden: T -> T_amb + P_perdidas*Rth con tau = Rth*Cth */
        const float ploss = acc->sum_i2 * inv_n * (s->r0 + p->r1);
        const float tss   = p->t_amb + ploss * p->rth;
        float a = (float)acc->n * p->dt / (p->rth * p->cth);
        if (a > 1.0f) a = 1.0f;
        s->temp += a * (tss - s->temp);

        if (acc->vbus_min < p->vbus_ref - p->band || acc->vbus_max > p->vbus_ref + p->band) xf |= 0x01u;
    }

    /* ---- parametros dependientes del SOC ---- */
    s->ocv = bat_ocv_pack(s->soc);
    s->r0  = p->r0_hi + (p->r0_lo - p->r0_hi) * (1.0f - s->soc * 0.01f);

    /* ---- heartbeat y EXT obsoleto ---- */
    s->hb_lost   = (uint8_t)(ms_since_cmd > SUB_HB_TIMEOUT_MS);
    s->ext_stale = (uint8_t)((now_ms - s->ext_ms) > 3000u);
    if (s->ext_stale) s->p_eff = 0.0f;                                  /* sin EXT vigente: P_net := 0 */

    /* ---- modo efectivo y limites nominales ---- */
    const uint32_t cp = s->cmd_pack;
    uint8_t mode = (uint8_t)(cp >> 16);
    float ich_set, idis_set;
    if (s->hb_lost) mode = 2u;
    if (mode == 1u) {
        ich_set  = (float)((cp >> 8) & 0xFFu) * 0.1f;
        idis_set = (float)(cp & 0xFFu) * 0.1f;
    } else if (mode == 2u) {
        ich_set = p->i_safe; idis_set = p->i_safe;
    } else {
        ich_set = p->i_nom;  idis_set = p->i_nom;
    }
    s->mode_eff    = mode;
    s->lim_nom_dis = idis_set;

    /* ---- (3) derating por SOC / tension / temperatura ---- */
    const float f_soc_ch  = clampf((100.0f - s->soc) / (100.0f - p->soc_ch0), 0.0f, 1.0f);
    const float f_soc_dis = clampf((s->soc - p->soc_dis1) / (p->soc_dis0 - p->soc_dis1), 0.0f, 1.0f);
    const float f_v_ch    = clampf((p->vch1 - s->vpack) / (p->vch1 - p->vch0), 0.0f, 1.0f);
    const float f_v_dis   = clampf((s->vpack - p->vdis1) / (p->vdis0 - p->vdis1), 0.0f, 1.0f);
    /* La tension en bornes depende de la corriente => el derating por tension cierra un lazo; el filtro lo estabiliza */
    s->fv_ch  += p->f_alpha * (f_v_ch  - s->fv_ch);
    s->fv_dis += p->f_alpha * (f_v_dis - s->fv_dis);
    const float f_t = clampf((p->t_der1 - s->temp) / (p->t_der1 - p->t_der0), 0.0f, 1.0f);

    const float f_ch  = minf(minf(f_soc_ch,  s->fv_ch),  f_t);
    const float f_dis = minf(minf(f_soc_dis, s->fv_dis), f_t);
    s->ich_eff  = ich_set  * f_ch;
    s->idis_eff = idis_set * f_dis;

    const uint8_t der_ch  = (uint8_t)(f_ch  < 0.999f);
    const uint8_t der_dis = (uint8_t)(f_dis < 0.999f);
    if (der_ch)  xf |= 0x02u;
    if (der_dis) xf |= 0x04u;
    if (s->hb_lost)   xf |= 0x08u;
    if (s->ext_stale) xf |= 0x10u;

    /* ---- falla por limites duros ---- */
    s->fault = (uint8_t)((s->vpack < p->v_fault_lo) || (s->vpack > p->v_fault_hi) || (s->temp > p->t_fault));

    /* ---- banderas del convertidor ---- */
    uint8_t fl = 0u;
    if (s->d_cmd >= p->d_max - 1.0e-4f) fl |= CVL_F_DUTY_MAX;
    if (s->d_cmd <= p->d_min + 1.0e-4f) fl |= CVL_F_DUTY_MIN;
    if (s->sat)                          fl |= CVL_F_LIMITING;
    if (s->mode_eff == 2u)               fl |= CVL_F_SAFE;
    if (s->iL < 0.05f && s->iL > -0.05f) fl |= CVL_F_IDLE;
    if (der_ch || der_dis)               fl |= CVL_F_PROT;
    s->cvl_flags = fl;
    s->xflags    = xf;
}

/* ------------------------------------------------------------------------------------------ */
/* Telemetria                                                                                   */
/* ------------------------------------------------------------------------------------------ */
static int32_t rnd(float x) { return (int32_t)(x + ((x >= 0.0f) ? 0.5f : -0.5f)); }
static int16_t sat16(int32_t v) { return (int16_t)((v > 32767) ? 32767 : ((v < -32768) ? -32768 : v)); }
static uint16_t satu16(int32_t v) { return (uint16_t)((v > 65535) ? 65535 : ((v < 0) ? 0 : v)); }

void bat_build_tel(const bat_state_t *s, uint8_t overrun, sub_plant_tel_t *pl, cvl_tel_t *cv,
                   sub_extra_tel_t *ex, sub_ctrl_tel_t *ct)
{
    const float v = s->vpack, i = s->iL;
    const float ibus = (1.0f - s->duty) * i;

    pl->v_mV   = satu16(rnd(v * 1000.0f));
    pl->i_mA   = sat16(rnd(i * 1000.0f));
    pl->p_mW   = rnd(v * i * 1000.0f);
    pl->status = s->fault ? SUB_ST_FAULT : (overrun ? SUB_ST_OVERRUN : (s->hb_lost ? SUB_ST_NOHB : SUB_ST_OK));

    cv->vin_mV   = pl->v_mV;
    cv->vbus_mV  = satu16(rnd(s->vbus * 1000.0f));
    cv->ibus_mA  = sat16(rnd(ibus * 1000.0f));
    cv->duty_1e4 = satu16(rnd(s->duty * 10000.0f));
    cv->flags    = (uint8_t)(s->cvl_flags | (overrun ? CVL_F_OVERRUN : 0u));

    ex->x1 = sat16(rnd(s->soc * 10.0f));
    ex->x2 = sat16(rnd(s->temp * 10.0f));
    ex->x3 = sat16(rnd(s->ich_eff * 10.0f));
    ex->x4 = sat16(rnd(s->idis_eff * 10.0f));
    ex->xflags = s->xflags;

    ct->ref_mV  = satu16(rnd(s->vref * 1000.0f));
    ct->act_1e4 = cv->duty_1e4;
    ct->p_dW    = sat16(rnd(v * i * 10.0f));
    ct->lim_dW  = satu16(rnd(s->lim_nom_dis * 10.0f));
    ct->mode    = s->mode_eff;
}
