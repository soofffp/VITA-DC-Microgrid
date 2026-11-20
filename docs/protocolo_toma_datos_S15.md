# Protocolo de toma de datos — entrega S15 (VITA-DC / Microred DC 48 V)

**Para:** un Claude que acompaña a Sofi y Tomás (Universidad de los Andes) mientras toman los datos de los experimentos que faltan, y después analiza los CSV.
**Idioma de trabajo:** español (informal, directo).
**Contexto:** la nota de S13 fue baja por pendientes de validación; la nota de S15 se promedia con S13 y reemplaza la de S13. Hay ~5 días desde el 2026-11-09. Lo que se obtenga aquí alimenta el informe (máx. 8 páginas + anexos) y el póster.

> Si tienes acceso al PC (terminal), puedes correr el logger y procesar los CSV tú mismo.
> Si no, guía a los humanos paso a paso y analiza los archivos que te suban.
> **No modifiques ni flashees firmware.** Tu trabajo es tomar datos limpios y calcular métricas. Si algo se ve raro, para y reporta.

---

## 0. Qué es el sistema (30 segundos)

- 4 placas STM32F303K8, una por subsistema, cada una con modelo + convertidor + control local en firmware: **PV** (I2C 0x10/0x20), **eólica** (0x11/0x21), **batería** (0x12/0x22, forma el bus de 48 V), **carga** (0x13/0x23).
- 1 ESP32-S3 = gateway: maestro I2C, ejecuta el **SCADA** (máquina de estados) una vez por segundo y publica por MQTT.
- PC: broker Mosquitto local (puerto 1883) + logger.
- Reloj de simulación: **1 día = 360 s** (1 h = 15 s). La batería integra el SOC con aceleración 60× (1 min real = 1 h de banco).
- Banda de operación del bus: 48 V ±5 % = **45.6 a 50.4 V**.

### Estado de partida (ya hecho, no repetir)
- **PV flasheada** con `pv_control.c` corregido (arregla el PV que se quedaba en circuito abierto, 0 W con ~22.3 V, al amanecer).
- **ESP32 con `gateway_v7.ino`**: recorte de generación proporcional al exceso de tensión y deslastre de carga escalonado.
- Eólica, batería y carga: firmware sin cambios.
- **Nada de esto está probado en hardware todavía.** Parte de lo que se busca es confirmarlo.

### Lo que cambió en el SCADA (v7) y que debes saber para interpretar los datos
| Tema | Antes (v6) | Ahora (v7) |
|---|---|---|
| Escalón de recorte (S1/S2) | 5 W fijo | `5 W + 10 W/V × (Vbus − 50.4)`, tope 30 W |
| Cadencia de S2 | cada 6 s, o cada 2 s si Vbus > 53 V | igual |
| Deslastre (S6/S7) | un nivel: 60 % si SOC < 20 %; restaura > 30 % | tres niveles: 60 / 40 / 25 % con SOC < 30 / 25 / 20 %; baja de nivel con SOC > 40 / 35 / 30 %; mínimo 10 s entre cambios |
| `estado` | — | agrega campo `shed` (0 a 3) |

---

## 1. Reglas generales para que los datos valgan (esto fue lo que falló en S13)

1. **Arranque limpio y sincronizado.** Antes de cada corrida, **desconectar y reconectar la alimentación de las 4 STM a la vez** (o el pulsador común de NRST si lo cablearon). Cada STM mide su propio tiempo desde que arranca; si arrancan desfasadas, las corridas no son comparables (en E3 hubo ~17 s de desfase).
2. **Un solo logger a la vez**, un archivo por corrida. Nunca mezclar datos de corridas distintas.
3. **Esperar a que las 4 placas estén en línea** antes de empezar (en la consola del ESP32 o en `microred_ctrl/<s>_online`).
4. **SCADA automático activo** salvo que el experimento diga lo contrario (`auto on`).
5. **Anotar** en un archivo de notas por corrida: fecha/hora, SOC inicial, comandos enviados con su hora, y cualquier anomalía.
6. **Guardar también el log del Monitor Serie del ESP32** (copiar/pegar a `.txt`) como respaldo.
7. **Si el PV se queda en 0 W con ~22.3 V en algún amanecer** (cada 360 s desde el arranque), anótalo con la hora exacta y avisa: significa que el arreglo no funcionó.
8. **Criterio de aborto:** si Vbus supera 58 V, baja de 40 V, o algún subsistema queda "SIN RESPUESTA" de forma sostenida, detener la corrida y reportar.

