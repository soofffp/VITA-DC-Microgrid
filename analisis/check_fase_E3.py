"""
check_fase_E3.py -- verifica que dos corridas de E3 (A sin SCADA, B con SCADA) esten en fase.

Uso:   python analisis/check_fase_E3.py datos/E3_A.csv datos/E3_B.csv

Criterios de validez (ver docs/protocolos/protocolo_experimentos.md):
  - diferencia entre los amaneceres (PV cruza 5 W hacia arriba despues de t=200 s) <= 2 s
  - SOC en t=302 s de A y B dentro de 2 puntos
  - el PV entrega potencia en ambas corridas
"""
import sys, json
import pandas as pd

def cargar(path):
    d = pd.read_csv(path)
    pv = d[d.topic == "microred/pv"].copy()
    pv["p"] = pv.payload.map(lambda s: json.loads(s)["p"])
    est = d[d.topic == "microred_ctrl/estado"].copy()
    est["soc"] = est.payload.map(lambda s: json.loads(s)["soc"])
    return pv, est

def amanecer(pv):
    x = pv[pv.t_rel_s > 200]
    prev = x.p.shift(1)
    cruce = x[(x.p >= 5) & (prev < 5)]
    return None if cruce.empty else float(cruce.iloc[0].t_rel_s)

def soc_en(est, t):
    return float(est.iloc[(est.t_rel_s - t).abs().argsort().iloc[0]].soc)

if len(sys.argv) != 3:
    sys.exit(__doc__)
res = {}
for nombre, path in zip("AB", sys.argv[1:]):
    pv, est = cargar(path)
    res[nombre] = dict(amanecer=amanecer(pv), soc302=soc_en(est, 302), pmax=float(pv.p.max()), dur=float(est.t_rel_s.max()))
    print(nombre, res[nombre])
a, b = res["A"], res["B"]
ok = True
if a["amanecer"] is None or b["amanecer"] is None:
    print("FALLA: no se detecto amanecer del PV en alguna corrida"); ok = False
else:
    dif = abs(a["amanecer"] - b["amanecer"]); print("diferencia de amanecer: %.1f s (max 2 s)" % dif); ok &= dif <= 2
dsoc = abs(a["soc302"] - b["soc302"]); print("diferencia de SOC en t=302 s: %.1f pts (max 2)" % dsoc); ok &= dsoc <= 2
print("PV max A/B: %.1f / %.1f W" % (a["pmax"], b["pmax"])); ok &= min(a["pmax"], b["pmax"]) > 20
print("duracion A/B: %.0f / %.0f s (min 1000)" % (a["dur"], b["dur"])); ok &= min(a["dur"], b["dur"]) >= 1000
print("RESULTADO:", "VALIDA, se pueden comparar" if ok else "NO VALIDA, repetir la corrida B (o A)")
