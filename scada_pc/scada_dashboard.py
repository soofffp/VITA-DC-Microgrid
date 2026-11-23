"""
scada_dashboard.py -- Dashboard SCADA de la Microred DC 48 V (VITA-DC)

Tres ventanas:
  1) Panel principal : estado del SCADA, Vbus, SOC, potencias, graficas y eventos.
  2) Subsistemas     : una tarjeta por subsistema (PV, Eolica, Bateria, Carga).
  3) Eventos y comandos : enviar comandos al sistema, programar una secuencia
                          (p. ej. la del protocolo E3/E4/E5) y editar eventos.

Requisitos:   pip install paho-mqtt matplotlib      (tkinter viene con Python)

Uso en vivo (Mosquitto + ESP32 corriendo):
    python scada_dashboard.py
Reproducir un CSV de logger_raw.py (sin hardware, para probar o para capturas):
    python scada_dashboard.py --replay ..\\datos\\E4.csv --vel 10
Otras opciones:  --broker localhost --port 1883

Solo LEE el broker (microred/# y microred_ctrl/#). Solo publica en
microred_cmd/* cuando activas "Habilitar envio" en la ventana de eventos.
"""
import argparse
import csv
import json
import threading
import time
import tkinter as tk
from collections import deque
from tkinter import filedialog, messagebox, ttk

import matplotlib
matplotlib.use("TkAgg")
from matplotlib.backends.backend_tkagg import FigureCanvasTkAgg
from matplotlib.figure import Figure

# ----------------------------------------------------------------------------
# Paleta
# ----------------------------------------------------------------------------
BG, PANEL, PANEL2, LINE = "#0B1220", "#131C2E", "#1A2540", "#2A3858"
TXT, MUTED = "#E6EDF7", "#8A9AB8"
C_PV, C_WIND, C_BAT, C_LOAD = "#38A3FF", "#FFC233", "#2ED39A", "#FF7AB6"
C_OK, C_WARN, C_BAD, C_INFO, C_OFF = "#2ED39A", "#FFB020", "#FF5470", "#38BDF8", "#7C8BA8"
ESTADO_COL = {"NORMAL": C_OK, "EXCEDENTE": C_WARN, "DEFICIT": C_BAD,
              "SEGURO": C_OFF, "ARRANQUE": C_INFO, "BLACKSTART": C_INFO}
SUBS = [("pv", "Paneles solares", C_PV, "☀"),
        ("eolica", "Turbina de viento", C_WIND, "🌬"),
        ("bateria", "Batería", C_BAT, "🔋"),
        ("carga", "Consumo", C_LOAD, "🏠")]
SUBCOL = {k: c for k, _, c, _ in SUBS}
BAND_LO, BAND_HI, V_ALTO = 45.6, 50.4, 53.0
FONT = "Segoe UI"


def vbus_color(v):
    if v is None:
        return MUTED
    if BAND_LO <= v <= BAND_HI:
        return C_OK
    if BAND_HI < v <= V_ALTO or 43.2 <= v < BAND_LO:
        return C_WARN
    return C_BAD


# ----------------------------------------------------------------------------
# Almacen de datos (lo escribe el hilo MQTT/replay, lo lee la interfaz)
# ----------------------------------------------------------------------------

ESTADO_TXT = {"NORMAL": "EN EQUILIBRIO", "EXCEDENTE": "SOBRA ENERGÍA", "DEFICIT": "FALTA ENERGÍA",
              "SEGURO": "MODO SEGURO", "ARRANQUE": "REINICIO", "BLACKSTART": "REINICIO"}
DESLASTRE = {0: "ninguna", 1: "algunas", 2: "varias", 3: "casi todas"}


def hora_real(h):
    """La hora del SCADA cuenta desde el arranque; las STM arrancan a las 6 h."""
    return None if h is None else (6 + h) % 24


def frase(est):
    e = (est.get("estado") or "").upper()
    sh = est.get("shed", 0) or 0
    if not e:
        return "Esperando datos del sistema…"
    if e == "NORMAL" and sh:
        return "El voltaje ya está estable, pero el cerebro mantiene apagada una parte del consumo mientras la batería se recupera."
    if e == "NORMAL":
        return "Todo en equilibrio: la batería mantiene estable el voltaje de la red."
    if e == "EXCEDENTE":
        return "Sobra energía y la batería ya no puede guardar más: el cerebro reduce la producción para que el voltaje no se dispare."
    if e == "DEFICIT":
        if sh:
            return "Falta energía y la batería está baja: el cerebro apaga una parte del consumo para protegerla."
        return "Falta energía: la batería entrega lo que falta para cubrir el consumo."
    if e == "SEGURO":
        return "Modo seguro: el sistema espera instrucciones antes de seguir."
    return "Reiniciando la red tras un apagón, paso a paso (primero la batería, luego los generadores y al final el consumo)."


EVENTOS_AMIGABLES = (("S1", "Sobra energía: el cerebro empieza a reducir la producción"),
                     ("S2", "Sigue sobrando: el cerebro reduce un poco más la producción"),
                     ("S3", "Ya sobra menos: el cerebro devuelve algo de producción"),
                     ("S4", "Se acabó el exceso: la producción vuelve a ser total"),
                     ("S5", "Falta energía: la batería empieza a ayudar"),
                     ("S6", "Batería baja: el cerebro apaga una parte del consumo"),
                     ("S7", "Batería recuperada: el consumo vuelve a encenderse"),
                     ("S8", "Modo seguro activado"),
                     ("S9", "Reinicio de la red tras un apagón"))


def evento_amigable(txt):
    t = txt.strip()
    for cod, f in EVENTOS_AMIGABLES:
        if t.startswith(cod):
            return f, (cod if cod in ("S1", "S4", "S5", "S6", "S7") else "evento")
    if t.lower().startswith("blackstart"):
        return "Reinicio de la red: " + t.split(":", 1)[-1].strip(), "evento"
    return t, "evento"


REGLAS_AMIGABLES = {
    "R1": "Aprovecha al máximo el sol", "R2": "Producción limitada a propósito por el cerebro",
    "R3": "Producción limitada a propósito por el cerebro", "R4": "Modo seguro: solo cuida el voltaje",
    "E1": "Aprovecha al máximo el viento", "E2": "Producción limitada a propósito por el cerebro",
    "E3": "Producción limitada a propósito por el cerebro", "E4": "Modo seguro: mantiene lo último que funcionó",
    "E5": "Hay poco viento: no genera por ahora",
    "B1": "La batería mantiene estable el voltaje de la red", "B2": "Carga limitada (sobra energía)",
    "B3": "Descarga limitada (falta energía)", "B4": "Modo seguro: corrientes conservadoras",
    "C1": "El consumo sigue su ritmo normal", "C2": "Consumo reducido por el cerebro",
    "C3": "Consumo mínimo (modo seguro)"}
DESCRIPCION = {"pv": "Imita el sol: más luz, más energía.", "eolica": "Imita una turbina: más viento, más energía.",
               "bateria": "Guarda lo que sobra y entrega lo que falta.", "carga": "Imita lo que gastan una casa o una fábrica."}


