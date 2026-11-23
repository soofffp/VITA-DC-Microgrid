# Protocolo experimental — VITA-DC

Procedimiento, métricas y criterios de aceptación de los experimentos de la microrred emulada. Los datos de cada corrida están en `datos/` y las métricas se calculan con `analisis/metricas_experimentos.py`.

## 1. El sistema en 30 segundos

- 4 placas STM32F303K8, una por subsistema, cada una con modelo, convertidor y control local en firmware: **PV** (I2C 0x10/0x20), **eólica** (0x11/0x21), **batería** (0x12/0x22, forma el bus de 48 V) y **carga** (0x13/0x23).
- 1 ESP32-S3 como gateway: maestro I2C, ejecuta el **SCADA** (máquina de estados) una vez por segundo y publica por MQTT.
- PC con broker Mosquitto local (puerto 1883) y registrador.
- Reloj de simulación: **1 día = 360 s** (1 h = 15 s). La batería integra el SOC con aceleración 60×.
- Banda de operación del bus: 48 V ±5 % = **45.6 a 50.4 V**.

**Firmware con el que se tomaron los experimentos:** gateway v7 (recorte de generación proporcional al exceso de tensión y deslastre escalonado) y PV con el MPPT P&O corregido. Los datos en `datos/linea_base_v6/` son de la versión anterior (gateway v6) y sirven solo como línea base.

| Tema | v6 | v7 |
|---|---|---|
| Escalón de recorte (S1/S2) | 5 W fijo | `5 W + 10 W/V × (Vbus − 50.4)`, tope 30 W |
| Cadencia de S2 | cada 6 s, o cada 2 s si Vbus > 53 V | igual |
| Deslastre (S6/S7) | un nivel: 60 % con SOC < 20 %; restaura con SOC > 30 % | tres niveles: 60 / 40 / 25 % con SOC < 30 / 25 / 20 %; baja de nivel con SOC > 40 / 35 / 30 %; mínimo 10 s entre cambios |
| `estado` | — | agrega el campo `shed` (0 a 3) |

## 2. Reglas para que los datos valgan

1. **Arranque sincronizado.** Antes de cada corrida, desconectar y reconectar la alimentación de las 4 STM a la vez (o usar un pulsador común de NRST). Cada STM mide su propio tiempo desde que arranca; si arrancan desfasadas, las corridas no son comparables.
2. **Un solo registrador a la vez**, un archivo por corrida. Nunca mezclar corridas.
3. **Esperar a que las 4 placas estén en línea** antes de empezar (consola del ESP32 o `microred_ctrl/<s>_online`).
4. **SCADA automático activo** salvo que el experimento diga lo contrario (`auto on`).
5. **Anotar** por corrida: fecha y hora, SOC inicial, comandos enviados con su hora y cualquier anomalía.
6. **Guardar el log del Monitor Serie** del ESP32 como respaldo.
7. **Si el PV queda en 0 W con ≈ 22.3 V en un amanecer** (cada 360 s desde el arranque), anotar la hora exacta: el MPPT falló.
8. **Criterio de aborto:** Vbus > 58 V, Vbus < 40 V, o un subsistema "SIN RESPUESTA" de forma sostenida.

## 3. Registro

`scada_pc/logger_raw.py` guarda `microred/#` y `microred_ctrl/#`, un mensaje por fila, con hora del PC en milisegundos:

```
python scada_pc/logger_raw.py E3_A       # crea runs/<fecha>_E3_A.csv ; Ctrl+C para cerrar
```

Iniciarlo antes de energizar las STM (o justo después, anotando el desfase).

### Tópicos y campos

| Tópico | Contenido |
|---|---|
| `microred/pv`, `/eolica`, `/bateria`, `/carga` | `{v, i, p, status}` (batería: i y p con signo, + = descarga) |
| `microred/<s>_conv` | `{vin, vbus, ibus, d, flags}` |
| `microred/<s>_extra` | eólica `{viento, f, rpm, vrect}`; batería `{soc, temp, ich_max, idis_max, xflags}`; carga `{nivel, fp, fase, esc}` |
| `microred_ctrl/<s>`, `_reglas`, `_online` | estado del control local, regla local activa y disponibilidad |
| **`microred_ctrl/estado`** (1 Hz) | `{estado, regla, hora, recorte_w, cap_pv, cap_eol, esc, shed, auto, vbus, soc, pgen, pcarga, pbat}`: fuente principal de las métricas |
| **`microred_ctrl/sistema`** | eventos: S1 entrada a EXCEDENTE, S2, S3, S4, S6 deslastre, S7 restauración, S8, pasos 0–3 del arranque en negro y cambios de escenario |

