# Normativas de referencia: alarmas y HMI

El SCADA y el tablero de este proyecto se diseñaron y se evalúan con dos normas de ISA como referencia. Aquí se explica qué se tomó de cada una, cómo se aplicó y qué falta. Las normas son de pago y no se incluyen en el repositorio.

| Norma | Tema | Cómo se citó |
|---|---|---|
| ANSI/ISA-18.2-2009 | Gestión de sistemas de alarmas en industrias de proceso | Se leyó la norma completa |
| ANSI/ISA-101.01-2015 | Interfaces humano-máquina (HMI) en sistemas de automatización | **No se pudo leer el texto de la norma.** Se usaron fuentes secundarias y un trabajo de grado que la aplica |

Por eso este proyecto habla de estar **alineado con** estas normas y no de **cumplirlas**: ninguna se certificó ni se auditó. Las dos están pensadas para plantas de proceso y aquí se aplican por analogía a una microrred emulada.

## ISA-18.2: alarmas

### Qué es una alarma aquí

La norma pide que cada alarma sea *accionable*: debe tener una consecuencia clara, una acción del operador, una prioridad y un tiempo de respuesta permitido. Con ese criterio, los estados normales del SCADA (DEFICIT, EXCEDENTE) **no son alarmas**: son modos de operación esperados y el SCADA ya actúa solo. Se muestran como estado.

La capa de alarmas vive en el PC y no toca el firmware del SCADA:

- `scada_pc/alarmas.py`: definición de las 11 alarmas y el motor que las evalúa (retardo de activación, banda muerta, retardo de retorno y supresión de alarmas redundantes).
- `analisis/kpi_alarmas.py`: reproduce los CSV de `datos/` por ese motor y calcula los indicadores.
- Tabla completa de alarmas: [`alarmas_tabla.md`](alarmas_tabla.md). Eventos generados: [`alarmas_eventos.csv`](alarmas_eventos.csv). Indicadores: [`resultados_alarmas.txt`](resultados_alarmas.txt).

### Qué se aplicó de la norma

| Práctica de ISA-18.2 | Aplicación |
|---|---|
| Solo alarmas accionables | 11 alarmas. DEFICIT y EXCEDENTE quedan como estado |
| Prioridad por consecuencia y tiempo de respuesta | Tres niveles (Alta, Media, Baja). Cada alarma tiene consecuencia, acción y tiempo de respuesta en la tabla |
| Retardo de activación (on-delay) | 2 a 10 s según la alarma, para no avisar en transitorios |
| Banda muerta y retardo de retorno | Cada alarma vuelve a normal en un umbral distinto del de activación, por ejemplo se activa en 50.4 V y vuelve en 50.0 V |
| Supresión de alarmas redundantes | `BUS_ALTO` se suprime mientras haya `BUS_MUY_ALTO` o `RECORTE_AGOTADO` |
| Justificar cada umbral | Cada umbral sale de la banda de ±5 %, de los límites de aceptación de la Tabla III del informe o de los niveles de deslastre del SCADA. La columna de condición de la tabla lo explica |

### Resultados sobre los experimentos

Indicadores de la norma: hasta 1 alarma por 10 min es aceptable, hasta 2 es manejable, y más de 10 en 10 min es inundación.

| Exp. | Alarmas | Por 10 min | Máx. en 10 min | Sin racionalizar, por 10 min |
|---|---|---|---|---|
| P0 | 3 | 1.5 | 2 | 14.1 |
| E1 | 1 | 0.6 | 1 | 19.5 |
| E2 | 15 | 6.3 | 10 | 83.2 |
| E3 B (con SCADA) | 9 | 5.0 | 9 | 70.5 |
| E4 | 3 | 1.7 | 2 | 19.4 |
| E5 | 2 | 2.3 | 2 | 14.9 |
| E6 | 1 | 3.7 | 1 | 11.1 |

- En ningún experimento hay inundación ni chattering.
- Los experimentos con carga normal (P0, E1, E4) quedan dentro de la meta de la norma. E2, E3 B, E5 y E6 la superan: son los de excedente, arranque en negro y falla, que son perturbaciones provocadas a propósito.
- **Cuidado al interpretar estas cifras.** Los experimentos están acelerados: un día dura 360 s, así que 10 min de experimento equivalen a unas 40 h simuladas. Los indicadores son una prueba de esfuerzo de la lógica de alarmas y no la carga real de un operador.
- La columna "sin racionalizar" cuenta cada cambio de estado y cada evento que publica el SCADA. Muestra cuánto ruido se evita al avisar solo lo accionable.
- Con 11 alarmas, la distribución de prioridades de la norma (≈80 % bajas, 15 % medias, 5 % altas) no es significativa. Hay 5 Altas, 4 Medias y 2 Bajas.

### Lo que falta

- La capa de alarmas se evaluó **reproduciendo CSV**. Todavía no está integrada al tablero en vivo.
- No hay reconocimiento (*acknowledge*) por parte del operador ni lista de alarmas activas en pantalla.
- No se midió el tiempo de respuesta real de un operador.
- La alarma de eólica, carga y batería fuera de línea se definió pero solo se vio activarse la de PV (E6), porque fue la única falla que se provocó.

## ISA-101: HMI

El tablero (`scada_pc/scada_dashboard.py`) se revisó contra los principios de ISA-101 que aparecen en las fuentes secundarias. Esta es una revisión de lectura del código, no una prueba con operadores.

| Principio | Estado | Cómo está en el tablero |
|---|---|---|
| Jerarquía de pantallas con pocos niveles (la norma sugiere no más de 4) | Sí | Cuatro pestañas: Mapa de energía (visión general), Gráficas y cifras (tendencias), Las 4 partes (detalle por subsistema) y Control |
| Estilos de pantalla distintos para visión general, tendencia y detalle | Sí | Cada pestaña tiene una función |
| Umbrales visibles en las tendencias | Sí | Banda de ±5 % y línea de 53 V en la gráfica de Vbus. Líneas de 20 % y 30 % en la del SOC |
| Color con significado de estado | Parcial | El color de Vbus y de los estados sigue verde, ámbar y rojo, pero los cuatro subsistemas tienen además un color propio de identidad (azul, amarillo, verde, rosa). La norma recomienda reservar el color para estado y alarma |
| No depender solo del color | Parcial | Los estados llevan texto, pero la prioridad de una alarma tendría que mostrarse también con texto o forma |
| Acciones de control protegidas | Sí | Los botones no envían nada hasta marcar "Permitir que los botones actúen", y el tablero avisa en qué modo está |
| Lista de alarmas con prioridad y reconocimiento | **Pendiente** | Lo cubre la capa de `alarmas.py` cuando se integre |

La norma no fija colores ni formas concretas. Lo anterior es una lectura de sus principios generales y no una verificación contra el texto.

## Fuentes

- ANSI/ISA-18.2-2009, *Management of Alarm Systems for the Process Industries*.
- ANSI/ISA-101.01-2015, *Human Machine Interfaces for Process Automation Systems*.
