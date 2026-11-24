#include "pv_plant.h"
#include <math.h>

/* Módulo Renogy RNG-100D-SS (datasheet) y ajuste de Rs/Rsh (Villalva):
 * a STC reproduce Pmpp = 100.16 W en V = 20.40 V, I = 4.91 A. */
#define PV_ISC      5.21f
#define PV_VOC      24.3f
#define PV_NS       36.0f
#define PV_ALPHA    0.0026f      /* A/C */
#define PV_BETA    (-0.068f)     /* V/C */
#define PV_NOCT     47.0f
#define PV_IDEAL    1.3f         /* factor de idealidad del diodo */
#define PV_RS       0.09169248627752043f
#define PV_RSH      4716.142640298697f
#define K_OVER_Q    8.617333262e-5f   /* k/q [V/K] */

#define NEWTON_ITERS_RUN  3      /* con arranque en caliente (valor previo) alcanza */

void pv_plant_init(pv_plant_t *s, float dt)
{
    s->Rs   = PV_RS;
    s->Rsh  = PV_RSH;
    s->L    = 470.0e-6f;
    s->C    = 220.0e-6f;
    s->rL   = 0.05f;
    s->Vbus = 48.0f;
    s->dt   = dt;
    s->v    = 0.0f;
    s->iL   = 0.0f;
    s->ipv  = 0.0f;
    s->G    = 0.0f;
    s->Tcell = 25.0f;
    s->Iph  = 0.0f;
    s->I0   = 1.0e-9f;
    s->Vt   = 1.2f;
    s->started = 0;
}

float pv_plant_panel_current(const pv_plant_t *s, float v, float i_guess, int iters)
{
    float i = i_guess;
    for (int k = 0; k < iters; k++) {
        float arg = (v + i * s->Rs) / s->Vt;
        if (arg > 40.0f) arg = 40.0f;                 /* evita desbordar expf */
        float e  = expf(arg);
        float f  = s->Iph - s->I0 * (e - 1.0f) - (v + i * s->Rs) / s->Rsh - i;
        float df = -s->I0 * s->Rs / s->Vt * e - s->Rs / s->Rsh - 1.0f;
        i -= f / df;
    }
    return (i < 0.0f) ? 0.0f : i;                     /* el panel no absorbe corriente */
}

float pv_plant_voc(const pv_plant_t *s)
{
    float lo = 0.0f, hi = PV_VOC + 3.0f;
    for (int k = 0; k < 30; k++) {                    /* biseccion sobre I(V) = 0 */
        float mid = 0.5f * (lo + hi);
        float i   = pv_plant_panel_current(s, mid, s->Iph, 25);
        if (i > 1.0e-3f) lo = mid; else hi = mid;
    }
    return 0.5f * (lo + hi);
}

void pv_plant_set_irradiance(pv_plant_t *s, float G, float Tamb)
{
    if (G < 1.0f) G = 1.0f;
    float g  = G / 1000.0f;
    float T  = Tamb + G * (PV_NOCT - 20.0f) / 800.0f;
    float dT = T - 25.0f;
    float Vt = PV_IDEAL * PV_NS * K_OVER_Q * (T + 273.15f);

    s->G     = G;
    s->Tcell = T;
    s->Vt    = Vt;
    s->Iph   = g * ((s->Rsh + s->Rs) / s->Rsh * PV_ISC + PV_ALPHA * dT);
    s->I0    = (PV_ISC + PV_ALPHA * dT) / (expf((PV_VOC + PV_BETA * dT) / Vt) - 1.0f);

    if (!s->started) {
        s->v   = pv_plant_voc(s);
        s->ipv = 0.0f;
        s->started = 1;
    }
}

void pv_plant_step(pv_plant_t *s, float duty)
{
    if (duty < 0.0f) duty = 0.0f;
    if (duty > 1.0f) duty = 1.0f;

    /* Euler semi-implicito: primero iL con v actual, luego v con iL nueva */
    s->iL += s->dt / s->L * (s->v - (1.0f - duty) * s->Vbus - s->rL * s->iL);
    if (s->iL < 0.0f) s->iL = 0.0f;                   /* el diodo del boost no deja invertir */

    s->ipv = pv_plant_panel_current(s, s->v, s->ipv, NEWTON_ITERS_RUN);
    s->v  += s->dt / s->C * (s->ipv - s->iL);
    if (s->v < 0.0f) s->v = 0.0f;
}
