"""Indicadores de desempeno de alarmas (ANSI/ISA-18.2) sobre los CSV del registrador.

Uso (desde la raiz del repositorio):  python analisis/kpi_alarmas.py
Reproduce cada experimento por `scada_pc/alarmas.py` y calcula, por experimento:
  - alarmas por 10 min (referencia ISA-18.2: <= 1 aceptable, <= 2 manejable) y maximo en cualquier ventana de 10 min
  - fraccion del tiempo en inundacion (> 10 alarmas en 10 min)
  - alarmas con chattering (>= 3 activaciones del mismo tag en 1 min)
  - distribucion por prioridad y duracion de cada alarma
  - comparacion con contar todo cambio de estado del SCADA ("sin racionalizar")
Escribe docs/resultados_alarmas.txt, docs/alarmas_eventos.csv y docs/alarmas_tabla.md.

OJO: los experimentos estan acelerados (un dia = 360 s, el SOC avanza 60x). Una ventana de 10 min
de experimento equivale a unas 40 h simuladas, asi que estas cifras son una prueba de esfuerzo de
la logica de alarmas, no la carga real de un operador.
"""
import json, os, sys
import numpy as np, pandas as pd

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "scada_pc"))
from alarmas import MotorAlarmas, ALARMAS, POR_TAG, NOMBRE_PRIORIDAD, tabla_md

ARCH = {"P0": "datos/P0_demo.csv", "E1": "datos/E1.csv", "E2": "datos/E2.csv", "E3 A (sin SCADA)": "datos/E3_A.csv",
        "E3 B (con SCADA)": "datos/E3_B.csv", "E4": "datos/E4.csv", "E5": "datos/E5.csv", "E6": "datos/E6.csv"}
VENT = 600.0   # ventana de 10 min


def reproducir(path):
    d = pd.read_csv(path)
    est = d[d.topic == "microred_ctrl/estado"]
    online = d[d.topic.str.endswith("_online")]
    # linea de tiempo unica: avisos online y muestras de estado, en orden
    ev_on = [(t, tp.split("/")[1].replace("_online", ""), int(float(p))) for t, tp, p in zip(online.t_rel_s, online.topic, online.payload)]
    muestras = [(t, json.loads(p)) for t, p in zip(est.t_rel_s, est.payload)]
    eng, on, i, out = MotorAlarmas(), {}, 0, []
    for t, s in muestras:
        while i < len(ev_on) and ev_on[i][0] <= t:
            on[ev_on[i][1]] = ev_on[i][2]; i += 1
        out += eng.actualizar(t, s, on)
    # el sistema online puede cambiar tras la ultima muestra; se ignora a proposito
    T = muestras[-1][0] - muestras[0][0]
    estados = [s["estado"] for _, s in muestras]
    cambios = sum(1 for a, b in zip(estados, estados[1:]) if a != b)
    sis = d[d.topic == "microred_ctrl/sistema"]
    n_sis = sum(1 for p in sis.payload if "evento" in json.loads(p) and "AUTO" not in json.loads(p)["evento"])
    return out, T, cambios + n_sis, muestras[0][0]


def maximo_en_ventana(tiempos, T, w=VENT):
    if not tiempos: return 0
    tiempos = np.sort(np.array(tiempos)); return max(int(((tiempos >= t) & (tiempos < t + w)).sum()) for t in tiempos)


def tiempo_inundacion(tiempos, T, w=VENT, umbral=10):
    """Fraccion del tiempo en que la ventana de 10 min que termina en ese instante tiene > 10 activaciones."""
    if T <= 0: return 0.0
    tiempos = np.array(tiempos); xs = np.arange(0, T, 1.0)
    return float(np.mean([((tiempos > x - w) & (tiempos <= x)).sum() > umbral for x in xs]))


def main():
    L, filas, tot = [], [], dict(act=0, T=0.0, base=0)
    L.append("INDICADORES DE ALARMAS (ISA-18.2) - reproduccion sobre los CSV de datos/")
    L.append("Las pruebas estan aceleradas (1 dia = 360 s): son una prueba de esfuerzo, no carga de operador.\n")
    por_prio = {1: 0, 2: 0, 3: 0}
    for nombre, path in ARCH.items():
        ev, T, base, t0 = reproducir(path)
        acts = [(t - t0, tag, p) for t, tag, k, p in ev if k == "ACT"]
        rtn = {}
        dur = []
        pend = {}
        for t, tag, k, p in ev:
            if k == "ACT": pend[tag] = t
            elif tag in pend: dur.append((tag, t - pend.pop(tag)))
        for tag, t in pend.items(): dur.append((tag, (T + t0) - t))   # siguen activas al final
        n = len(acts); tt = [a[0] for a in acts]
        chat = [tag for tag in POR_TAG if sum(1 for t, g, _ in acts if g == tag and any(0 <= t2 - t < 60 for t2, g2, _ in acts if g2 == tag and t2 != t)) >= 3]
        for _, tag, p in acts: por_prio[p] += 1
        L.append(f"== {nombre}: {T:.0f} s ==")
        L.append(f"  alarmas activadas: {n} -> {n / (T / VENT):.1f} por 10 min | maximo en una ventana de 10 min: {maximo_en_ventana(tt, T)} "
                 f"| tiempo en inundacion (>10 en 10 min): {100 * tiempo_inundacion(tt, T):.1f} %")
        L.append(f"  sin racionalizar (cada cambio de estado y cada evento que publica el SCADA): {base} -> {base / (T / VENT):.1f} por 10 min")
        if acts:
            c = pd.Series([a[1] for a in acts]).value_counts()
            L.append("  por tag: " + ", ".join(f"{k} {v}" for k, v in c.items()))
        if dur:
            L.append("  duracion: " + ", ".join(f"{g} {d:.0f} s" for g, d in dur[:8]) + (" ..." if len(dur) > 8 else ""))
        L.append(f"  chattering (>=3 activaciones del mismo tag en 1 min): {', '.join(chat) if chat else 'ninguno'}")
        for t, tag, k, p in ev:
            filas.append((nombre, round(t - t0, 2), tag, k, NOMBRE_PRIORIDAD[p]))
        tot["act"] += n; tot["T"] += T; tot["base"] += base
        L.append("")
    L.append(f"TOTAL en todos los experimentos: {tot['act']} alarmas en {tot['T']:.0f} s -> {tot['act'] / (tot['T'] / VENT):.1f} por 10 min "
             f"(sin racionalizar: {tot['base'] / (tot['T'] / VENT):.1f} por 10 min)")
    s = sum(por_prio.values()) or 1
    L.append("Distribucion por prioridad de lo activado: " + ", ".join(f"{NOMBRE_PRIORIDAD[k]} {100 * v / s:.0f} %" for k, v in por_prio.items()))
    cfg = [sum(1 for a in ALARMAS if a.prioridad == k) for k in (1, 2, 3)]
    L.append(f"Alarmas configuradas: {len(ALARMAS)} (Alta {cfg[0]}, Media {cfg[1]}, Baja {cfg[2]}). Con tan pocas alarmas, la regla 80/15/5 de la norma no es significativa.")
    txt = "\n".join(L); print(txt)
    open("docs/resultados_alarmas.txt", "w", encoding="utf8").write(txt + "\n")
    pd.DataFrame(filas, columns=["experimento", "t_s", "tag", "evento", "prioridad"]).to_csv("docs/alarmas_eventos.csv", index=False)
    open("docs/alarmas_tabla.md", "w", encoding="utf8").write(tabla_md() + "\n")


if __name__ == "__main__":
    main()
