# VITA-DC: microrred DC de 48 V emulada (STM32 + ESP32-S3)

Plataforma de emulación de una microrred DC de 48 V con cuatro subsistemas (arreglo fotovoltaico, turbina eólica, banco de baterías y carga variable) y un SCADA. Proyecto de Ingeniería Eléctrica y Electrónica, Universidad de los Andes.

Cada subsistema corre en una STM32F303K8 (modelo de la fuente, convertidor promediado y control local en una interrupción de 10 kHz). Un gateway ESP32-S3 las lee por I2C, ejecuta el SCADA y publica por MQTT hacia un registrador y un tablero en el PC.

> Todo lo que hay entre una fuente y el bus es software: no hay etapa de potencia física.

## Estructura

| Carpeta | Contenido |
|---|---|
| `firmware/stm32_pv`, `stm32_eolica`, `stm32_bateria`, `stm32_carga` | Proyectos CubeMX + CMake de cada placa |
| `firmware/gateway_esp32/gateway_v7` | Sketch de Arduino del gateway (SCADA + MQTT) |
| `scada_pc` | `logger_raw.py` (CSV crudo) |
| `datos` | CSV de cada experimento (P0, E1–E4; E5 y E6 pendientes) |
| `docs` | Protocolo de toma de datos y figuras del informe |

## Reproducir

1. **STM32** (una por subsistema): compilar con `cmake --preset <preset>` y `cmake --build`, y cargar por SWD con `STM32_Programmer_CLI -c port=SWD -w <archivo>.elf -v -rst`. Energizar o reiniciar las cuatro placas **a la vez** para que sus relojes de 360 s queden en fase.
2. **Gateway**: copiar `firmware/gateway_esp32/gateway_v7/secrets.example.h` a `secrets.h`, poner la red Wi-Fi y la IP del broker, y cargar el sketch desde Arduino IDE.
3. **PC**: iniciar Mosquitto y luego `python scada_pc/logger_raw.py` (guarda todos los topics en un CSV) y `python scada_pc/scada_dashboard.py` (tablero).

## Experimentos y datos

Todos los CSV están en `datos/`. Cada archivo indica con qué versión de firmware se tomó.

| Exp. | Qué valida | Archivo | Firmware |
|---|---|---|---|
| P0 | PV sin atasco al amanecer + deslastre escalonado (≈3.3 días simulados) | `P0_demo.csv` | S14 (gateway v7, MPPT corregido) |
| E1 | Operación normal del bus (sin SCADA actuando) | `E1_log.csv` | S13 |
| E2 | Excedente y tope de tensión | `E2_log.csv` | S13 |
| E3 | Coordinación PV/eólica en excedente: A (sin SCADA) vs B (con SCADA) | `E3_A.csv`, `E3_B.csv` | S14 (repetición del 18/11/2026) |
| E4 | Déficit y deslastre de la carga | `E4.csv` | S14 (repetición del 18/11/2026) |
| E5 | Perfil dinámico de carga | pendiente de subir | -- |
| E6 | Aislamiento de falla por contactor | pendiente | -- |

Formatos: `E1_log.csv` y `E2_log.csv` son del registrador anterior (una fila por subsistema o por muestra del SCADA). `P0_demo.csv`, `E3_*.csv` y `E4.csv` son del registrador actual (`logger_raw.py`): una fila por mensaje MQTT, con columnas `t_pc_iso, t_rel_s, topic, payload` (el payload es JSON).

Los criterios de aceptación y el procedimiento de cada experimento están en `docs/protocolo_toma_datos_S14.md`.

## Autores

Sofía Parra Rozo y Tomás Durán Cardoso. Departamento de Ingeniería Eléctrica y Electrónica, Universidad de los Andes.