class Store:
    def __init__(self):
        self.lock = threading.Lock()
        self.t0 = time.time()
        self.vclock = None            # reloj virtual en modo replay
        self.est = {}
        self.est_t = None
        self.hist = deque(maxlen=4000)  # (t, vbus, soc, pgen, pcarga, pbat, estado, shed)
        self.sub = {k: {"main": {}, "conv": {}, "extra": {}, "ctrl": {}, "regla": {}, "t": None,
                        "hist": deque(maxlen=900)} for k, *_ in SUBS}
        self.eventos = deque(maxlen=500)  # (t, texto, tipo)
        self.conex = deque(maxlen=300)    # (t, clave, conectado) medido por el gateway
        self.nuevos = deque()
        self.conectado = False
        self.msgs = 0

    def now(self):
        return self.vclock if self.vclock is not None else time.time() - self.t0

    def ingest(self, topic, d, t=None):
        t = self.now() if t is None else t
        with self.lock:
            self.msgs += 1
            if topic == "microred_ctrl/estado":
                self.est, self.est_t = d, t
                self.hist.append((t, d.get("vbus"), d.get("soc"), d.get("pgen"), d.get("pcarga"),
                                  d.get("pbat"), d.get("estado"), d.get("shed", 0)))
            elif topic == "microred_ctrl/sistema":
                if "escenario" in d:
                    txt, tipo = ("El consumo cambia a " + ("una fábrica 🏭" if d["escenario"] == "industrial" else "una casa 🏠")), "esc"
                else:
                    txt, tipo = evento_amigable(d.get("evento") or json.dumps(d))
                self._add_event(t, txt, tipo)
            elif topic.startswith("microred_ctrl/") and topic.endswith("_online"):
                k = topic.split("/")[1][:-7]
                if k in self.sub and "online" in d:
                    on = bool(d["online"])
                    nombre = {kk: nn for kk, nn, *_ in SUBS}.get(k, k)
                    self.conex.append((t, k, on))
                    self._add_event(t, f"{nombre}: {'se conectó' if on else 'se desconectó'} (lo detectó el cerebro)",
                                    "S7" if on else "S5")
            elif topic.startswith("microred_ctrl/"):
                n = topic.split("/")[1]
                if n.endswith("_reglas") and n[:-7] in self.sub:
                    self.sub[n[:-7]]["regla"] = d
                elif n in self.sub:
                    self.sub[n]["ctrl"] = d
            elif topic.startswith("microred/"):
                n = topic.split("/")[1]
                if n in self.sub:
                    s = self.sub[n]
                    s["main"], s["t"] = d, t
                    s["hist"].append((t, d.get("v", 0), d.get("i", 0), d.get("p", 0)))
                elif n.endswith("_conv") and n[:-5] in self.sub:
                    self.sub[n[:-5]]["conv"] = d
                elif n.endswith("_extra") and n[:-6] in self.sub:
                    self.sub[n[:-6]]["extra"] = d

    def _add_event(self, t, txt, tipo):
        self.eventos.append((t, txt, tipo))
        self.nuevos.append((t, txt, tipo))

    def log(self, txt, tipo="cmd"):
        with self.lock:
            self._add_event(self.now(), txt, tipo)


store = Store()


# ----------------------------------------------------------------------------
# Conexion MQTT y replay
# ----------------------------------------------------------------------------
class Bus:
    def __init__(self, broker, port):
        self.client = None
        self.replay = False
        self.broker, self.port = broker, port

    def start_live(self):
        import paho.mqtt.client as mqtt
        try:  # paho-mqtt 2.x
            c = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id="scada_dashboard_v2")
        except AttributeError:  # paho-mqtt 1.x
            c = mqtt.Client(client_id="scada_dashboard_v2")

        def on_connect(cl, u, f, rc, p=None):
            print("[dashboard] conectado al broker, codigo:", rc)
            store.conectado = True
            cl.subscribe("microred/#")
            cl.subscribe("microred_ctrl/#")

        def on_disconnect(cl, u, f, rc, p=None):
            store.conectado = False

        def on_message(cl, u, msg):
            try:
                d = json.loads(msg.payload.decode())
            except (ValueError, UnicodeDecodeError):
                return
            if isinstance(d, dict):
                store.ingest(msg.topic, d)
            elif msg.topic.endswith("_online") and d in (0, 1):
                store.ingest(msg.topic, {"online": bool(d)})

        c.on_connect, c.on_disconnect, c.on_message = on_connect, on_disconnect, on_message
        self.client = c
        try:
            c.connect(self.broker, self.port, keepalive=30)
            c.loop_start()
        except OSError as e:
            print(f"[dashboard] No pude conectar a {self.broker}:{self.port}: {e}")
        else:
            print(f"[dashboard] conectando a {self.broker}:{self.port} ... (si arriba dice En vivo, ya esta)")

    def start_replay(self, path, vel):
        self.replay = True
        store.conectado = True
        store.vclock = 0.0

        def run():
            with open(path, newline="", encoding="utf-8") as f:
                rd = csv.DictReader(f)
                for row in rd:
                    try:
                        t = float(row["t_rel_s"])
                        d = json.loads(row["payload"])
                    except (ValueError, KeyError, json.JSONDecodeError):
                        continue
                    while store.vclock is not None and t - store.vclock > 0.02:
                        time.sleep(min(0.05, (t - store.vclock) / vel))
                        store.vclock += min(0.05, (t - store.vclock) / vel) * vel
                    store.vclock = t
                    if isinstance(d, dict):
                        store.ingest(row["topic"], d, t)
                    elif row["topic"].endswith("_online") and d in (0, 1):
                        store.ingest(row["topic"], {"online": bool(d)}, t)
            store.log("Fin del archivo (replay)", "evento")

        threading.Thread(target=run, daemon=True).start()

    def publish(self, topic, payload):
        if self.replay:
            store.log(f"[replay, no se envía] {topic} {payload}")
            return True
        if not self.client or not store.conectado:
            store.log(f"SIN CONEXIÓN, no se envió: {topic} {payload}", "err")
            return False
        self.client.publish(topic, payload)
        store.log(f"Enviado → {topic}  {payload}")
        try:  # bitacora de comandos con hora real, para cruzar con el CSV del logger
            import os, datetime
            os.makedirs("runs", exist_ok=True)
            with open(os.path.join("runs", "comandos_dashboard.csv"), "a", encoding="utf-8") as f:
                f.write(f"{datetime.datetime.now().isoformat(timespec='milliseconds')},{topic},{payload}\n")
        except OSError:
            pass
        return True


bus = Bus("localhost", 1883)

# ----------------------------------------------------------------------------
# Comandos (misma sintaxis que el Monitor Serie del ESP32)
# ----------------------------------------------------------------------------
SUBTOP = {"pv": "pv", "wind": "eolica", "eolica": "eolica", "bat": "bateria", "bateria": "bateria",
          "load": "carga", "carga": "carga"}
MODE = {"mppt": 0, "normal": 0, "limited": 1, "reduced": 1, "safe": 2}


def parse_cmd(text):
    """'load reduced 40' -> (topic, payload). Lanza ValueError si no se entiende."""
    w = text.strip().split()
    if not w:
        raise ValueError("comando vacío")
    a = w[0].lower()
    if a in ("blackstart", "auto", "hora") or (a == "load" and len(w) > 1 and w[1].lower() in ("res", "ind")):
        if a == "blackstart":
            return "microred_cmd/sistema", "blackstart"
        if a == "auto":
            return "microred_cmd/sistema", "auto_on" if w[1].lower() == "on" else "auto_off"
        if a == "hora":
            return "microred_cmd/sistema", json.dumps({"hora": float(w[1])})
        return "microred_cmd/sistema", "residencial" if w[1].lower() == "res" else "industrial"
    if a in SUBTOP and len(w) > 1 and w[1].lower() in MODE:
        s, m = SUBTOP[a], MODE[w[1].lower()]
        nums = [float(x) for x in w[2:]]
        p = {"mode": m}
        if s in ("pv", "eolica") and nums:
            p["pmax"] = nums[0]
        elif s == "bateria" and nums:
            p["ich"] = nums[0]
            p["idis"] = nums[1] if len(nums) > 1 else nums[0]
        elif s == "carga" and nums:
            p["nivel"] = nums[0]
            if len(nums) > 1:
                p["esc"] = int(nums[1])
        return f"microred_cmd/{s}", json.dumps(p)
    raise ValueError(f"no entiendo «{text}»")


PRESETS = {
    "E3 – corrida A (sin SCADA)": [(0, "load reduced 40"), (290, "load res"), (302, "auto off"),
                                   (303, "pv mppt"), (304, "wind mppt")],
    "E3 – corrida B (con SCADA)": [(0, "load reduced 40"), (290, "load res")],
    "E4 – déficit industrial": [(28, "load ind")],
    "E2 – excedente y recorte": [(0, "load reduced 40")],
    "E5 – arranque en negro": [(0, "load res"), (90, "blackstart"), (320, "blackstart")],
    "P0 / E1 – sin comandos": [],
}

