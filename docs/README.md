# Documentación

| Carpeta / archivo | Contenido |
|---|---|
| `informe/main.tex` | Informe en formato IEEE y su PDF (`Informe_Final_Microrred.pdf`, 7 páginas). |
| `informe/anexos_tecnicos.tex` (PDF: `Anexos_Tecnicos.pdf`) | Anexos B–H: modelos y curvas, convertidor, bus I2C, lazos de control, reglas del SCADA, detalle de los experimentos con el gateway v6 y comparación con las referencias. |
| `protocolos/protocolo_experimentos.md` | Procedimiento y criterios de aceptación de cada experimento. |
| `protocolos/repeticion_E3_E4.md` | Cómo repetir E4 (largo) y E3 (en fase) y medir latencias I2C y MQTT. |
| `normativas.md` | Alineación con ISA-18.2 (alarmas) e ISA-101 (HMI): qué se aplicó, resultados y lo que falta. |
| `alarmas_tabla.md`, `alarmas_eventos.csv`, `resultados_alarmas.txt` | Tabla de alarmas racionalizadas, eventos generados sobre los experimentos e indicadores. Los produce `analisis/kpi_alarmas.py`. |
| `figuras/` | Figuras del informe y los scripts que las generan (`fig_*.py`, se corren desde esa carpeta). |
| `resultados_metricas.txt` | Salida de `analisis/metricas_experimentos.py`. |

Para compilar el informe se usa la plantilla IEEEtran (por ejemplo en Overleaf), con las figuras de `figuras/` y las de los experimentos en la raíz del proyecto.
