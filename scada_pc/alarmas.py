"""Capa de alarmas del PC (alineada con ANSI/ISA-18.2-2009).

El SCADA del ESP32 decide y actua; esta capa solo avisa al operador. Toma las muestras de
`microred_ctrl/estado` (1 Hz) y los avisos `microred_ctrl/<subsistema>_online`, y aplica a cada
alarma lo que pide la norma: prioridad por consecuencia, retardo de activacion (on-delay),
banda muerta y retardo de retorno (off-delay), supresion de alarmas redundantes y un tiempo
de respuesta permitido. Los estados normales del SCADA (DEFICIT, EXCEDENTE) NO son alarmas:
son modos de operacion esperados y se muestran como estado, no como aviso.

Uso como modulo:
    eng = MotorAlarmas()
    eventos = eng.actualizar(t, muestra, online)   # muestra = dict de microred_ctrl/estado
Reproduccion sobre un CSV del registrador: analisis/kpi_alarmas.py
"""
from dataclasses import dataclass, field
from typing import Callable, Dict, List, Tuple

ALTA, MEDIA, BAJA = 1, 2, 3
NOMBRE_PRIORIDAD = {ALTA: "Alta", MEDIA: "Media", BAJA: "Baja"}

# Pisos del recorte proporcional del SCADA (generador con tope activo en su piso)
PISO_PV_W, PISO_EOL_W = 15.0, 13.4


@dataclass
class Alarma:
    tag: str
    descripcion: str
    prioridad: int
    activa: Callable[[dict, dict], bool]     # condicion de activacion
    normal: Callable[[dict, dict], bool]     # condicion de retorno (con banda muerta)
    on_s: float                              # la condicion debe sostenerse este tiempo para activar
    off_s: float                             # y la de retorno este tiempo para volver a normal
    consecuencia: str
    accion: str
    respuesta_s: int                         # tiempo de respuesta permitido
    suprimida_por: Tuple[str, ...] = ()      # no se anuncia si alguna de estas esta activa
    umbral: str = ""                         # texto del umbral y su justificacion


def _vb(s): return s.get("vbus", 48.0)
def _soc(s): return s.get("soc", 100.0)


def _recorte_agotado(s, o):
    cp, ce = s.get("cap_pv", -1.0), s.get("cap_eol", -1.0)
    return _vb(s) > 50.4 and 0 <= cp <= PISO_PV_W + 0.5 and 0 <= ce <= PISO_EOL_W + 0.5


ALARMAS: List[Alarma] = [
    Alarma("BUS_MUY_ALTO", "Tension de bus por encima de 53 V", ALTA,
           lambda s, o: _vb(s) > 53.0, lambda s, o: _vb(s) < 52.0, 2, 2,
           "Sobretension en convertidores y cargas",
           "Reducir generacion o conectar una carga de disipacion", 10,
           umbral="53 V: limite de aceptacion de E2/E3 (+10.4 % sobre 48 V); el SCADA acelera el recorte (2 s) desde aqui."),
    Alarma("BUS_ALTO", "Tension de bus fuera de banda por arriba (> 50.4 V)", BAJA,
           lambda s, o: _vb(s) > 50.4, lambda s, o: _vb(s) < 50.0, 2, 5,
           "El SCADA ya recorta generacion; sin accion, puede subir a BUS_MUY_ALTO",
           "Confirmar que el recorte esta actuando", 600, ("BUS_MUY_ALTO", "RECORTE_AGOTADO"),
           umbral="50.4 V = 48 V + 5 %, borde de la banda de operacion; igual al umbral de entrada a EXCEDENTE (2 muestras)."),
    Alarma("BUS_BAJO", "Tension de bus por debajo de 45.6 V", ALTA,
           lambda s, o: _vb(s) < 45.6, lambda s, o: _vb(s) > 46.1, 2, 5,
           "Caida de tension en las cargas, riesgo de apagon",
           "Verificar la bateria y deslastrar carga", 10,
           umbral="45.6 V = 48 V - 5 %, borde inferior de la banda."),
    Alarma("SOC_BAJO", "Bateria por debajo de 30 %: deslastre activo", BAJA,
           lambda s, o: _soc(s) < 30.0, lambda s, o: _soc(s) > 35.0, 5, 10,
           "El SCADA ya reduce la carga; la bateria se sigue descargando",
           "Revisar el balance generacion-carga", 300, ("SOC_CRITICO",),
           umbral="30 % = primer nivel de deslastre del SCADA (60 % de la carga); retorno a 35 % por histeresis."),
    Alarma("SOC_CRITICO", "Bateria por debajo de 20 %", ALTA,
           lambda s, o: _soc(s) < 20.0, lambda s, o: _soc(s) > 25.0, 5, 10,
           "Agotamiento de la bateria y perdida de la regulacion del bus",
           "Desconectar cargas no esenciales y recuperar generacion", 60,
           umbral="20 % = limite inferior de aceptacion de E1/E4 y tercer nivel de deslastre."),
    Alarma("RECORTE_AGOTADO", "Excedente con recorte en su piso", MEDIA,
           _recorte_agotado, lambda s, o: _vb(s) < 50.0, 10, 5,
           "El SCADA no puede recortar mas; el bus sigue subiendo",
           "Reducir generacion manualmente o conectar carga", 60,
           umbral="Bus > 50.4 V con PV en 15 W y eolica en 13.4 W (pisos del recorte), sostenido 10 s para no avisar en transitorios."),
    Alarma("MODO_SEGURO", "SCADA en SEGURO: bateria sin respuesta", ALTA,
           lambda s, o: s.get("estado") == "SEGURO", lambda s, o: s.get("estado") != "SEGURO", 2, 5,
           "Los topes se liberan y nadie regula el bus",
           "Revisar la placa de la bateria y el bus I2C", 30,
           umbral="Estado S8 del SCADA."),
    Alarma("BATERIA_OFFLINE", "Placa de la bateria sin respuesta", ALTA,
           lambda s, o: o.get("bateria", 1) == 0, lambda s, o: o.get("bateria", 1) == 1, 0, 5,
           "Nadie forma el bus de 48 V", "Reiniciar la placa y revisar GND, pull-ups y alimentacion", 10,
           umbral="Aviso `bateria_online` = 0 del gateway (sin respuesta I2C)."),
    Alarma("PV_OFFLINE", "Placa del PV sin respuesta", MEDIA,
           lambda s, o: o.get("pv", 1) == 0, lambda s, o: o.get("pv", 1) == 1, 0, 5,
           "Se pierde la generacion solar", "Reiniciar la placa y revisar el bus I2C", 120,
           umbral="Aviso `pv_online` = 0 (E6: llega unos 3 s despues de la ultima lectura)."),
    Alarma("EOLICA_OFFLINE", "Placa de la eolica sin respuesta", MEDIA,
           lambda s, o: o.get("eolica", 1) == 0, lambda s, o: o.get("eolica", 1) == 1, 0, 5,
           "Se pierde la generacion eolica", "Reiniciar la placa y revisar el bus I2C", 120,
           umbral="Aviso `eolica_online` = 0."),
    Alarma("CARGA_OFFLINE", "Placa de la carga sin respuesta", MEDIA,
           lambda s, o: o.get("carga", 1) == 0, lambda s, o: o.get("carga", 1) == 1, 0, 5,
           "Se pierde el control del deslastre", "Reiniciar la placa y revisar el bus I2C", 120,
           umbral="Aviso `carga_online` = 0."),
]
POR_TAG = {a.tag: a for a in ALARMAS}


