/*
 * pv_plant.h — Planta emulada del subsistema PV (corre en la STM "M").
 *
 *   Panel (modelo de un diodo, Renogy RNG-100D-SS) -> C_in -> boost promediado -> bus rígido
 *
 * Portable (sin HAL). Se llama una vez por periodo de planta (dt) con el duty que mide M
 * de la PWM de C. Devuelve V_pv e I_pv, que M saca por sus DAC hacia el ADC de C.
 *
 * Es el mismo modelo que se validó previamente en simulación de PC.
 */
#ifndef PV_PLANT_H
#define PV_PLANT_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    /* panel (se recalculan en pv_plant_set_irradiance) */
    float Iph, I0, Vt;
    float Rs, Rsh;
    float G, Tcell;

    /* convertidor boost promediado */
    float L, C, rL, Vbus;

    /* estado */
    float v;        /* tensión del panel / C_in [V]   */
    float iL;       /* corriente del inductor [A]     */
    float ipv;      /* corriente del panel [A]        */
    float dt;       /* periodo de integración [s]     */
    int   started;
} pv_plant_t;

/* dt en segundos (p. ej. 1e-4 para 10 kHz). Deja L=470 uH, C=220 uF, rL=50 mOhm, Vbus=48 V. */
void  pv_plant_init(pv_plant_t *s, float dt);

/* Cambia la irradiancia [W/m2] y la temperatura ambiente [C]. Recalcula Iph, I0, Vt.
 * La primera llamada arranca el panel en circuito abierto. Llamar fuera de la ISR de
 * planta, o con las interrupciones deshabilitadas un instante (escribe varios campos). */
void  pv_plant_set_irradiance(pv_plant_t *s, float G, float Tamb);

/* Un paso de integración con el duty medido (0..1). */
void  pv_plant_step(pv_plant_t *s, float duty);

/* Corriente del panel a tensión v con 'iters' iteraciones de Newton (validación / arranque). */
float pv_plant_panel_current(const pv_plant_t *s, float v, float i_guess, int iters);

/* Tensión de circuito abierto con la irradiancia actual. */
float pv_plant_voc(const pv_plant_t *s);

#ifdef __cplusplus
}
#endif

#endif /* PV_PLANT_H */
