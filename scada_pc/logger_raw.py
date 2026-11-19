"""
logger_raw.py -- registra TODO lo que publica la microred (microred/# y microred_ctrl/#)
en un CSV crudo, una fila por mensaje MQTT. Sin interpretar nada: el analisis se hace despues.

Uso (una corrida = un archivo):
    python logger_raw.py E3_A
    -> crea runs/<fecha_hora>_E3_A.csv   (Ctrl+C para cerrar)

Columnas: t_pc_iso (con milisegundos), t_rel_s (segundos desde el primer mensaje), topic, payload (JSON crudo)

Requisitos: pip install paho-mqtt
"""
import csv
import os
import sys
import time
from datetime import datetime

import paho.mqtt.client as mqtt

BROKER = "localhost"
PORT = 1883
TOPICS = ["microred/#", "microred_ctrl/#"]

run = sys.argv[1] if len(sys.argv) > 1 else "sin_nombre"
os.makedirs("runs", exist_ok=True)
path = os.path.join("runs", f"{datetime.now():%Y%m%d_%H%M%S}_{run}.csv")
fh = open(path, "w", newline="", encoding="utf-8")
w = csv.writer(fh)
w.writerow(["t_pc_iso", "t_rel_s", "topic", "payload"])
t_first = None
n = 0


def on_connect(client, userdata, flags, reason_code, properties=None):
    print(f"Conectado al broker ({reason_code}). Guardando en {path}")
    for t in TOPICS:
        client.subscribe(t)


def on_message(client, userdata, msg):
    global t_first, n
    now = time.time()
    if t_first is None:
        t_first = now
    payload = msg.payload.decode("utf-8", errors="replace")
    w.writerow([datetime.fromtimestamp(now).isoformat(timespec="milliseconds"),
                f"{now - t_first:.3f}", msg.topic, payload])
    fh.flush()
    n += 1
    # en pantalla solo lo importante: estado del SCADA y eventos
    if msg.topic == "microred_ctrl/estado":
        print(f"[{now - t_first:7.1f}s] {payload}")
    elif msg.topic == "microred_ctrl/sistema":
        print(f"[{now - t_first:7.1f}s] EVENTO {payload}")


client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id=f"logger_raw_{run}")
client.on_connect = on_connect
client.on_message = on_message
client.connect(BROKER, PORT, 60)
try:
    client.loop_forever()
except KeyboardInterrupt:
    pass
finally:
    fh.close()
    print(f"\nCerrado. {n} mensajes guardados en {path}")