@dataclass
class _Est:
    activa: bool = False
    desde: float = None          # inicio de la condicion sostenida
    normal_desde: float = None   # inicio de la condicion de retorno
    t_act: float = None


class MotorAlarmas:
    """Procesa muestras en orden temporal y devuelve eventos (t, tag, 'ACT'|'RTN', prioridad)."""

    def __init__(self, alarmas: List[Alarma] = None):
        self.alarmas = alarmas or ALARMAS
        self.est: Dict[str, _Est] = {a.tag: _Est() for a in self.alarmas}

    def activas(self) -> List[str]:
        return [t for t, e in self.est.items() if e.activa]

    def actualizar(self, t: float, s: dict, online: dict) -> List[tuple]:
        ev = []
        for a in self.alarmas:                       # primero las que pueden suprimir a otras
            e = self.est[a.tag]
            if not e.activa:
                if a.activa(s, online):
                    if e.desde is None:
                        e.desde = t
                    if t - e.desde >= a.on_s:
                        e.activa, e.t_act, e.normal_desde = True, t, None
                        ev.append((t, a.tag, "ACT", a.prioridad))
                else:
                    e.desde = None
            else:
                if a.normal(s, online):
                    if e.normal_desde is None:
                        e.normal_desde = t
                    if t - e.normal_desde >= a.off_s:
                        e.activa, e.desde, e.normal_desde = False, None, None
                        ev.append((t, a.tag, "RTN", a.prioridad))
                else:
                    e.normal_desde = None
        # supresion: una alarma suprimida se anuncia solo si ninguna de sus supresoras esta activa
        out = []
        for x in ev:
            a = POR_TAG[x[1]]
            if x[2] == "ACT" and any(self.est[p].activa for p in a.suprimida_por):
                self.est[a.tag].activa = False       # se queda "en espera": la condicion sigue y se reevalua
                self.est[a.tag].desde = t - a.on_s
                continue
            out.append(x)
        return out


def tabla_md() -> str:
    """Tabla de racionalizacion (atributos que pide ISA-18.2 para cada alarma)."""
    f = ["| Tag | Prioridad | Condicion y umbral | On / off (s) | Consecuencia | Accion del operador | Respuesta |",
         "|---|---|---|---|---|---|---|"]
    for a in ALARMAS:
        resp = f"{a.respuesta_s} s" if a.respuesta_s < 120 else f"{a.respuesta_s // 60} min"
        f.append(f"| `{a.tag}` | {NOMBRE_PRIORIDAD[a.prioridad]} | {a.descripcion}. {a.umbral} | {a.on_s:g} / {a.off_s:g} | "
                 f"{a.consecuencia} | {a.accion} | {resp} |")
    return "\n".join(f)
