/*
 * wind_model.c — ver wind_model.h. Portable (sin HAL). La ISR no usa expf/powf/sinf ni divisiones propias
 * (la unica division por paso es la del feedforward dentro de pv_ctrl_step).
 */
#include "wind_model.h"
#include <math.h>

static float clampf(float x, float lo, float hi) { return (x < lo) ? lo : ((x > hi) ? hi : x); }

/* ------------------------------------------------------------------------------------------ */
/* Fuente: viento -> frecuencia -> Thevenin                                                    */
/* ------------------------------------------------------------------------------------------ */
float wind_fref(float vw)
{
    if (vw < WIND_CUT_IN_MS || vw > WIND_CUT_OUT_MS) return 0.0f;
    if (vw >= WIND_NOM_MS) return WIND_F_NOM_HZ;
    return WIND_F_NOM_HZ * (vw - WIND_CUT_IN_MS) / (WIND_NOM_MS - WIND_CUT_IN_MS);   /* 3.1875 Hz por m/s */
}

float wind_voc(float f)
{
    /* Voc = Ke*f - Vd; el offset (-Vd) se lleva a 0 linealmente bajo WIND_F_KNEE para que Voc(0) = 0 */
    float k = (f < WIND_F_KNEE) ? (f / WIND_F_KNEE) : 1.0f;
    float v = WIND_KE * f - WIND_VD * k;
    return (v > 0.0f) ? v : 0.0f;
}

float wind_req(float f) { return WIND_R0 + WIND_KR * f; }

float wind_pmax_ideal(float f)
{
    float v = wind_voc(f);
    return v * v / (4.0f * wind_req(f));
}

/* ------------------------------------------------------------------------------------------ */
/* Modelo                                                                                      */
/* ------------------------------------------------------------------------------------------ */
void wind_init(wind_t *w, float dt)
{
    w->dt   = dt;
    w->L    = 1.0e-3f;      /* ver nota de estabilidad en el resumen: ζ ≈ Z0/(2 Req), Z0 = sqrt(L/C) */
    w->C    = 470.0e-6f;
    w->rL   = 0.05f;
    w->Vbus = 48.0f;
    w->tau_f = 2.0f;
    w->dt_L = dt / w->L;
    w->dt_C = dt / w->C;

    w->vw = 0.0f; w->f_ref = 0.0f; w->f = 0.0f;
    w->voc = wind_voc(0.0f);
    w->inv_req = 1.0f / wind_req(0.0f);
    w->state = WIND_CALM;

    w->v = 0.0f; w->iL = 0.0f; w->isrc = 0.0f; w->duty = 0.0f; w->active = 0u;

    w->cmd_mode = 0u; w->cmd_pmax_dW = 0u; w->hb_lost = 0u;
}

void wind_update(wind_t *w, float vw, float dt_s)
{
    w->vw    = vw;
    w->f_ref = wind_fref(vw);
    float a = dt_s / w->tau_f;
    if (a > 1.0f) a = 1.0f;
    w->f += a * (w->f_ref - w->f);

    w->voc     = wind_voc(w->f);
    w->inv_req = 1.0f / wind_req(w->f);

    /* maquina de estados con histeresis de reconexion */
    uint8_t s = w->state;
    if (vw > WIND_CUT_OUT_MS) {
        s = WIND_OVERSPEED;
    } else if (s == WIND_RUN) {
        if (vw < WIND_CUT_IN_MS) s = WIND_CALM;
    } else if (s == WIND_CALM) {
        if (vw >= WIND_CUT_IN_MS + WIND_HYST_MS) s = WIND_RUN;
    } else { /* OVERSPEED: solo vuelve cuando el viento baja de cut-out - histeresis */
        if (vw <= WIND_CUT_OUT_MS - WIND_HYST_MS) {
            s = (vw < WIND_CUT_IN_MS) ? WIND_CALM : WIND_RUN;
        }
    }
    w->state   = s;
}

