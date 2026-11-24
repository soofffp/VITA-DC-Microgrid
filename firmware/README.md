# Firmware

| Carpeta | Placa | Contenido |
|---|---|---|
| `gateway_esp32/gateway_v7` | ESP32-S3 | Maestro I2C, SCADA (S0–S9) y publicación MQTT. Sketch de Arduino. Copiar `secrets.example.h` a `secrets.h`. |
| `gateway_esp32/gateway_v7_diag` | ESP32-S3 | Igual que v7 más medición de latencia I2C y eco MQTT en `microred_ctrl/diag`. Misma lógica del SCADA. |
| `stm32_pv` | STM32F303K8 | Modelo PV (`pv_plant.c`), convertidor y MPPT P&O corregido (`pv_control.c`). Direcciones I2C 0x10 / 0x20. |
| `stm32_eolica` | STM32F303K8 | Turbina, rectificador y control (`wind_model.c`, `app_wind.c`). Direcciones 0x11 / 0x21. |
| `stm32_bateria` | STM32F303K8 | Pack 10s5p y convertidor bidireccional (`bat_model.c`, `app_bat.c`). Direcciones 0x12 / 0x22. Forma el bus de 48 V. |
| `stm32_carga` | STM32F303K8 | Carga DC con perfiles dinámicos de inversor (`load_model.c`, `app_load.c`). Direcciones 0x13 / 0x23. |

Cada placa integra planta, convertidor promediado y control local en la misma interrupción de 10 kHz.

## Compilar una placa

Cada carpeta `stm32_*` es un proyecto de STM32CubeMX con CMake. El código propio está en `Core/Src` y `Core/Inc` (`app_*.c`, el modelo de la planta y `sub_i2c.c`). El resto lo genera CubeMX.

1. Abrir el `.ioc` de la carpeta con STM32CubeMX y generar el código. Esto descarga la HAL (`Drivers/`), que no se incluye en el repositorio por tamaño.
2. Compilar con `cmake --preset Debug` y `cmake --build --preset Debug`.
3. Cargar por SWD (ver el README raíz).

Los cuatro archivos `.ioc` se llaman `microred_pv_M.ioc` y el proyecto CMake también (`CMAKE_PROJECT_NAME`). Es una herencia de haber partido de la misma plantilla, y no afecta a la compilación.

## Interfaz entre placas

- **PV** usa su propio enlace (`pv_link.h`, direcciones 0x10 y 0x20).
- **Eólica, batería y carga** comparten `sub_link.h` y `sub_i2c.c`: la dirección A (0x11 / 0x12 / 0x13) entrega 30 bytes de telemetría y la dirección B (0x21 / 0x22 / 0x23) recibe comandos y entrega 10 bytes del control.
- La trama de convertidor es `conv_link.h`. La de las tres placas nuevas amplía la de PV: `ibus_mA` pasa a tener signo (positivo entrega, negativo absorbe) y se añaden las banderas `CVL_F_IDLE` y `CVL_F_PROT`.
- Los comentarios de cabecera de cada `app_*.c` dicen "no probado en hardware". Se refieren a la etapa de potencia real (ADC y PWM físicos), que sigue siendo trabajo futuro. Los experimentos del informe se hicieron con estas cuatro placas.

No se suben `build/`, `Drivers/`, `secrets.h` ni binarios (ver `.gitignore`).
