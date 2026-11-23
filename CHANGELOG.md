# Historial de versiones

## v1.0 (noviembre de 2026)

Primera versión completa y validada del sistema: cuatro subsistemas emulados, SCADA con recorte y deslastre escalonado, tablero, registrador, datos de todos los experimentos y análisis reproducible.

### Punto de partida

La versión anterior (gateway v6) funcionaba, pero tenía tres limitaciones medidas en sus experimentos (`datos/linea_base_v6/`):

- El PV quedaba en circuito abierto al amanecer en 3 ocasiones: la regla de inversión del P&O solo actúa cuando la potencia baja, y de noche nunca se cumple.
- El recorte de generación en excedente era de 5 W fijos, y el SCADA bajó el tiempo con el bus sobre 53 V solo un 27 %.
- El deslastre tenía un solo nivel (60 % de la carga con SOC < 20 %) y el SOC llegó a 13.0 % con carga industrial sostenida.

### Cambios

| Área | Cambio | Efecto medido |
|---|---|---|
| PV | MPPT P&O: si p < 0.5 W la referencia se estaciona en su valor inicial; si toca un límite se invierte el sentido | 0 de 3 amaneceres atascados |
| Gateway v7 | Recorte proporcional al exceso de tensión: ΔP = mín{30, 5 + 10·(Vbus − 50.4)} W, cada 6 s (2 s si Vbus > 53 V) | Tiempo sobre 53 V: −71 % con SCADA (52.5 → 15.2 %) |
| Gateway v7 | Deslastre escalonado y anticipado: 60 / 40 / 25 % con SOC < 30 / 25 / 20 %, restauración con 10 puntos de histéresis | SOC mínimo: 13.0 → 21.4 % (E4) y 16.8 → 24.8 % (E1) |
| Gateway v7 | Campo `shed` (0 a 3) en `microred_ctrl/estado` | Deslastre trazable en los datos |
| PC | Tablero SCADA con mapa de energía, gráficas, las cuatro partes y control | Replay de CSV sin hardware |
| Análisis | `analisis/metricas_experimentos.py` y `check_fase_E3.py` | Cifras del informe reproducibles desde los CSV |

### Limitaciones conocidas

- El pico de ≈ 58 V en excedente con la batería llena no se evita: el recorte llega a su piso (13–15 W por generador) y supera la carga.
- En E2 el bus pasó más tiempo fuera de banda que con v6 (22.0 frente a 9.2 %), pero las corridas no son idénticas.
- En el arranque en negro de día el bus llegó a 53.81 V, fuera de ±10 %.
- Cada experimento es una sola corrida; el tercer ciclo de E4 no terminó y E3 A y B no arrancaron en la misma fase.
- Falta el firmware actual de eólica, batería y carga en `firmware/`.

## v2.0 (previsto)

Llevar los subsistemas a hardware físico: fuentes y etapas de potencia reales, con contactores de aislamiento.