/* Limite superior de V_ref ligado a la tension en vacio. */
static void update_vref_bounds(const wind_t *w, pv_ctrl_t *c)
{
    float hi = WIND_VREF_HI_FRAC * w->voc;
    if (hi > WIND_VREF_MAX) hi = WIND_VREF_MAX;
    if (hi < c->p.vref_min + 0.5f) hi = c->p.vref_min + 0.5f;
    c->p.vref_max = hi;
}

/* Reanudacion: V_ref sensata (0.9*Voc, acotada), integrador a cero, P&O reiniciado.
 * Arrancar cerca de Voc limita el pico de corriente al conectar; el P&O baja hacia el MPP a 10 V/s. */
static void ctrl_restart(const wind_t *w, pv_ctrl_t *c)
{
    update_vref_bounds(w, c);
    const pv_params_t *p = &c->p;
    c->vref   = clampf(WIND_VREF_HI_FRAC * w->voc, p->vref_min, p->vref_max);
    c->integ  = 0.0f;
    c->duty   = clampf(1.0f - c->vref / p->v_bus_nom, p->d_min, p->d_max);
    c->p_prev = 0.0f;
    c->dir    = 1.0f;
    c->acc_v  = 0.0f;
    c->acc_i  = 0.0f;
    c->acc_n  = 0u;
    c->p_avg  = 0.0f;
}

void wind_plant_step(wind_t *w, float d)
{
    /* boost promediado CCM, Euler semi-implicito. Con V_in > V_bus y D~0 el diodo conduce
     * y el mismo modelo lo representa (iL crece por (V_in - V_bus)/L); iL >= 0 por el diodo. */
    w->iL += w->dt_L * (w->v - (1.0f - d) * w->Vbus - w->rL * w->iL);
    if (w->iL < 0.0f) w->iL = 0.0f;
    /* fuente Thevenin + diodos del puente: I = (Voc - V)/Req, nunca negativa */
    float is = (w->voc - w->v) * w->inv_req;
    w->isrc = (is > 0.0f) ? is : 0.0f;
    w->v += w->dt_C * (w->isrc - w->iL);
    if (w->v < 0.0f) w->v = 0.0f;
}

float wind_isr_step(wind_t *w, pv_ctrl_t *c)
{
    float d;
    if (w->state == WIND_RUN) {
        if (!w->active) { ctrl_restart(w, c); w->active = 1u; }

        d = pv_ctrl_step(c, w->v, w->isrc);                 /* mide (V, I) del paso anterior */

        wind_plant_step(w, d);
    } else {
        /* parado: fuente desconectada, D = 0, MPPT detenido. El condensador sigue a la tension en vacio. */
        w->active = 0u;
        d = 0.0f;
        w->iL = 0.0f; w->isrc = 0.0f; w->v = w->voc;
    }
    w->duty = d;
    return d;
}

void wind_on_cmd(wind_t *w, uint8_t cmd, uint8_t mode, uint16_t a)
{
    if (cmd != SUB_CMD_SETMODE || mode > (uint8_t)PV_MODE_SAFE) return;   /* SUB_CMD_EXT no aplica al eolico */
    w->cmd_pmax_dW = a;
    w->cmd_mode    = mode;
}

void wind_ctrl_params(pv_params_t *p, float ts_ctrl)
{
    pv_params_default(p);
    p->ts_ctrl   = ts_ctrl;
    p->ts_mppt   = 20.0e-3f;
    p->kp        = 0.02f;
    p->ki        = 2.0f;
    p->v_bus_nom = 48.0f;
    p->d_min     = 0.04f;    /* V_in max en regimen ~46 V (< 48 V) */
    p->d_max     = 0.92f;    /* V_in min ~3.8 V                     */
    p->vref_min  = 4.0f;
    p->vref_max  = WIND_VREF_MAX;
    p->vref_step = 0.2f;
    p->vref_init = 20.0f;
    p->lim_gain  = -0.05f;   /* NEGATIVA: se limita por el lado de BAJA tension (ver resumen) */
    p->lim_band  = 0.05f;
}