---

## 2. Logger

El `logger.py` que está en `C:\STM32\scada` es de agosto y **solo guarda `microred/#`** (V, I, P). No guarda `microred_ctrl/#`, donde están el estado del SCADA, Vbus, SOC y los eventos. **Usa `logger_raw.py`** (adjunto; guarda ambos árboles de topics, un mensaje por fila, con timestamp en milisegundos):

```
cd C:\STM32\scada
python logger_raw.py E3_A          # crea runs\<fecha>_E3_A.csv ; Ctrl+C para cerrar
```

Iniciar el logger **antes** de energizar las STM (o inmediatamente después, anotando el desfase), y cerrarlo al terminar la corrida.

### Topics y campos (para parsear el `payload` JSON)
| Topic | Contenido |
|---|---|
| `microred/pv`, `/eolica`, `/bateria`, `/carga` | `{v, i, p, status}` (batería: i y p con signo, + = descarga) |
| `microred/<s>_conv` | `{vin, vbus, ibus, d, flags}` |
| `microred/<s>_extra` | eólica `{viento, f, rpm, vrect}`; batería `{soc, temp, ich_max, idis_max, xflags}`; carga `{nivel, fp, fase, esc}` |
| `microred_ctrl/<s>`, `_reglas`, `_online` | estado del control local, regla local activa (R1–R5, E1–E7, B1–B7, C1–C5), disponibilidad |
| **`microred_ctrl/estado`** (1 Hz) | `{estado, regla, hora, recorte_w, cap_pv, cap_eol, esc, shed, auto, vbus, soc, pgen, pcarga, pbat}` ← **fuente principal de las métricas** |
| **`microred_ctrl/sistema`** | eventos: `{"evento":"S1 ENTRADA a EXCEDENTE..."}`, S2, S3, S4, S6 DESLASTRE, S7 RESTAURAR, S8, `blackstart` pasos 0–3, cambios de escenario |

### Comandos disponibles (por Monitor Serie del ESP32 o por MQTT)
- Serie: `<pv|wind|bat|load> <mppt|normal|limited|reduced|safe> [x] [y]` · `load res` · `load ind` · `auto on|off` · `hora <H>` · `blackstart`
- MQTT: `microred_cmd/sistema` con `blackstart`, `industrial`, `residencial`, `auto_on`, `auto_off`, `{"hora":6}`.
- Ejemplos: `load reduced 40` (carga al 40 %, pausa la secuencia automática de escenarios) · `pv mppt` · `wind mppt` (devuelve el generador a MPPT).
- Un comando manual a PV o eólica pausa el recorte automático hasta `auto on`.

---

## 3. Experimentos, en orden de prioridad

Si falta tiempo, mantén este orden. Tiempo total estimado: ~2 h 15 min de corridas + reinicios.

### P0 — Prueba del arreglo del PV (18 min) — HACER PRIMERO
- **Objetivo:** confirmar que el PV recupera producción en cada amanecer (antes quedaba en 0 W con ~22.3 V).
- **Procedimiento:** reinicio simultáneo de las 4 STM → logger `P0_PV` → dejar correr **3 días simulados (1080 s)** sin tocar nada. Con `auto on` (por defecto).
- **Qué mirar:** en t ≈ 360 s y 720 s (amaneceres 2 y 3), `microred/pv` pasa de ~0 W a producir y llega a ~88 W al mediodía (t ≈ 540, 900, 1260 s). V del PV ~18 V cuando produce.
- **Resultado esperado:** cero amaneceres atascados. Si hay uno, reportar hora y duración.