### Comandos (Monitor Serie del ESP32, tablero SCADA o MQTT)

- Serie: `<pv|wind|bat|load> <mppt|normal|limited|reduced|safe> [x] [y]` · `load res` · `load ind` · `auto on|off` · `hora <H>` · `blackstart`
- MQTT: `microred_cmd/sistema` con `blackstart`, `industrial`, `residencial`, `auto_on`, `auto_off` o `{"hora":6}`.
- `load reduced 40` lleva la carga al 40 % y pausa la secuencia automática de escenarios. `pv mppt` y `wind mppt` devuelven el generador a MPPT.
- Un comando manual a PV o eólica pausa el recorte automático hasta `auto on`.

## 4. Experimentos

### P0 — Arranque del PV al amanecer y deslastre (≈ 1080 s)
- **Objetivo:** confirmar que el PV recupera producción en cada amanecer.
- **Procedimiento:** reinicio simultáneo, registrador `P0`, dejar correr 3 días simulados sin tocar nada, con `auto on`.
- **Qué mirar:** en t ≈ 360 s y 720 s `microred/pv` pasa de ≈ 0 W a producir y llega a ≈ 88 W al mediodía.

### E1 — Operación nominal con carga alternante (≥ 1010 s)
- **Procedimiento:** reinicio simultáneo, registrador `E1`, SCADA activo y carga automática (alterna residencial e industrial cada 120 s).
- **Métricas por escenario y global:** potencias medias de carga y generación, % de tiempo con la batería descargando, % de tiempo de Vbus en banda y SOC mínimo.

### E2 — Excedente y recorte (≈ 1400 s)
- **Procedimiento:** reinicio simultáneo, registrador `E2`, `load reduced 40` al inicio para llevar la batería a SOC alto, SCADA activo.
- **Métricas por episodio:** instantes de S1 y S4, Vbus máximo, tiempo sobre 50.4 V y sobre 53 V, conteo de S1–S4 (y cuántos S2 son de +0 W, señal de recorte saturado en el piso) y tiempo fuera de banda.
- **Nota:** el techo de 57–58 V es una propiedad del modelo de la batería (repliegue del excedente) y no de la lógica del SCADA.

### E3 — Sin SCADA frente a con SCADA (2 corridas de ≈ 1000 s)
Las dos corridas deben ser idénticas salvo por el SCADA.
- **Corrida A (sin SCADA):** reinicio simultáneo. t ≈ 0 s: `load reduced 40`. t ≈ 290 s: `load res`. t ≈ 302 s: `auto off`, `pv mppt`, `wind mppt`. Registrar hasta t ≈ 1000 s.
- **Corrida B (con SCADA):** igual, pero sin enviar nada después de `load res`.
- **Condición de validez:** SOC similar al inicio de la ventana en A y B, PV operativo en ambas y amaneceres desfasados menos de 2 s (`analisis/check_fase_E3.py`).
- **Métricas (ventana desde t = 302 s):** tiempo con Vbus > 50.4 V y > 53 V, Vbus máximo, recorte máximo y conteo de eventos S1–S4.
- **Referencia del firmware anterior (v6):** sin SCADA 50.0 s > 50.4 V, 46.9 s > 53 V, máximo 56.98 V; con SCADA 43.4 s, 34.2 s, 56.29 V.

