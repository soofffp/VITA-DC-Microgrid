# Instructivo de retomas — VITA-DC (para Tomás)

**Objetivo:** tomar tres cosas que le faltan al informe de la entrega S15 y que solo se pueden hacer con las STM en la mano:

1. **E4 largo** — cerrar el tercer ciclo de deslastre (en la corrida anterior el tercer ciclo quedó a medias).
2. **E3 en fase** — repetir E3 A (sin SCADA) y B (con SCADA) con las cuatro STM arrancando a la vez. En las corridas anteriores el primer amanecer de B llegó ≈17 s antes que el de A.
3. **Latencias I2C y MQTT** — se miden gratis durante las dos corridas anteriores, con un ESP32 reflasheado con un firmware que solo agrega medición.

Tiempo estimado: 1.5 a 2 horas en total. Todo sale del protocolo `docs/protocolos/protocolo_toma_datos_S15.md`; este instructivo solo lo resume y agrega lo nuevo.

---

## 0. Qué necesitas

- Las 4 STM32 (PV, eólica, batería, carga), el ESP32-S3 y el cableado I2C como en las corridas anteriores.
- El PC con Mosquitto y la carpeta `C:\STM32\scada` con `logger_raw.py`.
- Arduino IDE con la misma configuración con la que se cargó `gateway_v7.ino` (placa ESP32-S3, librería PubSubClient).
- Un solo interruptor o regleta que energice las 4 STM **al mismo tiempo** (o un hub USB con interruptor). Esto es lo que más importa para E3.
- Un papel o archivo de notas por corrida (plantilla en la sección 5).

## 1. Preparación (una sola vez)

1. En Arduino IDE abre `firmware/gateway_esp32/gateway_v7_diag/gateway_v7_diag.ino` del repositorio.
2. Copia tu `secrets.h` (el que ya usas con `gateway_v7`, con el WiFi y el broker) a esa misma carpeta. **No lo subas a GitHub.**
3. Compila y carga al ESP32-S3. Si no compila, no insistas: manda el mensaje de error a Sofi y sigue con el `gateway_v7.ino` normal (se pierde solo la medición de latencia, las corridas valen igual).
4. Verifica en el Monitor Serie que diga "MQTT conectado" y que las 4 placas estén en línea.
5. En una terminal: `mosquitto_sub -t "microred_ctrl/diag" -v`. Debe salir una línea por segundo del estilo `{"i2c_avg_us":...,"i2c_max_us":...,"i2c_n":...,"rtt_us":...,"pub_us":...}`. Si no sale, avisa antes de empezar.

> Esta versión agrega solo la medición (`microred_ctrl/diag`, 1 vez por segundo). La lógica del SCADA es idéntica a la v7. Anótalo en las notas: "firmware: v7 + diag".

## 2. Reglas para que los datos valgan

1. **Arranque sincronizado:** antes de cada corrida, apaga las 4 STM y enciéndelas a la vez con el mismo interruptor. Espera a que las 4 estén en línea.
2. **Un logger por corrida:** `cd C:\STM32\scada` y `python logger_raw.py NOMBRE`. Inícialo antes de energizar o justo después (anota el desfase). Ciérralo con Ctrl+C al terminar.
3. **SCADA automático activo** salvo que el experimento diga otra cosa.
4. **Anota** la hora del PC de cada comando que envíes (el logger guarda la hora, pero la nota te salva si algo falla).
5. **Guarda también el texto del Monitor Serie** del ESP32 en un `.txt` por corrida.
6. **Aborta y avisa** si Vbus > 58 V, Vbus < 40 V, o un subsistema queda "SIN RESPUESTA" de forma sostenida.
7. **Si el PV queda en 0 W con ≈22.3 V en un amanecer**, anótalo con la hora: el arreglo del MPPT falló.
8. No cambies nada más del firmware. No reinicies nada a mitad de una corrida.

Los comandos (`load ind`, `load res`, `load reduced 40`, `auto off`, `pv mppt`, `wind mppt`) se mandan igual que en las corridas de la semana pasada.

## 3. E4 largo (≈ 25 min)

**Por qué:** en la corrida anterior (1083 s) el tercer ciclo de deslastre terminó con el SOC en 21.4 % y aún bajando. Los dos primeros ciclos duraron ≈250 s de nivel 1 a restauración total, así que el tercero debería cerrar hacia t ≈ 1270 s. Hay que llegar a ≈ 1450 s.

1. Reinicio simultáneo (SOC inicial ≈ 50 %). Logger: `python logger_raw.py E4_largo`.
2. En t ≈ 28 s: `load ind`.
3. No mandes más comandos. SCADA activo.
4. Deja correr **1450 s** y cierra el logger.

**Se da por buena si:** el SOC mínimo queda ≥ 20 % y aparece la restauración total (carga de vuelta al 100 %) del tercer ciclo antes de cerrar. Si el SOC baja de 20 %, **no repitas**: es un resultado y hay que reportarlo así.

## 4. E3 A y B en fase (≈ 2 × 20 min + reinicios)

Las dos corridas deben ser **idénticas salvo por el SCADA**. Haz primero A y luego B.

**Corrida A (sin SCADA)**
1. Reinicio simultáneo. Logger: `python logger_raw.py E3_A`.
2. t ≈ 0 s: `load reduced 40`.
3. t ≈ 290 s: `load res`.
4. t ≈ 302 s: `auto off`, luego `pv mppt` y `wind mppt`.
5. Registra hasta t ≈ 1000 s.

**Corrida B (con SCADA)**
1. Reinicio simultáneo (igual que A). Logger: `python logger_raw.py E3_B`.
2. t ≈ 0 s: `load reduced 40`.
3. t ≈ 290 s: `load res`.
4. No envíes nada más: el SCADA sigue activo.
5. Registra hasta t ≈ 1000 s.

**Verificación inmediata (antes de desmontar nada):**

```
python analisis/check_fase_E3.py datos/E3_A.csv datos/E3_B.csv
```

(Copia los CSV a `datos/` con esos nombres, o pasa las rutas de `runs\`.) El script comprueba:
- diferencia entre los amaneceres de A y B ≤ 2 s,
- SOC en t = 302 s dentro de 2 puntos,
- PV entregando potencia en las dos,
- duración ≥ 1000 s.

Si dice **NO VALIDA**, repite la corrida que haga falta (normalmente B). Con las corridas actuales da 16.8 s de diferencia de amanecer, así que esa es la que hay que mejorar.

## 5. Notas por corrida (plantilla)

```
Corrida: ______  Fecha/hora inicio (PC): ______
Firmware ESP32: v7 + diag      PV: pv_control corregido
SOC inicial: ____  Desfase logger-energizado: ____ s
Comandos (hora PC):  ______________________________
Anomalías: ________________________________________
```

## 6. Opcional (si sobra tiempo): E6 con la hora exacta

En la corrida E6 anterior no se registró el instante exacto de la desconexión del PV. Si la repiten: operación normal con SCADA activo; anota **la hora del PC, al segundo,** en que desenchufas el USB del PV y en que lo vuelves a enchufar (≈ 44 s después). Con eso se puede dar el tiempo de detección con precisión.

## 7. Qué mandarle a Sofi

- `E4_largo.csv`, `E3_A.csv`, `E3_B.csv` (y `E6.csv` si se repitió).
- Las salidas del `check_fase_E3.py`.
- Las notas y los `.txt` del Monitor Serie.
- Cualquier cosa rara, aunque parezca menor.

Con eso se calculan las latencias (campo `diag`), se cierra el tercer ciclo de E4 y se actualiza E3 en el informe.
