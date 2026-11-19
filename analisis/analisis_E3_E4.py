"""Metricas y figuras de E3 (A/B) y E4 a partir de los CSV del registrador.

Uso (desde la raiz del repositorio):
    python analisis/analisis_E3_E4.py
Genera docs/figuras/fig_E3.png y fig_E4.png e imprime las metricas del informe.
"""
import json
import numpy as np, pandas as pd
import matplotlib; matplotlib.use("Agg")
import matplotlib.pyplot as plt

F = {"A": "datos/E3_A.csv", "B": "datos/E3_B.csv", "E4": "datos/E4.csv"}
GR, BL, OR, GN = "#7a7a7a", "#1f5fbf", "#d9731a", "#2e8b57"
plt.rcParams.update({"font.size": 7.5, "axes.spines.top": False, "axes.spines.right": False,
                     "axes.linewidth": .6, "lines.linewidth": 1.0})

def estado(k):
    d = pd.read_csv(F[k]); d = d[d.topic == "microred_ctrl/estado"]
    r = pd.DataFrame([json.loads(p) for p in d.payload]); r["t"] = d.t_rel_s.values
    return r

def metricas(nombre, r):
    t = r.t.values; dt = np.diff(t, prepend=t[0]); dt[0] = 0; T = t[-1] - t[0]
    s1 = (r.estado == "EXCEDENTE").values
    fuera = ((r.vbus > 50.4) | (r.vbus < 45.6)).values
    ov = (r.vbus > 53).values
    entradas = ((r.estado == "EXCEDENTE") & (r.estado.shift() != "EXCEDENTE")).sum()
    print(f"{nombre}: T={T:.0f} s | Vbus>53: {dt[ov].sum():.1f} s ({100*dt[ov].sum()/T:.1f} %) | "
          f"en banda {100-100*dt[fuera].sum()/T:.1f} % | Vmax {r.vbus.max():.2f} V | "
          f"entradas S1 {entradas} | recorte max {r.recorte_w.max():.0f} W | SOC min {r.soc.min():.1f} % | "
          f"muestras con shed>0: {(r.shed > 0).sum()}")

def sombra(ax, r):
    s = (r.estado == "EXCEDENTE").values; t = r.t.values; i = 0
    while i < len(s):
        if s[i]:
            j = i
            while j + 1 < len(s) and s[j + 1]: j += 1
            ax.axvspan(t[i], t[j], color=OR, alpha=.10, lw=0); i = j + 1
        else: i += 1

rA, rB, r4 = estado("A"), estado("B"), estado("E4")
for n, r in (("E3 A (sin SCADA)", rA), ("E3 B (con SCADA)", rB), ("E4", r4)): metricas(n, r)

fig, ax = plt.subplots(3, 1, figsize=(3.45, 4.0), sharex=True, gridspec_kw={"height_ratios": [1.5, 1, 1]})
for a in ax: a.set_xlim(0, 1085)
ax[0].plot(rA.t, rA.vbus, color=GR, label="A: sin SCADA"); ax[0].plot(rB.t, rB.vbus, color=BL, label="B: con SCADA")
ax[0].axhline(53, color="k", ls=":", lw=.7); ax[0].axhspan(45.6, 50.4, color=GN, alpha=.10, lw=0)
ax[0].set_ylabel("$V_{bus}$ (V)"); ax[0].legend(loc="upper left", fontsize=6.5, frameon=False); ax[0].set_ylim(46, 60); sombra(ax[0], rB)
ax[1].plot(rB.t, rB.recorte_w, color=BL); ax[1].set_ylabel("Recorte B (W)")
ax[2].plot(rA.t, rA.pgen, color=GR, label="gen. A"); ax[2].plot(rB.t, rB.pgen, color=BL, label="gen. B")
ax[2].plot(rB.t, rB.pcarga, color=OR, label="carga B")
ax[2].set_ylabel("Potencia (W)"); ax[2].set_xlabel("t (s)"); ax[2].legend(fontsize=6, frameon=False, ncol=3, loc="upper right")
fig.tight_layout(pad=.4); fig.savefig("docs/figuras/fig_E3.png", dpi=220)

fig, ax = plt.subplots(3, 1, figsize=(3.45, 3.8), sharex=True, gridspec_kw={"height_ratios": [1.3, 1.2, 1]})
for a in ax: a.set_xlim(0, 1020)
ax[0].plot(r4.t, r4.vbus, color=BL); ax[0].axhline(53, color="k", ls=":", lw=.7); ax[0].axhspan(45.6, 50.4, color=GN, alpha=.10, lw=0)
ax[0].set_ylabel("$V_{bus}$ (V)"); ax[0].set_ylim(46, 59); sombra(ax[0], r4)
ax[1].plot(r4.t, r4.pgen, color=BL, label="generación"); ax[1].plot(r4.t, r4.pcarga, color=OR, label="carga")
ax[1].plot(r4.t, r4.recorte_w, color=GR, label="recorte"); ax[1].set_ylabel("Potencia (W)")
ax[1].legend(fontsize=6, frameon=False, ncol=3, loc="upper right")
ax[2].plot(r4.t, r4.soc, color=GN); ax[2].axhline(30, color="k", ls="--", lw=.7)
ax[2].text(5, 32, "umbral S6 (30 %)", fontsize=6); ax[2].set_ylim(0, 105)
ax[2].set_ylabel("SOC (%)"); ax[2].set_xlabel("t (s)")
fig.tight_layout(pad=.4); fig.savefig("docs/figuras/fig_E4.png", dpi=220)