pv_mode_t wind_mode_eff(const wind_t *w)
{
    return w->hb_lost ? PV_MODE_SAFE : (pv_mode_t)w->cmd_mode;
}

void wind_supervise(wind_t *w, pv_ctrl_t *c)
{
    update_vref_bounds(w, c);
    /* Anti-atasco del P&O: pegado a un limite, V_ref ya no cambia, P no baja y pv_control nunca invierte el
     * sentido (solo invierte si p < p_prev). Se fuerza el sentido hacia dentro del rango. */
    if (c->vref <= c->p.vref_min + 1.0e-3f && c->dir < 0.0f) c->dir = 1.0f;
    if (c->vref >= c->p.vref_max - 1.0e-3f && c->dir > 0.0f) c->dir = -1.0f;
    pv_mode_t m = wind_mode_eff(w);
    float pmax = (float)w->cmd_pmax_dW * 0.1f;
    if (c->mode != m || c->p_max != pmax) pv_ctrl_set_mode(c, m, pmax);
}

/* ------------------------------------------------------------------------------------------ */
/* Viento sintetico                                                                            */
/* ------------------------------------------------------------------------------------------ */
#define WP_MEAN      8.0f
#define WP_SLOW_AMP  3.5f
#define WP_SLOW_MS   ((uint32_t)MICRORED_DAY_S * 1000u)   /* una oscilacion lenta por dia simulado */
#define WP_TURB_SIG  1.2f
#define WP_TURB_ALPHA 0.02f          /* muestreo a 10 Hz, tau = 5 s  (ancho de banda de ruido ≈ media movil de 10 s) */
#define WP_GUST_PERIOD_MS 300000u
#define WP_GUST_START_MS  292000u    /* dura 8 s al final de cada periodo, rampas de 1 s */
#define WP_GUST_MS        27.0f

static uint32_t xorshift32(uint32_t *s)
{
    uint32_t x = *s;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    *s = x;
    return x;
}

static float gauss_approx(uint32_t *s)      /* Irwin-Hall n=4: media 0, varianza 1 */
{
    float acc = 0.0f;
    for (int i = 0; i < 4; i++) acc += (float)(xorshift32(s) >> 8) * (1.0f / 16777216.0f);
    return (acc - 2.0f) * 1.7320508f;       /* sqrt(12/4) */
}

void wind_profile_init(wind_profile_t *p, uint32_t seed)
{
    p->rng = seed ? seed : 2463534242u;
    p->turb = 0.0f;
    p->t_ms = 0u;
    p->next_n_ms = 0u;
}

float wind_profile(wind_profile_t *p, uint32_t t_ms, int gust_on)
{
    while ((int32_t)(t_ms - p->next_n_ms) >= 0) {
        const float sig_in = WP_TURB_SIG * sqrtf((2.0f - WP_TURB_ALPHA) / WP_TURB_ALPHA);   /* solo a 10 Hz */
        p->turb += WP_TURB_ALPHA * (sig_in * gauss_approx(&p->rng) - p->turb);
        p->next_n_ms += 100u;
    }
    p->t_ms = t_ms;

    float ph = (float)(t_ms % WP_SLOW_MS) * (6.2831853f / (float)WP_SLOW_MS) - 3.14159265f;   /* maximo a las 18 h simuladas, minimo a las 6 h */
    float v  = WP_MEAN + WP_SLOW_AMP * sinf(ph) + p->turb;
    v = clampf(v, 0.0f, 28.0f);

    if (gust_on) {
        uint32_t tp = t_ms % WP_GUST_PERIOD_MS;
        if (tp >= WP_GUST_START_MS) {
            uint32_t te = tp - WP_GUST_START_MS;                  /* 0..8000 */
            float s = 1.0f;
            if (te < 1000u)        s = (float)te * 1.0e-3f;
            else if (te > 7000u)   s = (float)(8000u - te) * 1.0e-3f;
            v += (WP_GUST_MS - v) * s;
        }
    }
    return v;
}