### E3 — Sin SCADA vs con SCADA (2 corridas de ~1000 s cada una)
Es el experimento que más pesa: demuestra el aporte del SCADA. Las dos corridas deben ser **idénticas salvo por el SCADA**.
- **Corrida A (sin SCADA) y corrida B (con SCADA)**, cada una desde un arranque limpio sincronizado:
  1. t ≈ 0 s: `load reduced 40` (lleva la batería a SOC alto; deshabilita la secuencia automática de carga).
  2. t ≈ 290 s: `load res` (carga residencial fija).
  3. **Solo en A:** en t ≈ 302 s `auto off`, luego `pv mppt` y `wind mppt` para asegurar que no queden topes. En **B** no se envía nada: el SCADA sigue activo.
  4. Registrar hasta t ≈ 1000 s.
- **Condición de validez:** SOC al inicio de la ventana similar en A y B (en S13: 91.6 % vs 91.4 %), y **PV operativo en ambas** (en S13 B tuvo el PV bloqueado).
- **Métricas (ventana desde t = 302 s):** tiempo con Vbus > 50.4 V; tiempo con Vbus > 53 V; Vbus máximo; recorte máximo del SCADA; conteo de eventos S1/S2/S3/S4.
- **Referencia S13 (para comparar):** sin SCADA 50.0 s > 50.4 V, 46.9 s > 53 V, máx 56.98 V; con SCADA 43.4 s, 34.2 s, 56.29 V.

### E4 — Déficit industrial sostenido con deslastre (1080 s)
- **Procedimiento:** reinicio simultáneo (SOC inicial ≈ 50 %) → logger `E4` → en t ≈ 28 s `load ind` (carga industrial fija) → **no enviar más comandos**, SCADA activo, dejar correr **1080 s** (3 días simulados, más largo que en S13 para ver si el ciclo de deslastre/restauración se repite).
- **Métricas:** SOC inicial y mínimo; instante y SOC de cada evento S6 (nivel) y S7; duración total del deslastre; número de cambios de nivel; muestras en estado DEFICIT; potencia máxima de batería; Vbus mín/máx.
- **Referencia S13:** SOC mínimo 13.0 %, S6 a 363.7 s (19.6 %), S7 a 601.6 s.
- **Criterio propuesto:** SOC mínimo ≥ 20 %.

### E2 — Excedente y recorte (≈ 1400 s)
- **Procedimiento (el mismo de S13):** reinicio simultáneo → logger `E2` → `load reduced 40` al inicio para llevar la batería a SOC alto, SCADA activo → registrar ≈ 1400 s. Se esperan episodios de excedente cuando la batería llega a su protección por SOC (~95 %).
- **Métricas por episodio:** hora de S1 y de S4; Vbus máximo; tiempo con Vbus > 50.4 V y > 53 V; conteo S1/S2/S3/S4 (y cuántos S2 son de +0 W, señal de recorte saturado en el piso); tiempo fuera de banda total y como % de la corrida.
- **Referencia S13:** Vbus máx 57.0 V; 130 s fuera de banda (9.2 %); varios S2 de +0 W por piso de generación.
- **Nota:** el techo cercano a 57–58 V es una propiedad del modelo de la batería (repliegue del excedente), no del SCADA. Reportar qué tanto bajó el pico con el escalón proporcional.

### E1 — Operación nominal con carga alternante (≥ 1010 s)
- **Procedimiento:** reinicio simultáneo → logger `E1` → SCADA activo, carga automática (alterna residencial/industrial cada 120 s) → ≥ 1010 s.
- **Métricas por escenario y global:** duración; P carga/PV/eólica/batería medias; % de tiempo con la batería descargando; % de tiempo de Vbus en banda; SOC mínimo.
- **Referencia S13:** Vbus en banda 100 %, SOC mínimo 16.8 %.
- **Criterio propuesto:** SOC mínimo ≥ 20 % (ahora debería cumplirse por el deslastre escalonado).

### E5 — Arranque en negro (≈ 450 s)
- **Procedimiento:** reinicio simultáneo → logger `E5` → `load res` → esperar a que haya sol (t ≈ 90 s, p. ej.) → `blackstart` (de día) → dejar que termine (~25 s) → esperar a la noche simulada (t ≈ 320 s) → `blackstart` (de noche).
- **Métricas por secuencia:** instante de la orden; duración hasta NORMAL; duración de los pasos 0–3; Vbus extremo y cuánto dura; carga mínima durante la secuencia; potencia de la batería.
- **Referencia S13:** 24.8 s de día, 25.4 s de noche; Vbus 52.92 V (4 s) y 45.16 V (2 s).
- **Nota:** el "bus caído" es emulado (en modo seguro los generadores no se desconectan).

