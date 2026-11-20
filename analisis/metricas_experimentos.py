"""Metricas de P0 y E1-E6 a partir de los CSV del registrador (logger_raw.py).

Uso (desde la raiz del repositorio):  python analisis/metricas_experimentos.py
Imprime las cifras que usa el informe y las guarda en docs/metricas_experimentos.txt.
Tiempos = suma de intervalos entre muestras del topic microred_ctrl/estado (~1 Hz).
"""
import json, io, contextlib
import numpy as np, pandas as pd

ARCH = {"P0": "datos/P0_demo.csv", "E1": "datos/E1.csv", "E2": "datos/E2.csv", "E3 A (sin SCADA)": "datos/E3_A.csv",
        "E3 B (con SCADA)": "datos/E3_B.csv", "E4": "datos/E4.csv", "E5": "datos/E5.csv", "E6": "datos/E6.csv"}

def leer(path, topic):
    d = pd.read_csv(path); d = d[d.topic == topic]
    r = pd.DataFrame([json.loads(p) for p in d.payload]); r["t"] = d.t_rel_s.values
    return r

def eventos(path):
    d = pd.read_csv(path); d = d[d.topic == "microred_ctrl/sistema"]
    return [(t, json.loads(p)) for t, p in zip(d.t_rel_s, d.payload)]

def pct(x): return f"{100*x:.1f} %"

def resumen(nombre, path):
    r = leer(path, "microred_ctrl/estado"); t = r.t.values
    dt = np.diff(t, prepend=t[0]); dt[0] = 0; T = t[-1] - t[0]
    fuera = ((r.vbus > 50.4) | (r.vbus < 45.6)).values
    print(f"\n== {nombre} ({path}) ==")
    print(f"duracion {T:.0f} s | Vbus en banda {pct(1-dt[fuera].sum()/T)} | Vbus max {r.vbus.max():.2f} V | "
          f"tiempo >53 V {dt[(r.vbus>53).values].sum():.0f} s ({pct(dt[(r.vbus>53).values].sum()/T)})")
    print(f"SOC inicial {r.soc.iloc[0]:.1f} % | SOC min {r.soc.min():.1f} % (t={t[r.soc.values.argmin()]:.0f} s) | "
          f"bateria descargando {pct(dt[(r.pbat>0).values].sum()/T)} | recorte max {r.recorte_w.max():.0f} W")
    print(f"muestras: " + ", ".join(f"{k} {pct(v)}" for k, v in r.estado.value_counts(normalize=True).items()))
    ev = eventos(path)
    for tt, p in ev:
        if "evento" in p and any(k in p["evento"] for k in ("DESLASTRE", "RESTAURAR", "S1 ENTRADA", "S4 LIBERAR")):
            print(f"  t={tt:7.1f} s  {p['evento'][:90]}")
        if "blackstart" in p: print(f"  t={tt:7.1f} s  arranque en negro paso {p['blackstart']}: {p['txt']}")
    n = sum(1 for _, p in ev if "evento" in p and "ESCALON ARRIBA" in p["evento"])
    n0 = sum(1 for _, p in ev if "evento" in p and "ESCALON ARRIBA" in p["evento"] and "+0 W" in p["evento"])
    if n: print(f"  escalones S2: {n} (de ellos {n0} de +0 W)")

buf = io.StringIO()
with contextlib.redirect_stdout(buf):
    for n, p in ARCH.items(): resumen(n, p)
    pv = leer("datos/E6.csv", "microred/pv"); i = pv.t.diff().idxmax()
    print(f"\n== E6: hueco de telemetria del PV: ultima lectura t={pv.t.iloc[i-1]:.2f} s ({pv.p.iloc[i-1]:.1f} W), "
          f"vuelve t={pv.t.iloc[i]:.2f} s ({pv.t.iloc[i]-pv.t.iloc[i-1]:.1f} s sin datos)")
    d = pd.read_csv("datos/E6.csv"); d = d[d.topic == "microred_ctrl/pv_online"]
    print("   pv_online:", ", ".join(f"{p} en t={t:.2f} s" for t, p in zip(d.t_rel_s, d.payload)))
print(buf.getvalue()); open("docs/metricas_experimentos.txt", "w", encoding="utf8").write(buf.getvalue())