# ----------------------------------------------------------------------------
# Utilidades de interfaz
# ----------------------------------------------------------------------------
def style_ttk(root):
    st = ttk.Style(root)
    st.theme_use("clam")
    st.configure(".", background=PANEL, foreground=TXT, fieldbackground=PANEL2, font=(FONT, 10))
    st.configure("TCombobox", fieldbackground=PANEL2, background=PANEL2, foreground=TXT, arrowcolor=TXT)
    st.map("TCombobox", fieldbackground=[("readonly", PANEL2)], foreground=[("readonly", TXT)])
    st.configure("Treeview", background=PANEL2, fieldbackground=PANEL2, foreground=TXT, rowheight=26,
                 borderwidth=0, font=(FONT, 10))
    st.configure("Treeview.Heading", background=LINE, foreground=TXT, font=(FONT, 10, "bold"), relief="flat")
    st.map("Treeview", background=[("selected", "#2F4B8A")])
    st.configure("TNotebook", background=BG, borderwidth=0, tabmargins=(12, 8, 12, 0))
    st.configure("TNotebook.Tab", background=PANEL2, foreground=MUTED, padding=(18, 8), font=(FONT, 11, "bold"), borderwidth=0)
    st.map("TNotebook.Tab", background=[("selected", C_INFO)], foreground=[("selected", "#06101F")])
    st.configure("TCheckbutton", background=PANEL, foreground=TXT)


def card(parent, **kw):
    f = tk.Frame(parent, bg=PANEL, highlightbackground=LINE, highlightthickness=1, **kw)
    return f


def lbl(parent, text="", size=10, bold=False, color=TXT, bg=None, **kw):
    return tk.Label(parent, text=text, bg=bg or parent["bg"], fg=color,
                    font=(FONT, size, "bold" if bold else "normal"), **kw)


def button(parent, text, cmd, color=C_PV, fg="#06101F", **kw):
    b = tk.Button(parent, text=text, command=cmd, bg=color, fg=fg, activebackground=color,
                  activeforeground=fg, relief="flat", bd=0, padx=12, pady=6, cursor="hand2",
                  font=(FONT, 10, "bold"), **kw)
    return b


def chip(parent, text, color):
    return tk.Label(parent, text=f"  {text}  ", bg=color, fg="#06101F", font=(FONT, 11, "bold"), pady=2)


def fmt(v, nd=1, suf=""):
    if v is None:
        return "—"
    try:
        return f"{float(v):.{nd}f}{suf}"
    except (TypeError, ValueError):
        return "—"


class Bar(tk.Canvas):
    """Barra horizontal de progreso con marcas."""
    def __init__(self, parent, w=240, h=18, marks=(), **kw):
        super().__init__(parent, width=w, height=h, bg=parent["bg"], highlightthickness=0, **kw)
        self.w, self.h, self.marks = w, h, marks

    def set(self, frac, color):
        self.delete("all")
        frac = 0 if frac is None else max(0, min(1, frac))
        self.create_rectangle(0, 0, self.w, self.h, fill=PANEL2, outline=LINE)
        self.create_rectangle(0, 0, self.w * frac, self.h, fill=color, outline="")
        for m in self.marks:
            x = self.w * m
            self.create_line(x, 0, x, self.h, fill=TXT, dash=(2, 2))


class Spark(tk.Canvas):
    def __init__(self, parent, w=300, h=60, color=C_PV, **kw):
        super().__init__(parent, width=w, height=h, bg=PANEL2, highlightthickness=0, **kw)
        self.w, self.h, self.color = w, h, color

    def set(self, pts, tmax, win=60):
        self.delete("all")
        pts = [(t, v) for t, v in pts if t >= tmax - win]
        if len(pts) < 2:
            return
        vs = [v for _, v in pts]
        lo, hi = min(0, min(vs)), max(vs) if max(vs) > 0 else 1
        if hi - lo < 1e-6:
            hi = lo + 1
        xy = []
        for t, v in pts:
            xy += [(t - (tmax - win)) / win * self.w, self.h - 4 - (v - lo) / (hi - lo) * (self.h - 10)]
        self.create_line(*xy, fill=self.color, width=2)
        if lo < 0:
            y0 = self.h - 4 - (0 - lo) / (hi - lo) * (self.h - 10)
            self.create_line(0, y0, self.w, y0, fill=LINE, dash=(3, 3))



# ----------------------------------------------------------------------------
# Pestaña 1: mapa de energía (para el publico general)
# ----------------------------------------------------------------------------
class Mapa:
    def __init__(self, page):
        self.page = page
        self.fase = 0
        self.banner = tk.Label(page, text="", bg=PANEL, fg=TXT, font=(FONT, 17, "bold"), wraplength=1250,
                               justify="left", anchor="w", padx=20, pady=14, highlightbackground=LINE, highlightthickness=1)
        self.banner.pack(fill="x", padx=16, pady=(14, 6))
        self.cv = tk.Canvas(page, bg=BG, highlightthickness=0)
        self.cv.pack(fill="both", expand=True, padx=16, pady=4)
        self.leyenda = tk.Label(page, bg=BG, fg=MUTED, font=(FONT, 11), anchor="w",
                                text="Cuanto más gruesa y rápida es la flecha, más energía fluye. Todo el sistema es una réplica en miniatura: "
                                     "las fuentes, la batería y el consumo están simulados por microcontroladores.")
        self.leyenda.pack(fill="x", padx=18, pady=(0, 12))
        page.after(150, self.tick)

    def tick(self):
        try:
            self.draw()
        except Exception as e:
            print("[mapa] error:", e)
        self.page.after(150, self.tick)

    def _nodo(self, x, y, w, h, col, titulo, valor, sub="", ico=""):
        c = self.cv
        c.create_rectangle(x - w / 2, y - h / 2, x + w / 2, y + h / 2, fill=PANEL, outline=col, width=3)
        c.create_rectangle(x - w / 2, y - h / 2, x + w / 2, y - h / 2 + 8, fill=col, outline=col)
        c.create_text(x, y - h / 2 + 34, text=f"{ico} {titulo}", fill=col, font=(FONT, 15, "bold"))
        c.create_text(x, y + 6, text=valor, fill=TXT, font=(FONT, 26, "bold"))
        if sub:
            c.create_text(x, y + h / 2 - 22, text=sub, fill=MUTED, font=(FONT, 11), width=w - 16, justify="center")

    def _flecha(self, a, b, p, col, fase):
        """Flecha de a hacia b; grosor segun potencia; animada."""
        mag = abs(p or 0)
        if mag < 1:
            self.cv.create_line(*a, *b, fill=LINE, width=2, dash=(4, 6))
            return
        wd = 3 + min(mag, 160) / 160 * 16
        self.cv.create_line(*a, *b, fill=col, width=wd, dash=(14, 10), dashoffset=-(fase * 3) % 24, arrow="last",
                            arrowshape=(wd * 1.6 + 8, wd * 1.8 + 10, wd * 0.8 + 4))

    def draw(self):
        self.fase += 1
        c = self.cv
        c.delete("all")
        W, H = max(c.winfo_width(), 800), max(c.winfo_height(), 400)
        with store.lock:
            est = dict(store.est)
            last = {k: dict(v["main"]) for k, v in store.sub.items()}
            extra = {k: dict(v["extra"]) for k, v in store.sub.items()}
        self.banner.config(text=frase(est), fg=ESTADO_COL.get(est.get("estado"), TXT) if est else TXT)
        p = {k: (last[k].get("p") or 0) for k in last}
        pbat = est.get("pbat", p["bateria"]) or 0
        vbus = est.get("vbus")
        hr = hora_real(est.get("hora"))
        rec = (est.get("recorte_w") or 0) > 0
        sh = int(est.get("shed", 0) or 0)
        # posiciones
        bx, by = W * 0.50, H * 0.50
        sol = (W * 0.14, H * 0.20); vie = (W * 0.14, H * 0.50); bat = (W * 0.14, H * 0.80)
        con = (W * 0.86, H * 0.50); cer = (W * 0.50, H * 0.13)
        nw, nh = min(280, W * 0.22), min(150, H * 0.27)
        # cerebro: lineas finas hacia todos
        for q in (sol, vie, bat, con):
            c.create_line(cer[0], cer[1] + 30, q[0], q[1], fill=LINE, dash=(2, 6))
        # flechas de energia
        self._flecha((sol[0] + nw / 2, sol[1]), (bx - 70, by - 22), p["pv"], C_PV, self.fase)
        self._flecha((vie[0] + nw / 2, vie[1]), (bx - 70, by), p["eolica"], C_WIND, self.fase)
        if pbat >= 0:
            self._flecha((bat[0] + nw / 2, bat[1]), (bx - 70, by + 22), pbat, C_BAT, self.fase)
        else:
            self._flecha((bx - 70, by + 22), (bat[0] + nw / 2, bat[1]), pbat, C_BAT, self.fase)
        self._flecha((bx + 70, by), (con[0] - nw / 2, con[1]), p["carga"], C_LOAD, self.fase)
        # centro: la red
        col = vbus_color(vbus)
        c.create_oval(bx - 70, by - 70, bx + 70, by + 70, fill=PANEL, outline=col, width=4)
        c.create_text(bx, by - 30, text="LA RED", fill=MUTED, font=(FONT, 11, "bold"))
        c.create_text(bx, by + 2, text=fmt(vbus, 1, " V"), fill=col, font=(FONT, 24, "bold"))
        c.create_text(bx, by + 36, text="estable" if vbus is not None and BAND_LO <= vbus <= BAND_HI else ("alto" if vbus and vbus > BAND_HI else "—"),
                      fill=col, font=(FONT, 11))
        # cerebro
        e = est.get("estado")
        c.create_rectangle(cer[0] - 150, cer[1] - 30, cer[0] + 150, cer[1] + 30, fill=PANEL2, outline=ESTADO_COL.get(e, LINE), width=3)
        c.create_text(cer[0], cer[1] - 8, text="🧠 EL CEREBRO (SCADA)", fill=TXT, font=(FONT, 13, "bold"))
        c.create_text(cer[0], cer[1] + 14, text=ESTADO_TXT.get(e, "—"), fill=ESTADO_COL.get(e, MUTED), font=(FONT, 12, "bold"))
        # nodos
        self._nodo(*sol, nw, nh, C_PV, "Sol", fmt(p["pv"], 0, " W"),
                   "producción recortada por el cerebro" if rec and p["pv"] > 1 else "paneles solares", "☀")
        self._nodo(*vie, nw, nh, C_WIND, "Viento", fmt(p["eolica"], 0, " W"),
                   "producción recortada por el cerebro" if rec and p["eolica"] > 1 else "turbina de viento", "🌬")
        so = est.get("soc")
        self._nodo(*bat, nw, nh, C_BAT, "Batería", fmt(so, 0, " %"),
                   ("entrega " if pbat > 1 else "guarda " if pbat < -1 else "en reposo") + (fmt(abs(pbat), 0, " W") if abs(pbat) > 1 else ""), "🔋")
        esc = "fábrica 🏭" if est.get("esc") == 1 else "casas 🏠"
        self._nodo(*con, nw, nh, C_LOAD, "Consumo", fmt(p["carga"], 0, " W"),
                   esc + ("  ·  cargas apagadas" if sh else ""), "🏘")
        # reloj dia/noche
        if hr is not None:
            c.create_text(W - 20, H - 14, anchor="se", fill=C_WIND if 6 <= hr < 18 else C_INFO, font=(FONT, 14, "bold"),
                          text=("☀ día  " if 6 <= hr < 18 else "🌙 noche  ") + f"{int(hr):02d}:{int((hr % 1) * 60):02d}  (1 hora = 15 s)")