### E6 — Aislamiento de fallas por apertura de contactor — **BLOQUEADO**
- Es lo que exige S13 y es la prioridad del informe, pero **aún no está implementado** ni definido. Falta confirmar con el profesor si piden (a) aislar un subsistema por orden, o (b) que el SCADA detecte la falla y aísle por sí solo. No tomar datos de E6 hasta que haya firmware de contactor y definición. Si Sofi o Tomás confirman la rúbrica, ayudar a diseñar el procedimiento.

---

## 4. Criterios cuantitativos de aceptación (PROPUESTOS — confirmar con el equipo)

Definidos por requisitos de diseño, no por los resultados. Reportar cumple/no cumple sin maquillar; si un criterio falla, se reporta así y se explica.

| Exp. | Criterio | Umbral propuesto |
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
| E6 | (por definir) | detección, recuperación del bus, aislamiento de los demás subsistemas |

---

## 5. Después de cada corrida

1. Cerrar el logger (Ctrl+C) y verificar que el CSV tiene filas en `microred_ctrl/estado` y en `microred_ctrl/sistema`.
2. Guardar: `runs\<fecha>_<exp>.csv`, el log del Monitor Serie y las notas de la corrida.
3. Calcular las métricas de la sección 3 desde `microred_ctrl/estado` (Vbus, SOC, potencias, estado, recorte, `shed`) y los eventos. Usa los instantes de los eventos S1–S9 para anotar los cambios de regla.
4. Producir una gráfica por experimento con: Vbus con su banda (45.6 y 50.4 V), potencias de los cuatro subsistemas, SOC, y sombreado del estado (EXCEDENTE / DEFICIT / deslastre). Cada figura necesita un párrafo con: **qué se ve, por qué ocurre y qué implica**.
5. Rellenar la tabla de aceptación (sección 4).

---

## 6. Entregables que necesitan para el póster y el informe

- Tabla de criterios de aceptación con cumple/no cumple y valor medido, por experimento.
- Una figura por experimento (P0, E1–E5) con su párrafo de análisis.
- **Captura de pantalla del SCADA en funcionamiento** (lo pide el profesor). El `scada_dashboard.py` de `C:\STM32\scada` es una versión vieja que solo grafica V/I/P; el informe describe un tablero más completo (Vbus con banda, potencias, SOC, sombreado, línea de estado). Preguntar a Tomás dónde está la versión actual y tomar la captura durante E2 o E3, cuando se vea un episodio de excedente.
- Comparación antes/después: S13 vs S15 para E3 (sin SCADA vs con SCADA) y E4 (SOC mínimo y deslastre).
- Tabla **hardware vs emulación** (qué corre en hardware real: STM, ESP32, I2C, MQTT, SCADA; qué se emula en firmware: fuentes, convertidores, potencia; qué no existe: etapa de potencia, contactores, sensores físicos).
- Para el póster (público no ingeniero): 3 mensajes grandes y 2 o 3 gráficas limpias. No pegar tablas de parámetros.

## 7. Qué hacer si algo sale mal
- **No aparecen los mensajes de `microred_ctrl/#`:** el logger no está suscrito al árbol correcto (usar `logger_raw.py`) o el ESP32 no se conectó al broker (revisar IP del PC en `MQTT_HOST`, firewall del puerto 1883 y que Mosquitto escuche en `0.0.0.0`).
- **Un subsistema "SIN RESPUESTA":** revisar GND común, pull-ups de 4.7 kΩ (un solo par para todo el bus) y alimentación USB propia de cada STM.
- **No hay eventos S6/S7 en E4 pese a SOC bajo:** verificar que el ESP32 corre `gateway_v7` (el Monitor Serie debe decir "Gateway MICRORED v7").
- **Cualquier comportamiento que no entiendas:** parar, guardar los archivos tal cual y reportar a Sofi/Tomás con la hora de la anomalía.
