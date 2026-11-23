# Documentación

| Carpeta / archivo | Contenido |
|---|---|
| `informe/main.tex` | Informe en formato IEEE (máximo 6 páginas). |
| `informe/anexos_tecnicos.tex` (y su PDF) | Anexos B–H: modelos y curvas, convertidor, bus I2C, lazos de control, reglas del SCADA, detalle de S13 y comparación con las referencias. |
| `protocolos/protocolo_toma_datos_S15.md` | Procedimiento y criterios de aceptación de cada experimento. |
| `protocolos/instructivo_retomas_S15.md` | Instructivo para repetir E4 (largo) y E3 (en fase) y medir latencias. |
| `figuras/` | Figuras del informe y los scripts que las generan (`fig_*.py`, se corren desde esa carpeta). |
| `resultados_metricas.txt` | Salida de `analisis/metricas_experimentos.py`. |

Para compilar el informe se usa la plantilla IEEEtran (por ejemplo en Overleaf), con las figuras de `figuras/` y las de los experimentos en la raíz del proyecto.
