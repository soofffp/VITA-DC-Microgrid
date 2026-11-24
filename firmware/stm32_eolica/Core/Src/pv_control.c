#include "pv_control.h"

static float clampf(float x, float lo, float hi)
{
    return (x < lo) ? lo : ((x > hi) ? hi : x);
}

void pv_params_default(pv_params_t *p)
{
    p->ts_ctrl   = 1.0e-4f;   /* lazo de V a 10 kHz (PWM a 20 kHz)                 */
    p->ts_mppt   = 20.0e-3f;  /* MPPT cada 20 ms: mucho mayor que el asentamiento  */
    p->kp        = 0.004f;    /* duty / V                                          */
    p->ki        = 4.0f;      /* duty / (V*s)                                      */
    p->v_bus_nom = 48.0f;
    p->d_min     = 0.35f;     /* con 48 V: V_pv maximo ~31 V (> Voc)               */
    p->d_max     = 0.85f;     /* V_pv minimo ~7 V                                  */
    p->vref_min  = 12.0f;
    p->vref_max  = 23.8f;
    p->vref_step = 0.2f;
    p->vref_init = 19.4f;     /* ~0.8 * 24.3 V                                     */
    p->lim_gain  = 0.01f;     /* V/W: ganancia por actualizacion < 1/|dP/dV|       */
    p->lim_band  = 0.05f;
}

void pv_ctrl_init(pv_ctrl_t *c, const pv_params_t *p)
{
    c->p      = *p;
    c->mode   = PV_MODE_MPPT;
    c->p_max  = 0.0f;

    c->vref   = clampf(p->vref_init, p->vref_min, p->vref_max);
    c->integ  = 0.0f;
    c->duty   = clampf(1.0f - c->vref / p->v_bus_nom, p->d_min, p->d_max);

    c->p_prev = 0.0f;
    c->dir    = 1.0f;
    c->acc_v  = 0.0f;
    c->acc_i  = 0.0f;
    c->acc_n  = 0;
    c->mppt_div = (uint32_t)(p->ts_mppt / p->ts_ctrl + 0.5f);
    if (c->mppt_div < 1u) c->mppt_div = 1u;

    c->p_avg  = 0.0f;
}

void pv_ctrl_set_mode(pv_ctrl_t *c, pv_mode_t mode, float p_max_w)
{
    c->mode  = mode;
    c->p_max = p_max_w;
}

/* P&O con tope de potencia. Se ejecuta cada ts_mppt con V e I promediados. */
static void mppt_update(pv_ctrl_t *c, float p)
{
    c->p_avg = p;

    if (c->mode == PV_MODE_SAFE) {
        /* congelar V_ref; solo se actualiza la referencia de potencia previa */
        c->p_prev = p;
        return;
    }

    if (c->mode == PV_MODE_LIMITED && p > c->p_max * (1.0f - c->p.lim_band)) {
        /* Cerca o sobre el tope: regular P hacia p_max moviendo V_ref de forma
         * proporcional al error (paso acotado por vref_step). Se opera del lado de
         * mayor tension que el MPP, donde subir V baja P. Un paso fijo aqui produce
         * oscilacion grande porque la pendiente dP/dV es alta cerca de Voc. */
        float dv = c->p.lim_gain * (p - c->p_max);
        dv = clampf(dv, -c->p.vref_step, c->p.vref_step);
        c->vref   = clampf(c->vref + dv, c->p.vref_min, c->p.vref_max);
        c->dir    = (dv >= 0.0f) ? 1.0f : -1.0f;
        c->p_prev = p;
        return;
    }

    /* P&O clasico: si la potencia bajo, invertir el sentido de la perturbacion */
    if (p < c->p_prev) {
        c->dir = -c->dir;
    }

    c->vref = clampf(c->vref + c->dir * c->p.vref_step, c->p.vref_min, c->p.vref_max);
    c->p_prev = p;
}

float pv_ctrl_step(pv_ctrl_t *c, float v_pv, float i_pv)
{
    const pv_params_t *p = &c->p;

    /* --- MPPT lento: promedia V e I en la ventana y actualiza V_ref --- */
    c->acc_v += v_pv;
    c->acc_i += i_pv;
    c->acc_n++;
    if (c->acc_n >= c->mppt_div) {
        float n  = (float)c->acc_n;
        float vm = c->acc_v / n;
        float im = c->acc_i / n;
        c->acc_v = 0.0f;
        c->acc_i = 0.0f;
        c->acc_n = 0;
        mppt_update(c, vm * im);
    }

    /* --- Lazo rapido de tension: PI + feedforward del boost --- */
    float err    = v_pv - c->vref;                    /* >0 => subir D (baja V_pv) */
    float d_ff   = 1.0f - c->vref / p->v_bus_nom;
    float u_raw  = d_ff + p->kp * err + c->integ;
    float u      = clampf(u_raw, p->d_min, p->d_max);

    /* integracion condicional (anti-windup): no integrar si satura empujando mas */
    if ((u_raw == u) ||
        (u_raw > p->d_max && err < 0.0f) ||
        (u_raw < p->d_min && err > 0.0f)) {
        c->integ = clampf(c->integ + p->ki * p->ts_ctrl * err, -0.3f, 0.3f);
    }

    c->duty = u;
    return u;
}