### E4 — Déficit industrial sostenido con deslastre (≥ 1080 s)
- **Procedimiento:** reinicio simultáneo (SOC inicial ≈ 50 %), registrador `E4`, en t ≈ 28 s `load ind`, sin más comandos y con el SCADA activo. Para cerrar el tercer ciclo de deslastre conviene llegar a ≈ 1450 s.
- **Métricas:** SOC inicial y mínimo, instante y SOC de cada evento S6 y S7, duración total del deslastre, número de cambios de nivel, muestras en DEFICIT y Vbus mínimo y máximo.
- **Referencia del firmware anterior (v6):** SOC mínimo 13.0 %, S6 a 363.7 s (19.6 %), S7 a 601.6 s.

### E5 — Arranque en negro (≈ 450 s)
- **Procedimiento:** reinicio simultáneo, registrador `E5`, `load res`, esperar a que haya sol (t ≈ 90 s) y enviar `blackstart`; esperar a que termine (≈ 25 s), esperar a la noche simulada (t ≈ 320 s) y repetir.
- **Métricas por secuencia:** instante de la orden, duración hasta NORMAL, duración de los pasos 0–3, Vbus extremo y su duración, y carga mínima durante la secuencia.
- **Nota:** el bus caído es emulado: en modo seguro los generadores no se desconectan.

### E6 — Falla de un generador (emulada)
- **Procedimiento:** con el sistema en operación normal y el SCADA activo, desenergizar la placa del PV (desconectar su USB) durante ≈ 40–45 s y reconectarla. Anotar la hora del PC de ambas maniobras.
- **Métricas:** tiempo de detección (último dato del PV hasta `pv_online = 0`), comportamiento del bus y de la batería durante la falla, y tiempo de recuperación del PV al reconectar.
- **Alcance:** no es la apertura de un contactor. El SCADA marca el subsistema fuera de línea y es la batería, que regula el bus, la que cubre el faltante. Al reconectar, la placa reinicia su reloj y empieza de nuevo el día simulado.

## 5. Criterios de aceptación

Definidos a partir de los requisitos de diseño y no de los resultados. Se reporta cumple o no cumple con el valor medido.

| Exp. | Criterio | Umbral |
|---|---|---|
| P0 | Amaneceres con PV atascado | 0 de 3 |
| E1 | Vbus en banda (45.6–50.4 V) | ≥ 95 % del tiempo |
| E1, E4 | SOC mínimo | ≥ 20 % |
| E2, E3 | Vbus máximo | ≤ 53 V |
| E2, E3 | Entrada a EXCEDENTE (S1) tras cruzar 50.4 V | ≤ 2 ciclos (2 s) |
| E3 | Reducción con SCADA del tiempo > 53 V respecto a sin SCADA | ≥ 30 % |
| E4 | Latencia del deslastre tras cruzar el umbral | ≤ 2 s |
| E5 | Duración hasta NORMAL | ≤ 30 s |
| E5 | Excursión de Vbus | ≤ ±10 % (43.2–52.8 V) |
| E6 | Detección del subsistema caído; el bus se mantiene en banda | ≤ 5 s; 100 % del tiempo |

## 6. Después de cada corrida

1. Cerrar el registrador (Ctrl+C) y verificar que el CSV tiene filas en `microred_ctrl/estado` y en `microred_ctrl/sistema`.
2. Guardar `runs/<fecha>_<exp>.csv`, el log del Monitor Serie y las notas.
3. Calcular las métricas con `analisis/metricas_experimentos.py`.
4. Graficar Vbus con su banda (45.6 y 50.4 V), potencias de los cuatro subsistemas, SOC y sombreado del estado (EXCEDENTE, DEFICIT, deslastre).
5. Rellenar la tabla de criterios.

## 7. Si algo sale mal

- **No aparecen los mensajes de `microred_ctrl/#`:** el registrador no está suscrito al árbol correcto (usar `logger_raw.py`) o el ESP32 no se conectó al broker (revisar la IP del PC en `MQTT_HOST`, el firewall del puerto 1883 y que Mosquitto escuche en `0.0.0.0`).
- **Un subsistema "SIN RESPUESTA":** revisar GND común, pull-ups de 4.7 kΩ (un solo par para todo el bus) y la alimentación USB propia de cada STM.
- **No hay eventos S6/S7 en E4 pese a SOC bajo:** verificar que el ESP32 corre `gateway_v7` (el Monitor Serie debe decir "Gateway MICRORED v7").
