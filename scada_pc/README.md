# SCADA en el PC

| Script | Qué hace |
|---|---|
| `logger_raw.py` | Se suscribe a `microred/#` y `microred_ctrl/#` y guarda un mensaje por fila en `runs/<fecha>_<nombre>.csv`. |
| `alarmas.py` | Capa de alarmas alineada con ISA-18.2: 11 alarmas con prioridad, retardos, banda muerta y acción del operador. Es un módulo, no un programa; hoy se usa reproduciendo CSV con `analisis/kpi_alarmas.py` y aún no está integrada al tablero. Ver [`docs/normativas.md`](../docs/normativas.md). |
| `scada_dashboard.py` | Tablero con cuatro pestañas: mapa de energía, gráficas y cifras, las cuatro partes y control (comandos, guiones y registro). Solo lee el broker; publica en `microred_cmd/*` únicamente si se marca «Permitir que los botones actúen». |

```bash
pip install -r ../requirements.txt

python logger_raw.py E4                               # registrar una corrida (Ctrl+C para cerrar)
python scada_dashboard.py                             # tablero en vivo (Mosquitto + ESP32 corriendo)
python scada_dashboard.py --replay ../datos/E4.csv --vel 10   # reproducir un CSV, sin hardware
```

El tablero guarda en `runs/comandos_dashboard.csv` cada comando enviado, con hora real, para cruzarlo con el CSV del registrador.
