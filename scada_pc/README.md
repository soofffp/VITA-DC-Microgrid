# SCADA en el PC

| Script | Qué hace |
|---|---|
| `logger_raw.py` | Se suscribe a `microred/#` y `microred_ctrl/#` y guarda un mensaje por fila en `runs/<fecha>_<nombre>.csv`. |
| `scada_dashboard.py` | Tablero con cuatro pestañas: mapa de energía, gráficas y cifras, las cuatro partes y control (comandos, guiones y registro). Solo lee el broker; publica en `microred_cmd/*` únicamente si se marca «Permitir que los botones actúen». |

```bash
pip install -r ../requirements.txt

python logger_raw.py E4                               # registrar una corrida (Ctrl+C para cerrar)
python scada_dashboard.py                             # tablero en vivo (Mosquitto + ESP32 corriendo)
python scada_dashboard.py --replay ../datos/E4.csv --vel 10   # reproducir un CSV, sin hardware
```

El tablero guarda en `runs/comandos_dashboard.csv` cada comando enviado, con hora real, para cruzarlo con el CSV del registrador.