# ----------------------------------------------------------------------------
# Ventana 1: panel principal
# ----------------------------------------------------------------------------
class Principal:
    def __init__(self, root):
        self.root = root
        root.title("VITA-DC · Una red eléctrica en miniatura — demostración en vivo")
        root.configure(bg=BG)
        root.geometry("1400x900")
        self.win = tk.IntVar(value=120)
        self._build()
        self.subw = Subsistemas(self.page_sub, self.nb)
        self.evw = Eventos(self.page_ev, self.nb)
        root.after(300, self.tick)

    def _build(self):
        self.nb = ttk.Notebook(self.root)
        self.nb.pack(fill="both", expand=True)
        self.page_map = tk.Frame(self.nb, bg=BG)
        self.page_main = tk.Frame(self.nb, bg=BG)
        self.page_sub = tk.Frame(self.nb, bg=BG)
        self.page_ev = tk.Frame(self.nb, bg=BG)
        self.nb.add(self.page_map, text="  Mapa de energía  ")
        self.nb.add(self.page_main, text="  Gráficas y cifras  ")
        self.nb.add(self.page_sub, text="  Las 4 partes  ")
        self.nb.add(self.page_ev, text="  Control  ")
        self.mapa = Mapa(self.page_map)
        r = self.page_main
        top = tk.Frame(r, bg=BG)
        top.pack(fill="x", padx=16, pady=(12, 6))
        lbl(top, "VITA-DC", 20, True, C_INFO, BG).pack(side="left")
        lbl(top, "  Una red eléctrica en miniatura, en vivo", 13, False, MUTED, BG).pack(side="left")
        self.conn = tk.Label(top, text="", bg=BG, font=(FONT, 10, "bold"))
        self.conn.pack(side="right", padx=8)
        self.vwin = tk.StringVar(value="120 s")
        ttk.Combobox(top, width=8, state="readonly", values=["60 s", "120 s", "300 s", "600 s"],
                     textvariable=self.vwin).pack(side="right", padx=8)
        self.vwin.trace_add("write", lambda *a: self.win.set(int(self.vwin.get().split()[0])))
        lbl(top, "Tiempo visible:", 10, False, MUTED, BG).pack(side="right")

        # fila de tarjetas
        row = tk.Frame(r, bg=BG)
        row.pack(fill="x", padx=16, pady=6)
        for i in range(5):
            row.columnconfigure(i, weight=1, uniform="k")
        # estado
        c = card(row); c.grid(row=0, column=0, sticky="nsew", padx=(0, 8))
        lbl(c, "¿QUÉ ESTÁ HACIENDO EL CEREBRO?", 9, True, MUTED).pack(anchor="w", padx=14, pady=(10, 2))
        self.estado = chip(c, "—", C_OFF); self.estado.pack(anchor="w", padx=14)
        self.regla = lbl(c, "", 10, False, MUTED, wraplength=230, justify="left"); self.regla.pack(anchor="w", padx=14, pady=(6, 10))
        # Vbus
        c = card(row); c.grid(row=0, column=1, sticky="nsew", padx=8)
        lbl(c, "VOLTAJE DE LA RED (ideal: 48 V)", 9, True, MUTED).pack(anchor="w", padx=14, pady=(10, 0))
        self.vbus = lbl(c, "—", 30, True); self.vbus.pack(anchor="w", padx=14)
        self.vbus_sub = lbl(c, "", 9, False, MUTED); self.vbus_sub.pack(anchor="w", padx=14, pady=(0, 10))
        # SOC
        c = card(row); c.grid(row=0, column=2, sticky="nsew", padx=8)
        lbl(c, "BATERÍA: CARGA DISPONIBLE", 9, True, MUTED).pack(anchor="w", padx=14, pady=(10, 0))
        self.soc = lbl(c, "—", 30, True); self.soc.pack(anchor="w", padx=14)
        self.socbar = Bar(c, 200, 12, marks=(0.2, 0.3)); self.socbar.pack(anchor="w", padx=14, pady=(0, 10))
        # Hora / escenario
        c = card(row); c.grid(row=0, column=3, sticky="nsew", padx=8)
        lbl(c, "HORA DEL DÍA SIMULADO", 9, True, MUTED).pack(anchor="w", padx=14, pady=(10, 0))
        self.hora = lbl(c, "—", 30, True); self.hora.pack(anchor="w", padx=14)
        self.dia = Bar(c, 200, 12, marks=(0.25, 0.75)); self.dia.pack(anchor="w", padx=14, pady=(0, 10))
        # Modo
        c = card(row); c.grid(row=0, column=4, sticky="nsew", padx=(8, 0))
        lbl(c, "SITUACIÓN", 9, True, MUTED).pack(anchor="w", padx=14, pady=(10, 2))
        self.esc = lbl(c, "Escenario: —", 11, True); self.esc.pack(anchor="w", padx=14)
        self.shed = lbl(c, "Deslastre: —", 11); self.shed.pack(anchor="w", padx=14)
        self.auto = lbl(c, "Automático: —", 11); self.auto.pack(anchor="w", padx=14, pady=(0, 10))

        # fila de potencias
        row2 = tk.Frame(r, bg=BG)
        row2.pack(fill="x", padx=16, pady=6)
        self.pw = {}
        for i, (key, name, col) in enumerate([("pgen", "ENERGÍA QUE SE PRODUCE", C_PV), ("pcarga", "ENERGÍA QUE SE CONSUME", C_LOAD),
                                              ("pbat", "BATERÍA", C_BAT), ("recorte", "PRODUCCIÓN RECORTADA", C_WARN)]):
            row2.columnconfigure(i, weight=1, uniform="p")
            c = card(row2); c.grid(row=0, column=i, sticky="nsew", padx=(0 if i == 0 else 8, 0 if i == 3 else 0))
            tk.Frame(c, bg=col, height=4).pack(fill="x")
            lbl(c, name, 9, True, MUTED).pack(anchor="w", padx=14, pady=(8, 0))
            v = lbl(c, "—", 22, True, col); v.pack(side="left", padx=14, pady=(0, 10))
            s = lbl(c, "", 10, False, MUTED); s.pack(side="left", pady=(8, 10))
            self.pw[key] = (v, s)

        # eventos
        bot = card(r)
        bot.pack(side="bottom", fill="x", padx=16, pady=(6, 14))
        lbl(bot, "LO QUE VA PASANDO", 9, True, MUTED).pack(anchor="w", padx=14, pady=(8, 0))
        self.feed = tk.Text(bot, height=6, bg=PANEL, fg=TXT, relief="flat", font=(FONT, 11), wrap="none",
                            highlightthickness=0, state="disabled")
        self.feed.pack(fill="x", padx=10, pady=(2, 8))
        for tag, col in (("evento", TXT), ("esc", C_INFO), ("cmd", C_WARN), ("err", C_BAD), ("S1", C_WARN),
                         ("S6", C_BAD), ("S7", C_OK), ("S4", C_OK), ("S5", C_BAD)):
            self.feed.tag_config(tag, foreground=col)

        # graficas
        mid = tk.Frame(r, bg=BG)
        mid.pack(fill="both", expand=True, padx=16, pady=6)
        self.fig = Figure(figsize=(10, 5.2), dpi=100, facecolor=BG)
        gs = self.fig.add_gridspec(3, 2, width_ratios=[1.35, 1], hspace=0.62, wspace=0.18,
                                   left=0.06, right=0.985, top=0.95, bottom=0.09)
        self.a_v = self.fig.add_subplot(gs[0, 0]); self.a_z = self.fig.add_subplot(gs[1, 0], sharex=self.a_v)
        self.a_s = self.fig.add_subplot(gs[2, 0], sharex=self.a_v)
        self.a_p = self.fig.add_subplot(gs[:, 1])
        for a in (self.a_v, self.a_z, self.a_s, self.a_p):
            a.set_facecolor(PANEL)
            a.tick_params(colors=MUTED, labelsize=9)
            for s in a.spines.values():
                s.set_color(LINE)
            a.grid(True, color=LINE, alpha=0.6, lw=0.6)
        self.a_v.set_title("Voltaje de la red [V]  (franja verde = rango normal)", color=TXT, fontsize=11, loc="left")
        self.a_z.set_title("Voltaje de la red con zoom [V]  (líneas: una parte se conecta o se desconecta)", color=TXT, fontsize=11, loc="left")
        self.a_s.set_title("Carga de la batería [%]  (rojo: 20 %, ámbar: 30 %)", color=TXT, fontsize=11, loc="left")
        self.a_p.set_title("Energía de cada parte [W]  (negativo = se guarda en la batería)", color=TXT, fontsize=11, loc="left")
        self.a_v.axhspan(BAND_LO, BAND_HI, color=C_OK, alpha=0.10, lw=0)
        self.a_v.axhline(V_ALTO, color=C_BAD, ls="--", lw=1)
        self.a_v.text(0.995, V_ALTO, " 53 V", transform=self.a_v.get_yaxis_transform(), color=C_BAD, fontsize=8, va="bottom", ha="right")
        self.a_s.axhline(20, color=C_BAD, ls="--", lw=1); self.a_s.axhline(30, color=C_WARN, ls="--", lw=1)
        self.l_v, = self.a_v.plot([], [], color=C_INFO, lw=2)
        self.l_s, = self.a_s.plot([], [], color=C_BAT, lw=2)
        self.l_z, = self.a_z.plot([], [], color=C_INFO, lw=2, marker=".", ms=3)
        self.a_z.set_ylim(47.9, 48.1)
        self.a_z.axhline(48.0, color=MUTED, lw=0.8, ls=":")
        self.zmarks = []
        self.ztxt = self.a_z.text(0.01, 0.06, "", transform=self.a_z.transAxes, color=TXT, fontsize=8.5, va="bottom")
        self.l_p = {k: self.a_p.plot([], [], color=c, lw=1.8, label=n)[0]
                    for k, n, c, _ in [(k, n.split(" (")[0], c, i) for k, n, c, i in SUBS]}
        self.a_p.legend(facecolor=PANEL2, edgecolor=LINE, labelcolor=TXT, fontsize=9, loc="upper left")
        self.a_s.set_xlabel("tiempo transcurrido [s]", color=MUTED, fontsize=9)
        self.a_v.set_ylim(44, 58); self.a_s.set_ylim(0, 100)
        self.spans = []
        self.canvas = FigureCanvasTkAgg(self.fig, master=mid)
        self.canvas.get_tk_widget().configure(bg=BG, highlightthickness=0)
        self.canvas.get_tk_widget().pack(fill="both", expand=True)

    def _zoom_eventos(self, hist, now, w):
        """Marca en la gráfica ampliada cada conexión/desconexión detectada y reporta,
        con el dato medido, cuánto cambió el voltaje después. No inventa nada: si el
        sistema reportó 48.00 V todo el tiempo, lo dice así."""
        for m in self.zmarks:
            try:
                m.remove()
            except Exception:
                pass
        self.zmarks = []
        with store.lock:
            ev = [e for e in store.conex if e[0] >= now - w - 5]
        nombres = {k: n for k, n, *_ in SUBS}
        yl = self.a_z.get_ylim()
        for t, k, on in ev:
            col = SUBCOL.get(k, MUTED)
            self.zmarks.append(self.a_z.axvline(t, color=col, lw=1.6, ls="-" if on else "--"))
            self.zmarks.append(self.a_z.text(t, yl[1], (" ▲ " if on else " ✕ ") + nombres.get(k, k).split()[0],
                                             color=col, fontsize=8, va="top", ha="left"))
        if ev:
            t, k, on = ev[-1]
            antes = [x[1] for x in hist if x[0] < t and x[1] is not None]
            despues = [x[1] for x in hist if t <= x[0] <= t + 10 and x[1] is not None]
            if antes and despues:
                d = max(abs(v - antes[-1]) for v in despues)
                msg = (f"{nombres.get(k, k)} {'se conectó' if on else 'se desconectó'}: cambio medido del voltaje en 10 s = {d:.2f} V"
                       + ("\nSin cambio medible: la batería sostiene el voltaje (se reporta con 2 decimales)." if d < 0.005 else ""))
            else:
                msg = f"{nombres.get(k, k)} {'se conectó' if on else 'se desconectó'}…"
            self.ztxt.set_text(msg)
        else:
            self.ztxt.set_text("Sin maniobras en esta ventana. Las líneas aparecen cuando una parte se conecta o desconecta.")

    def tick(self):
        try:
            self.update()
        except Exception as e:  # nunca matar la ventana por un dato raro
            print("[dashboard] error de actualización:", e)
        self.root.after(500, self.tick)

    def update(self):
        s = store
        with s.lock:
            est = dict(s.est)
            hist = list(s.hist)
            subs = {k: list(v["hist"]) for k, v in s.sub.items()}
            nuevos = list(s.nuevos); s.nuevos.clear()
        now = s.now()
        if bus.replay:
            self.conn.config(text="● DEMOSTRACIÓN (datos grabados)", fg=C_INFO)
        else:
            self.conn.config(text="● En vivo" if s.conectado else "● Sin conexión con el sistema",
                             fg=C_OK if s.conectado else C_BAD)
        e = est.get("estado")
        if e:
            self.estado.config(text=f"  {ESTADO_TXT.get(e, e)}  ", bg=ESTADO_COL.get(e, C_OFF))
            self.regla.config(text=frase(est))
            v = est.get("vbus")
            self.vbus.config(text=fmt(v, 2, " V"), fg=vbus_color(v))
            self.vbus_sub.config(text=("estable" if v is not None and BAND_LO <= v <= BAND_HI else ("demasiado alto" if v is not None and v > BAND_HI else "demasiado bajo")),
                                 fg=vbus_color(v))
            so = est.get("soc")
            self.soc.config(text=fmt(so, 1, " %"), fg=C_BAD if so is not None and so < 20 else (C_WARN if so is not None and so < 30 else C_BAT))
            self.socbar.set(None if so is None else so / 100, C_BAD if so is not None and so < 20 else (C_WARN if so is not None and so < 30 else C_BAT))
            h = est.get("hora")
            hr = hora_real(h)
            self.hora.config(text=(("☀ " if 6 <= hr < 18 else "🌙 ") + f"{int(hr):02d}:{int((hr % 1) * 60):02d}") if hr is not None else "—")
            self.dia.set(None if hr is None else hr / 24, C_WIND)
            self.esc.config(text="Consumo: " + ("fábrica 🏭" if est.get("esc") == 1 else "casas 🏠"))
            sh = est.get("shed", 0)
            self.shed.config(text="Cargas apagadas: " + DESLASTRE[min(int(sh), 3)],
                             fg=C_BAD if sh else TXT)
            self.auto.config(text="Cerebro: " + ("automático" if est.get("auto") else "en pausa (manual)"),
                             fg=C_OK if est.get("auto") else C_WARN)
            for key, sub in (("pgen", " W"), ("pcarga", " W")):
                self.pw[key][0].config(text=fmt(est.get(key), 1, sub))
            pb = est.get("pbat")
            self.pw["pbat"][0].config(text=fmt(pb, 1, " W"))
            self.pw["pbat"][1].config(text="" if pb is None else ("entrega energía" if pb > 1 else ("guarda energía" if pb < -1 else "en reposo")))
            self.pw["recorte"][0].config(text=fmt(est.get("recorte_w"), 0, " W"))
            self.pw["recorte"][1].config(text="el cerebro limita la producción" if (est.get("recorte_w") or 0) > 0 else "sin recorte")
        # graficas
        w = self.win.get()
        h = [x for x in hist if x[0] >= now - w - 5]
        if h:
            ts = [x[0] for x in h]
            self.l_v.set_data(ts, [x[1] for x in h]); self.l_s.set_data(ts, [x[2] for x in h])
            for p in self.spans:
                p.remove()
            self.spans = []
            i = 0
            while i < len(h):
                j = i
                while j + 1 < len(h) and h[j + 1][6] == h[i][6]:
                    j += 1
                col = ESTADO_COL.get(h[i][6]) if h[i][6] in ("EXCEDENTE", "DEFICIT", "ARRANQUE", "BLACKSTART") else None
                if col:
                    x1 = h[j + 1][0] if j + 1 < len(h) else h[j][0]
                    self.spans.append(self.a_v.axvspan(h[i][0], x1, color=col, alpha=0.13, lw=0))
                    self.spans.append(self.a_s.axvspan(h[i][0], x1, color=col, alpha=0.13, lw=0))
                i = j + 1
            vs = [x[1] for x in h if x[1] is not None]
            if vs:
                self.a_v.set_ylim(min(44, min(vs) - 1), max(54, max(vs) + 1))
                self.l_z.set_data(ts, [x[1] for x in h])
                self.a_z.set_ylim(min(47.9, min(vs) - 0.02), max(48.1, max(vs) + 0.02))
            self._zoom_eventos(hist, now, w)
            self.a_v.set_xlim(max(0, now - w), max(w, now))
        pmax = 10
        for k, line in self.l_p.items():
            pts = [x for x in subs[k] if x[0] >= now - w - 5]
            line.set_data([x[0] for x in pts], [x[3] for x in pts])
            if pts:
                pmax = max(pmax, max(abs(x[3]) for x in pts))
        self.a_p.set_xlim(max(0, now - w), max(w, now)); self.a_p.set_ylim(-pmax * 1.15, pmax * 1.15)
        self.canvas.draw_idle()
        # feed de eventos
        if nuevos:
            self.feed.config(state="normal")
            for t, txt, tipo in nuevos:
                if tipo in ("cmd", "err"):
                    continue
                self.feed.insert("end", f"{t:6.0f} s   {txt}\n", tipo)
            self.feed.see("end")
            self.feed.config(state="disabled")
        self.subw.update(); self.evw.update()