/* ------------------------------------------------------------------------------------------ */
/* Telemetria                                                                                  */
/* ------------------------------------------------------------------------------------------ */
static uint16_t sat_u16(float x) { return (x <= 0.0f) ? 0u : ((x >= 65535.0f) ? 65535u : (uint16_t)(x + 0.5f)); }
static int16_t  sat_i16(float x)
{
    if (x >= 32767.0f)  return 32767;
    if (x <= -32768.0f) return -32768;
    return (int16_t)(x >= 0.0f ? x + 0.5f : x - 0.5f);
}

uint8_t wind_conv_flags(const wind_t *w, const pv_ctrl_t *c, uint8_t overrun)
{
    uint8_t fl = 0u;
    const pv_params_t *p = &c->p;
    if (w->active) {
        if (w->duty >= p->d_max - 1.0e-4f) fl |= CVL_F_DUTY_MAX;
        if (w->duty <= p->d_min + 1.0e-4f) fl |= CVL_F_DUTY_MIN;
        if (c->mode == PV_MODE_LIMITED && c->p_avg > c->p_max * (1.0f - p->lim_band)) fl |= CVL_F_LIMITING;
        if (c->vref <= p->vref_min + 1.0e-3f || c->vref >= p->vref_max - 1.0e-3f)     fl |= CVL_F_VREF_LIMIT;
    }
    if (overrun)                                   fl |= CVL_F_OVERRUN;
    if (c->mode == PV_MODE_SAFE || w->hb_lost)     fl |= CVL_F_SAFE;
    if (w->state == WIND_CALM)                     fl |= CVL_F_IDLE;
    if (w->state == WIND_OVERSPEED)                fl |= CVL_F_PROT;
    return fl;
}

void wind_build_tel(const wind_t *w, const pv_ctrl_t *c, uint8_t overrun,
                    sub_plant_tel_t *p, cvl_tel_t *cv, sub_extra_tel_t *x, sub_ctrl_tel_t *k)
{
    float v = w->v, i = w->isrc, d = w->duty;
    float pw = v * i;
    uint8_t mode = (uint8_t)(w->hb_lost ? PV_MODE_SAFE : c->mode);

    /* planta. v_mV es uint16: satura en 65.5 V (Voc llega a 72.8 V con la turbina parada tras sobreviento) */
    p->v_mV   = sat_u16(v * 1000.0f);
    p->i_mA   = sat_i16(i * 1000.0f);
    p->p_mW   = (int32_t)(pw * 1000.0f + 0.5f);
    p->status = overrun ? SUB_ST_OVERRUN : (w->hb_lost ? SUB_ST_NOHB : (w->state != WIND_RUN ? SUB_ST_IDLE : SUB_ST_OK));
    p->checksum = 0u;

    /* convertidor */
    float ibus = (1.0f - d) * w->iL;
    cv->vin_mV   = sat_u16(v * 1000.0f);
    cv->vbus_mV  = sat_u16(w->Vbus * 1000.0f);
    cv->ibus_mA  = sat_i16((ibus < 0.0f ? 0.0f : ibus) * 1000.0f);
    cv->duty_1e4 = sat_u16(d * 10000.0f);
    cv->flags    = wind_conv_flags(w, c, overrun);
    cv->checksum = 0u;

    /* extra: viento [cm/s], f [cHz], rpm, V_dc [dV] */
    x->x1 = sat_i16(w->vw * 100.0f);
    x->x2 = sat_i16(w->f * 100.0f);
    x->x3 = sat_i16(w->f * WIND_RPM_PER_HZ);
    x->x4 = sat_i16(v * 10.0f);
    x->xflags = 0u;
    x->checksum = 0u;

    /* control */
    k->ref_mV  = sat_u16(c->vref * 1000.0f);
    k->act_1e4 = sat_u16(d * 10000.0f);
    k->p_dW    = sat_i16(pw * 10.0f);
    k->lim_dW  = (c->mode == PV_MODE_LIMITED) ? sat_u16(c->p_max * 10.0f) : 0u;
    k->mode    = mode;
    k->checksum = 0u;
}
