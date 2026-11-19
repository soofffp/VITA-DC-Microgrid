"""
scada_dashboard.py -- Dashboard SCADA en vivo para el proyecto Microred.

Se suscribe al mismo broker MQTT que logger.py (microred/#) y muestra en
vivo, en una sola ventana, el voltaje, la corriente y la potencia de los
tres subsistemas (PV, eolica, bateria). Pensado para correr en el mismo
PC que ya hace de "centro de control SCADA" del proyecto (Mosquitto +
logger.py), como una tercera terminal.

No reemplaza a logger.py (que sigue siendo el que deja el historial en
CSV) -- este script es solo la capa de visualizacion en tiempo real,
equivalente en espiritu a la pantalla de KingView del paper de
referencia (Xiao et al. 2017), aunque mucho mas simple.

Requisitos:
    pip install paho-mqtt matplotlib

Uso:
    python scada_dashboard.py
    (Ctrl+C o cerrar la ventana para salir)
"""

import json
import time
from collections import deque
from datetime import datetime

import matplotlib.pyplot as plt
import matplotlib.animation as animation
import paho.mqtt.client as mqtt

# --------------------------------------------------------------------------
# Configuracion
# --------------------------------------------------------------------------
BROKER = "localhost"     # el broker corre en este mismo PC
PORT = 1883
TOPIC = "microred/#"
VENTANA_S = 120           # segundos de historia visibles en cada grafica
REFRESCO_MS = 500         # cada cuanto se redibuja la pantalla

# Un color fijo por subsistema, consistente en las tres graficas
# (paleta apta para daltonismo: azul / naranja / verde)
SUBSISTEMAS = {
    "pv":      {"color": "#0072B2", "label": "PV"},
    "eolica":  {"color": "#E69F00", "label": "Eólica"},
    "bateria": {"color": "#009E73", "label": "Batería"},
    "carga":   {"color": "#CC79A7", "label": "Carga"},
}

# --------------------------------------------------------------------------
# Buffers de datos (thread-safe por simplicidad: solo se agrega al final
# desde el hilo de MQTT y se lee desde el hilo principal de matplotlib;
# deque.append/iteración es atómico a nivel de GIL para este uso)
# --------------------------------------------------------------------------
MAXLEN = int(VENTANA_S * 1000 / 200)  # margen holgado asumiendo ~5 msg/s

buffers = {
    nombre: {
        "t": deque(maxlen=MAXLEN),
        "v": deque(maxlen=MAXLEN),
        "i": deque(maxlen=MAXLEN),
        "p": deque(maxlen=MAXLEN),
        "status": 0,
        "ultimo": None,
    }
    for nombre in SUBSISTEMAS
}

t0 = time.time()


def on_connect(client, userdata, flags, reason_code, properties=None):
    print(f"[scada_dashboard] Conectado al broker ({reason_code})")
    client.subscribe(TOPIC)


def on_message(client, userdata, msg):
    nombre = msg.topic.split("/")[-1]
    if nombre not in buffers:
        return  # subsistema desconocido (p.ej. cuando se agregue "carga"
                # habra que sumarlo a SUBSISTEMAS arriba)
    try:
        data = json.loads(msg.payload.decode())
    except (ValueError, UnicodeDecodeError):
        return

    b = buffers[nombre]
    b["t"].append(time.time() - t0)
    b["v"].append(data.get("v", 0.0))
    b["i"].append(data.get("i", 0.0))
    b["p"].append(data.get("p", 0.0))
    b["status"] = data.get("status", 0)
    b["ultimo"] = datetime.now().strftime("%H:%M:%S")


# --------------------------------------------------------------------------
# Cliente MQTT en su propio hilo (loop_start), matplotlib se queda con el
# hilo principal para poder dibujar
# --------------------------------------------------------------------------
client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id="scada_dashboard")
client.on_connect = on_connect
client.on_message = on_message
client.connect(BROKER, PORT, keepalive=30)
client.loop_start()

# --------------------------------------------------------------------------
# Figura: 3 filas (V, I, P) x 1 columna, una linea por subsistema en cada una
# --------------------------------------------------------------------------
plt.style.use("seaborn-v0_8-darkgrid" if "seaborn-v0_8-darkgrid" in plt.style.available else "default")
fig, (ax_v, ax_i, ax_p) = plt.subplots(3, 1, sharex=True, figsize=(9, 8))
fig.suptitle("Microred DC 48V — SCADA en vivo (PV / Eólica / Batería)", fontsize=13, fontweight="bold")

lines = {}
for ax, key, ylabel in ((ax_v, "v", "Voltaje [V]"), (ax_i, "i", "Corriente [A]"), (ax_p, "p", "Potencia [W]")):
    ax.set_ylabel(ylabel)
    ax.grid(True, alpha=0.3)
    for nombre, cfg in SUBSISTEMAS.items():
        (line,) = ax.plot([], [], color=cfg["color"], label=cfg["label"], linewidth=1.8)
        lines[(key, nombre)] = line
    ax.legend(loc="upper right", fontsize=8, framealpha=0.9)

ax_p.set_xlabel("Tiempo desde el arranque del dashboard [s]")

status_text = fig.text(0.01, 0.01, "", fontsize=8, family="monospace")


def actualizar(frame):
    ahora = time.time() - t0
    for key, ax in (("v", ax_v), ("i", ax_i), ("p", ax_p)):
        ymin, ymax = 0.0, 1.0
        for nombre in SUBSISTEMAS:
            b = buffers[nombre]
            xs = list(b["t"])
            ys = list(b[key])
            lines[(key, nombre)].set_data(xs, ys)
            if ys:
                ymin = min(ymin, min(ys))
                ymax = max(ymax, max(ys))
        ax.set_xlim(max(0, ahora - VENTANA_S), max(VENTANA_S, ahora))
        margen = (ymax - ymin) * 0.1 or 1.0
        ax.set_ylim(ymin - margen, ymax + margen)

    lineas_status = []
    for nombre, cfg in SUBSISTEMAS.items():
        b = buffers[nombre]
        ultimo = b["ultimo"] or "sin datos"
        lineas_status.append(f"{cfg['label']:8s} status={b['status']}  último={ultimo}")
    status_text.set_text("   |   ".join(lineas_status))

    return list(lines.values()) + [status_text]


ani = animation.FuncAnimation(fig, actualizar, interval=REFRESCO_MS, blit=False, cache_frame_data=False)

plt.tight_layout(rect=(0, 0.03, 1, 0.96))

try:
    plt.show()
finally:
    client.loop_stop()
    client.disconnect()
