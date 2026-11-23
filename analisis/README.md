# Análisis

| Script | Qué hace |
|---|---|
| `metricas_experimentos.py` | Lee `datos/*.csv` (tópico `microred_ctrl/estado`) y calcula las métricas del informe: duración, % en banda, V máx., tiempo sobre 53 V, SOC inicial y mínimo, % de descarga, recorte máximo, fracción por estado, eventos S1/S4/S6/S7, escalones S2 (+0 W), pasos del arranque en negro y huecos de telemetría. Guarda la salida en `docs/resultados_metricas.txt`. |
| `check_fase_E3.py` | Verifica que dos corridas de E3 estén en fase: amaneceres a ≤ 2 s, SOC en t = 302 s dentro de 2 puntos, PV entregando, duración ≥ 1000 s. |

```bash
python analisis/metricas_experimentos.py
python analisis/check_fase_E3.py datos/E3_A.csv datos/E3_B.csv
```

**Método de conteo.** Los tiempos del informe cuentan muestras de 1 s del estado del SCADA. Este script pondera por el intervalo real entre muestras, así que puede diferir hasta un 2.4 % de las cifras del informe.
