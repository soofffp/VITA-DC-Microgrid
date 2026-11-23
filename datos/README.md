# Datos

Salida cruda del registrador (`scada_pc/logger_raw.py`): **una fila por mensaje MQTT**.

| Columna | Contenido |
|---|---|
| `t_pc_iso` | Hora del PC al recibir el mensaje (ISO 8601, ms) |
| `t_rel_s` | Segundos desde el primer mensaje de la corrida |
| `topic` | Tópico MQTT |
| `payload` | Mensaje en JSON (o un número, en `*_online`) |

## Tópicos principales

| Tópico | Contenido |
|---|---|
| `microred/<s>` | V, I, P y `status` de cada subsistema (`pv`, `eolica`, `bateria`, `carga`) |
| `microred/<s>_conv`, `_extra` | Convertidor (`vin`, `vbus`, `ibus`, `d`, banderas) y variables propias de cada fuente |
| `microred_ctrl/<s>`, `_reglas` | Referencias y regla de control local activa |
| `microred_ctrl/estado` | Estado del SCADA cada 1 s: `estado`, `regla`, `vbus`, `soc`, `pgen`, `pcarga`, `pbat`, `recorte_w`, `shed`, `auto` |
| `microred_ctrl/sistema` | Eventos del SCADA (S1–S9, comandos, cambios de escenario) |
| `microred_ctrl/<s>_online` | 1 si el gateway ve la placa, 0 si no |
| `microred_ctrl/diag` | Latencia I2C y MQTT (solo con `gateway_v7_diag`) |

## Experimentos

Todos con el firmware v7 del gateway y el PV con el MPPT corregido.

| Exp. | Qué valida | Archivo | Duración | Fecha |
|---|---|---|---|---|
| P0 | PV sin atasco al amanecer + deslastre escalonado | `P0_demo.csv` | 1194 s | 18/11/2026 |
| E1 | Operación normal con carga alternante | `E1.csv` | 1047 s | 19/11/2026 |
| E2 | Excedente con SCADA (tres episodios) | `E2.csv` | 1428 s | 19/11/2026 |
| E3 | Sin SCADA (A) frente a con SCADA (B) | `E3_A.csv`, `E3_B.csv` | 1009 s, 1080 s | 18/11/2026 |
| E4 | Carga industrial sostenida y deslastre | `E4.csv` | 1083 s | 19/11/2026 |
| E5 | Arranque en negro, de día y de noche | `E5.csv` | 525 s | 19/11/2026 |
| E6 | Falla de un generador (PV desenergizado) | `E6.csv` | 162 s | 18/11/2026 |

Otras carpetas:

- `s13/`: E1 y E2 de la entrega anterior (firmware viejo). Solo como línea base.
- `intentos_previos/`: corrida de E4 del 18/11 descartada (la carga se limitó a 40 % por error y el SOC no bajó de 45 %).

## Retomas pendientes

E4 largo (cerrar el tercer ciclo de deslastre) y E3 en fase (A y B con arranque simultáneo). Procedimiento en `docs/protocolos/instructivo_retomas_S15.md`; el verificador de fase es `analisis/check_fase_E3.py`.