# ----------------------------------------------------------------------------
# Ventana 2: subsistemas
# ----------------------------------------------------------------------------
class Subsistemas:
    def __init__(self, root, nb):
        self.top, self.nb = root, nb
        self.det = tk.BooleanVar(value=False)
        tk.Checkbutton(self.top, text="  Mostrar detalles técnicos", variable=self.det, bg=BG, fg=MUTED, selectcolor=PANEL2,
                       activebackground=BG, activeforeground=TXT, font=(FONT, 10)).pack(anchor="e", padx=18, pady=(8, 0))
        g = tk.Frame(self.top, bg=BG); g.pack(fill="both", expand=True, padx=14, pady=10)
        for i in range(2):
            g.columnconfigure(i, weight=1, uniform="s"); g.rowconfigure(i, weight=1, uniform="s")
        self.w = {}
        for n, (key, name, col, ico) in enumerate(SUBS):
            c = card(g); c.grid(row=n // 2, column=n % 2, sticky="nsew", padx=6, pady=6)
            tk.Frame(c, bg=col, height=5).pack(fill="x")
            h = tk.Frame(c, bg=PANEL); h.pack(fill="x", padx=14, pady=(8, 0))
            lbl(h, f"{ico}  {name}", 14, True, col).pack(side="left")
            st = tk.Label(h, text="  SIN DATOS  ", bg=C_OFF, fg="#06101F", font=(FONT, 9, "bold")); st.pack(side="right")
            big = tk.Frame(c, bg=PANEL); big.pack(fill="x", padx=14, pady=4)
            p = lbl(big, "— W", 28, True, col); p.pack(side="left")
            vi = lbl(big, "", 12, False, MUTED); vi.pack(side="left", padx=14)
            sp = Spark(c, 480, 62, col); sp.pack(padx=14, pady=4, anchor="w")
            lbl(c, DESCRIPCION[key], 10, False, MUTED).pack(anchor="w", padx=14)
            conv = lbl(c, "", 10, False, MUTED, justify="left"); conv.pack(anchor="w", padx=14)
            ext = lbl(c, "", 10, False, TXT, justify="left"); ext.pack(anchor="w", padx=14, pady=(2, 0))
            rg = lbl(c, "", 10, True, col, justify="left", wraplength=470); rg.pack(anchor="w", padx=14, pady=(4, 10))
            self.w[key] = dict(st=st, p=p, vi=vi, sp=sp, conv=conv, ext=ext, rg=rg, col=col)

    def show(self):
        self.nb.select(self.top)

    def update(self):
        if not self.top.winfo_viewable():
            return
        now = store.now()
        with store.lock:
            data = {k: {kk: (dict(v) if isinstance(v, dict) else v) for kk, v in s.items() if kk != "hist"} for k, s in store.sub.items()}
            hist = {k: list(s["hist"]) for k, s in store.sub.items()}
        for key, w in self.w.items():
            d = data[key]
            m, cv, ex, rg, ct = d["main"], d["conv"], d["extra"], d["regla"], d["ctrl"]
            vivo = d["t"] is not None and now - d["t"] < 5
            stt = m.get("status", 0)
            if not vivo:
                w["st"].config(text="  SIN DATOS  ", bg=C_OFF)
            elif stt:
                w["st"].config(text="  ATENCIÓN  ", bg=C_WARN)
            else:
                w["st"].config(text="  FUNCIONANDO  ", bg=C_OK)
            w["p"].config(text=fmt(m.get("p"), 1, " W"))
            w["vi"].config(text=("potencia en este momento" if not self.det.get() else f"{fmt(m.get('v'), 2, ' V')}   ·   {fmt(m.get('i'), 2, ' A')}"))
            w["sp"].set([(t, p) for t, _, _, p in hist[key]], now)
            det = self.det.get()
            w["conv"].config(text=(f"Convertidor:  Vin {fmt(cv.get('vin'), 2, ' V')}   Vbus {fmt(cv.get('vbus'), 2, ' V')}   "
                                   f"Ibus {fmt(cv.get('ibus'), 2, ' A')}   D {fmt(cv.get('d'), 3)}") if cv and det else "")
            if key == "eolica" and ex:
                t = f"Viento: {fmt(ex.get('viento'), 1, ' m/s')}   ·   turbina girando a {fmt(ex.get('rpm'), 0, ' rpm')}"
            elif key == "bateria" and ex:
                t = f"Carga disponible: {fmt(ex.get('soc'), 0, ' %')}   ·   temperatura {fmt(ex.get('temp'), 0, ' °C')}"
            elif key == "carga" and ex:
                t = f"Consumo: {'fábrica 🏭' if ex.get('esc') == 1 else 'casas 🏠'}   ·   nivel {fmt(ex.get('nivel'), 0, ' %')} de lo que pediría"
            elif key == "pv":
                t = f"Voltaje {fmt(m.get('v'), 1, ' V')}" if (det and m) else ""
            else:
                t = ""
            w["ext"].config(text=t)
            if rg:
                txt = REGLAS_AMIGABLES.get(rg.get("regla", ""), rg.get("texto", ""))
                if det:
                    txt += f"   [{rg.get('regla', '')} · {rg.get('modo', '')}]"
                w["rg"].config(text="▸ " + txt)
            else:
                w["rg"].config(text="")


# ----------------------------------------------------------------------------
# Ventana 3: eventos y comandos
# ----------------------------------------------------------------------------
class Eventos:
    def __init__(self, root, nb):
        self.root = root
        self.top, self.nb = root, nb
        self.enable = tk.BooleanVar(value=False)
        self.running = False
        self.t_start = None
        self.sec = []  # [t, texto, estado]
        self._build()
        self.cargar_preset(list(PRESETS)[0])

    def show(self):
        self.nb.select(self.top)

    # ---- construccion
    def _build(self):
        t = self.top
        head = tk.Frame(t, bg=BG); head.pack(fill="x", padx=14, pady=(12, 4))
        lbl(head, "Control de la demostración", 16, True, C_WARN, BG).pack(side="left")
        c = tk.Checkbutton(head, text="  Permitir que los botones actúen", variable=self.enable, bg=BG, fg=TXT,
                           selectcolor=PANEL2, activebackground=BG, activeforeground=TXT, font=(FONT, 11, "bold"),
                           command=self._enable_changed)
        c.pack(side="right")
        self.aviso = lbl(head, "", 10, True, C_OFF, BG); self.aviso.pack(side="right", padx=12)
        self._enable_changed()

        body = tk.Frame(t, bg=BG); body.pack(fill="both", expand=True, padx=14, pady=6)
        body.columnconfigure(0, weight=1, uniform="e"); body.columnconfigure(1, weight=1, uniform="e")
        body.rowconfigure(0, weight=1)

        # --- izquierda: comandos directos
        L = card(body); L.grid(row=0, column=0, sticky="nsew", padx=(0, 6))
        lbl(L, "ACCIONES RÁPIDAS DE LA DEMOSTRACIÓN", 9, True, MUTED).pack(anchor="w", padx=14, pady=(10, 4))
        f = tk.Frame(L, bg=PANEL); f.pack(fill="x", padx=14)
        for txt, cmd, col in (("Simular apagón y reinicio", "blackstart", C_BAD), ("Consumo de fábrica", "load ind", C_LOAD),
                              ("Consumo de casas", "load res", C_LOAD), ("Cerebro automático", "auto on", C_OK),
                              ("Pausar cerebro", "auto off", C_WARN)):
            button(f, txt, lambda c=cmd: self.enviar(c), col).pack(side="left", padx=(0, 6), pady=2)
        f = tk.Frame(L, bg=PANEL); f.pack(fill="x", padx=14, pady=6)
        lbl(f, "Hora simulada (h):", 10, False, MUTED).pack(side="left")
        self.hora = tk.Spinbox(f, from_=0, to=24, increment=0.5, width=5, bg=PANEL2, fg=TXT, relief="flat",
                               buttonbackground=PANEL2, insertbackground=TXT); self.hora.delete(0, "end"); self.hora.insert(0, "6")
        self.hora.pack(side="left", padx=6)
        button(f, "Fijar", lambda: self.enviar(f"hora {self.hora.get()}"), C_INFO).pack(side="left")

        lbl(L, "CONTROL MANUAL DE CADA PARTE (avanzado)", 9, True, MUTED).pack(anchor="w", padx=14, pady=(14, 4))
        self.sub_ui = {}
        for key, name, col, ico in SUBS:
            f = tk.Frame(L, bg=PANEL2); f.pack(fill="x", padx=14, pady=3)
            tk.Frame(f, bg=col, width=5).pack(side="left", fill="y")
            lbl(f, f"{ico} {name.split(' (')[0]}", 10, True, col, PANEL2, width=14, anchor="w").pack(side="left", padx=6, pady=6)
            mode = ttk.Combobox(f, width=12, state="readonly", values=["mppt/normal", "limitado", "seguro"]); mode.set("mppt/normal")
            mode.pack(side="left", padx=3)
            etiquetas = {"pv": ("Pmax W", None), "eolica": ("Pmax W", None), "bateria": ("I carga A", "I desc. A"),
                         "carga": ("Nivel %", "Esc 0/1")}[key]
            e1 = tk.Entry(f, width=6, bg=PANEL, fg=TXT, insertbackground=TXT, relief="flat"); e1.pack(side="left", padx=(8, 2))
            lbl(f, etiquetas[0], 8, False, MUTED, PANEL2).pack(side="left")
            e2 = None
            if etiquetas[1]:
                e2 = tk.Entry(f, width=6, bg=PANEL, fg=TXT, insertbackground=TXT, relief="flat"); e2.pack(side="left", padx=(8, 2))
                lbl(f, etiquetas[1], 8, False, MUTED, PANEL2).pack(side="left")
            button(f, "Enviar", lambda k=key: self.enviar_sub(k), col).pack(side="right", padx=6, pady=4)
            self.sub_ui[key] = (mode, e1, e2)
        lbl(L, "Los comandos manuales a paneles/turbina pausan el recorte automático hasta pulsar «Cerebro automático».", 9, False, MUTED,
            wraplength=520, justify="left").pack(anchor="w", padx=14, pady=(6, 4))
        lbl(L, "COMANDO LIBRE (avanzado, misma sintaxis del Monitor Serie)", 9, True, MUTED).pack(anchor="w", padx=14, pady=(12, 4))
        f = tk.Frame(L, bg=PANEL); f.pack(fill="x", padx=14, pady=(0, 14))
        self.libre = tk.Entry(f, bg=PANEL2, fg=TXT, insertbackground=TXT, relief="flat", font=("Consolas", 11))
        self.libre.pack(side="left", fill="x", expand=True, ipady=5)
        self.libre.bind("<Return>", lambda e: self.enviar(self.libre.get()))
        button(f, "Enviar", lambda: self.enviar(self.libre.get()), C_PV).pack(side="left", padx=6)

        # --- derecha: programador
        R = card(body); R.grid(row=0, column=1, sticky="nsew", padx=(6, 0))
        lbl(R, "GUIONES: eventos programados en el tiempo", 9, True, MUTED).pack(anchor="w", padx=14, pady=(10, 4))
        f = tk.Frame(R, bg=PANEL); f.pack(fill="x", padx=14)
        lbl(f, "Plantilla:", 10, False, MUTED).pack(side="left")
        self.preset = ttk.Combobox(f, state="readonly", width=30, values=list(PRESETS)); self.preset.set(list(PRESETS)[0])
        self.preset.pack(side="left", padx=6)
        self.preset.bind("<<ComboboxSelected>>", lambda e: self.cargar_preset(self.preset.get()))
        button(f, "Guardar…", self.guardar, PANEL2, TXT).pack(side="right", padx=2)
        button(f, "Abrir…", self.abrir, PANEL2, TXT).pack(side="right", padx=2)
        self.tree = ttk.Treeview(R, columns=("t", "cmd", "est"), show="headings", height=9, selectmode="browse")
        for c_, w_, tx in (("t", 80, "t [s]"), ("cmd", 300, "Comando"), ("est", 110, "Estado")):
            self.tree.heading(c_, text=tx); self.tree.column(c_, width=w_, anchor="w")
        self.tree.pack(fill="both", expand=True, padx=14, pady=8)
        self.tree.tag_configure("hecho", foreground=C_OK); self.tree.tag_configure("error", foreground=C_BAD)
        self.tree.tag_configure("prox", foreground=C_WARN)
        self.tree.bind("<Double-1>", lambda e: self.editar())
        f = tk.Frame(R, bg=PANEL); f.pack(fill="x", padx=14)
        lbl(f, "t [s]", 9, False, MUTED).pack(side="left")
        self.e_t = tk.Entry(f, width=7, bg=PANEL2, fg=TXT, insertbackground=TXT, relief="flat"); self.e_t.pack(side="left", padx=4, ipady=4)
        lbl(f, "comando", 9, False, MUTED).pack(side="left", padx=(8, 0))
        self.e_c = tk.Entry(f, bg=PANEL2, fg=TXT, insertbackground=TXT, relief="flat", font=("Consolas", 11)); self.e_c.pack(side="left", fill="x", expand=True, padx=4, ipady=4)
        button(f, "Agregar", self.agregar, C_OK).pack(side="left", padx=2)
        f = tk.Frame(R, bg=PANEL); f.pack(fill="x", padx=14, pady=6)
        button(f, "Editar", self.editar, PANEL2, TXT).pack(side="left", padx=(0, 4))
        button(f, "Quitar", self.quitar, PANEL2, TXT).pack(side="left", padx=4)
        button(f, "Vaciar", self.vaciar, PANEL2, TXT).pack(side="left", padx=4)
        f = tk.Frame(R, bg=PANEL); f.pack(fill="x", padx=14, pady=(2, 14))
        self.btn_run = button(f, "▶  Iniciar secuencia (t = 0 ahora)", self.toggle_run, C_OK); self.btn_run.pack(side="left")
        self.cuenta = lbl(f, "", 12, True, C_WARN); self.cuenta.pack(side="left", padx=12)

        # --- inferior: registro
        B = card(t); B.pack(fill="x", padx=14, pady=(0, 12))
        lbl(B, "REGISTRO (lo que pasó y lo que se envió)", 9, True, MUTED).pack(anchor="w", padx=14, pady=(8, 0))
        self.log = tk.Text(B, height=8, bg=PANEL, fg=TXT, relief="flat", font=("Consolas", 10), highlightthickness=0, state="disabled", wrap="none")
        self.log.pack(fill="x", padx=10, pady=(2, 8))
        for tag, col in (("evento", TXT), ("esc", C_INFO), ("cmd", C_WARN), ("err", C_BAD)):
            self.log.tag_config(tag, foreground=col)
        self._log_cursor = 0

    # ---- logica
    def _enable_changed(self):
        if self.enable.get():
            self.aviso.config(text="ENVÍO ACTIVO: los botones actúan sobre el sistema", fg=C_WARN)
        else:
            self.aviso.config(text="Solo mirar: los botones no envían nada", fg=C_OFF)

    def enviar(self, texto):
        try:
            topic, payload = parse_cmd(texto)
        except (ValueError, IndexError) as e:
            messagebox.showerror("Comando", str(e), parent=self.top)
            return False
        if not self.enable.get() and not bus.replay:
            store.log(f"Bloqueado (marca «Habilitar envío»): {texto}", "err")
            return False
        return bus.publish(topic, payload)

    def enviar_sub(self, key):
        mode, e1, e2 = self.sub_ui[key]
        m = {"mppt/normal": "mppt", "limitado": "limited", "seguro": "safe"}[mode.get()]
        word = {"pv": "pv", "eolica": "wind", "bateria": "bat", "carga": "load"}[key]
        if key == "carga" and m == "limited":
            m = "reduced"
        parts = [word, m] + [x.get().strip() for x in (e1, e2) if x is not None and x.get().strip()]
        self.enviar(" ".join(parts))

    def _refresh_tree(self):
        self.tree.delete(*self.tree.get_children())
        for i, (t, c, st) in enumerate(self.sec):
            tag = {"enviado": "hecho", "error": "error"}.get(st, "")
            self.tree.insert("", "end", iid=str(i), values=(f"{t:g}", c, st), tags=(tag,) if tag else ())

    def cargar_preset(self, name):
        self.sec = [[float(t), c, "pendiente"] for t, c in PRESETS[name]]
        self._refresh_tree()

    def agregar(self):
        try:
            t = float(self.e_t.get().replace(",", "."))
            parse_cmd(self.e_c.get())
        except (ValueError, IndexError) as e:
            messagebox.showerror("Evento", f"Revisa el tiempo y el comando.\n{e}", parent=self.top)
            return
        self.sec.append([t, self.e_c.get().strip(), "pendiente"]); self.sec.sort(key=lambda r: r[0])
        self._refresh_tree(); self.e_c.delete(0, "end")

    def _sel(self):
        s = self.tree.selection()
        return int(s[0]) if s else None

    def editar(self):
        i = self._sel()
        if i is None:
            return
        self.e_t.delete(0, "end"); self.e_t.insert(0, f"{self.sec[i][0]:g}")
        self.e_c.delete(0, "end"); self.e_c.insert(0, self.sec[i][1])
        del self.sec[i]; self._refresh_tree()

    def quitar(self):
        i = self._sel()
        if i is not None:
            del self.sec[i]; self._refresh_tree()

    def vaciar(self):
        self.sec = []; self._refresh_tree()

    def guardar(self):
        p = filedialog.asksaveasfilename(defaultextension=".json", filetypes=[("Secuencia", "*.json")], parent=self.top)
        if p:
            with open(p, "w", encoding="utf-8") as f:
                json.dump([[t, c] for t, c, _ in self.sec], f, indent=1)

    def abrir(self):
        p = filedialog.askopenfilename(filetypes=[("Secuencia", "*.json")], parent=self.top)
        if p:
            with open(p, encoding="utf-8") as f:
                self.sec = [[float(t), c, "pendiente"] for t, c in json.load(f)]
            self._refresh_tree()

    def toggle_run(self):
        if self.running:
            self.running = False
            self.btn_run.config(text="▶  Iniciar secuencia (t = 0 ahora)", bg=C_OK, activebackground=C_OK)
            store.log("Secuencia detenida", "cmd")
            return
        if not self.enable.get() and not bus.replay:
            messagebox.showinfo("Secuencia", "Marca «Habilitar envío al hardware» para ejecutar la secuencia.", parent=self.top)
            return
        for r in self.sec:
            r[2] = "pendiente"
        self.running, self.t_start = True, time.time()
        self.btn_run.config(text="■  Detener secuencia", bg=C_BAD, activebackground=C_BAD)
        store.log(f"Secuencia iniciada ({len(self.sec)} eventos)", "cmd")
        self._refresh_tree()

    def update(self):
        # registro
        with store.lock:
            ev = list(store.eventos)
        if len(ev) != self._log_cursor:
            self.log.config(state="normal")
            for t, txt, tipo in ev[self._log_cursor:]:
                self.log.insert("end", f"[{t:7.1f}s] {txt}\n", tipo)
            self.log.see("end"); self.log.config(state="disabled")
            self._log_cursor = len(ev)
        # secuencia
        if self.running:
            el = time.time() - self.t_start
            pend = [r for r in self.sec if r[2] == "pendiente"]
            for r in pend:
                if el >= r[0]:
                    ok = self.enviar(r[1])
                    r[2] = "enviado" if ok else "error"
                    self._refresh_tree()
            pend = [r for r in self.sec if r[2] == "pendiente"]
            self.cuenta.config(text=f"t = {el:6.1f} s" + (f"   próximo en {pend[0][0] - el:5.1f} s" if pend else "   (secuencia completa)"))
        else:
            self.cuenta.config(text="")


# ----------------------------------------------------------------------------
def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--broker", default="localhost")
    ap.add_argument("--port", type=int, default=1883)
    ap.add_argument("--replay", help="CSV de logger_raw.py para reproducir sin hardware")
    ap.add_argument("--vel", type=float, default=10.0, help="velocidad del replay (x)")
    ap.add_argument("--abrir-todo", action="store_true", help="(sin efecto, ahora son pestañas)")
    a = ap.parse_args()
    bus.broker, bus.port = a.broker, a.port
    root = tk.Tk()
    style_ttk(root)
    app = Principal(root)
    if a.replay:
        bus.start_replay(a.replay, a.vel)
    else:
        bus.start_live()
    if a.abrir_todo:
        app.subw.show(); app.evw.show()
    try:
        root.mainloop()
    finally:
        if bus.client:
            bus.client.loop_stop(); bus.client.disconnect()


if __name__ == "__main__":
    main()
