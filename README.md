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
| `datos` | CSV de cada experimento (P0, E1–E6) |
| `analisis` | `metricas_experimentos.py`: métricas de P0 y E1–E6 desde los CSV |
| `docs` | Protocolo de toma de datos y figuras del informe |

## Reproducir

1. **STM32** (una por subsistema): compilar con `cmake --preset <preset>` y `cmake --build`, y cargar por SWD con `STM32_Programmer_CLI -c port=SWD -w <archivo>.elf -v -rst`. Energizar o reiniciar las cuatro placas **a la vez** para que sus relojes de 360 s queden en fase.
2. **Gateway**: copiar `firmware/gateway_esp32/gateway_v7/secrets.example.h` a `secrets.h`, poner la red Wi-Fi y la IP del broker, y cargar el sketch desde Arduino IDE.
3. **PC**: iniciar Mosquitto y luego `python scada_pc/logger_raw.py` (guarda todos los topics en un CSV).

## Experimentos y datos

Todos los CSV están en `datos/` y salen del registrador (`scada_pc/logger_raw.py`): una fila por mensaje MQTT, con columnas `t_pc_iso, t_rel_s, topic, payload` (el payload es JSON). Las cifras del informe se reproducen con `python analisis/metricas_experimentos.py` (resultado en `docs/metricas_experimentos.txt`).

| Exp. | Qué valida | Archivo | Fecha de toma |
|---|---|---|---|
| P0 | PV sin atasco al amanecer + deslastre escalonado (≈3.3 días simulados) | `P0_demo.csv` | 18/11/2026 |
| E1 | Operación normal con carga alternante residencial/industrial | `E1.csv` | 19/11/2026 |
| E2 | Excedente con SCADA (tres episodios) | `E2.csv` | 19/11/2026 |
| E3 | Excedente sin SCADA (A) frente a con SCADA (B) | `E3_A.csv`, `E3_B.csv` | 18/11/2026 |
| E4 | Carga industrial sostenida y deslastre escalonado | `E4.csv` | 19/11/2026 |
| E5 | Arranque en negro, de día y de noche | `E5.csv` | 19/11/2026 |
| E6 | Falla de un generador (PV desenergizado) | `E6.csv` | 18/11/2026 |

Todos se tomaron con el firmware v7 del gateway y el PV corregido. Otras carpetas de datos:

- `datos/s13/`: E1 y E2 de la entrega anterior (S13, firmware viejo), solo como línea base.
- `datos/intentos_previos/`: una corrida de E4 del 18/11 en la que el SOC nunca bajó de 45 % y el deslastre no se activó; se repitió el 19/11 (`E4.csv`).

Los criterios de aceptación y el procedimiento de cada experimento están en `docs/protocolo_toma_datos_S15.md`.

## Estado del firmware

| Carpeta | Contenido actual |
|---|---|
| `firmware/gateway_esp32/gateway_v7` | Sketch completo del gateway (SCADA y MQTT). |
| `firmware/stm32_pv` | `app_pv_M.c`, `pv_control.c` (con la corrección del P&O), `pv_control.h`, `conv_link.h` y `CMakeLists.txt`. Falta el resto del proyecto CubeMX (`main.c` generado, `.ioc`, HAL). |
| `firmware/stm32_eolica`, `stm32_bateria`, `stm32_carga` | Pendiente: solo hay un `LEEME.txt`. |

## Autores

Sofía Parra Rozo y Tomás Durán Cardoso. Departamento de Ingeniería Eléctrica y Electrónica, Universidad de los Andes.
