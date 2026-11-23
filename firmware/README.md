# Firmware

| Carpeta | Placa | Contenido |
|---|---|---|
| `gateway_esp32/gateway_v7` | ESP32-S3 | Maestro I2C, SCADA (S0–S9) y publicación MQTT. Sketch de Arduino. Copiar `secrets.example.h` a `secrets.h`. |
| `gateway_esp32/gateway_v7_diag` | ESP32-S3 | Igual que v7 más medición de latencia I2C y eco MQTT en `microred_ctrl/diag`. Misma lógica del SCADA. |
| `stm32_pv` | STM32F303K8 | Modelo PV, convertidor y MPPT P&O corregido. Direcciones I2C 0x10 / 0x20. |
| `stm32_eolica` | STM32F303K8 | Pendiente. Direcciones 0x11 / 0x21. |
| `stm32_bateria` | STM32F303K8 | Pendiente. Direcciones 0x12 / 0x22. Forma el bus de 48 V. |
| `stm32_carga` | STM32F303K8 | Pendiente. Direcciones 0x13 / 0x23. |

Las cuatro placas comparten la trama de convertidor definida en `stm32_pv/Core/Inc/conv_link.h`. Cada una integra planta, convertidor y control local en la misma interrupción de 10 kHz.

No se suben `build/`, `secrets.h` ni binarios (ver `.gitignore`).
