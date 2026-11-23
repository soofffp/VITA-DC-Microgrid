/*
 * Gateway I2C <-> MQTT de la MICRORED (PV + EOLICA + BATERIA + CARGA) — ESP32-S3   (v7)
 *
 * CAMBIOS v7
 *  - EXCEDENTE: el escalon de recorte (S1/S2) es PROPORCIONAL al exceso de tension:
 *      paso = R_STEP_W + R_STEP_GAIN_WV * (Vbus - 50.4), acotado a R_STEP_MAX_W. Antes era fijo (5 W).
 *      Cadencia: DWELL_MS (6 s) o cada OVER_CYCLES (2 s) si Vbus > VBUS_FAST_V (53 V).
 *  - DESLASTRE ESCALONADO por SOC: 100 -> 60 -> 40 -> 25 % (umbrales 30/25/20 %), restauracion de a un nivel
 *      con 10 puntos de histeresis (40/35/30 %) y tiempo minimo SHED_DWELL_MS entre cambios.
 *  - Estado publicado: campo "shed" (nivel de deslastre 0..3).
 *
 * CAMBIOS v6
 *  - SCADA como MAQUINA DE ESTADOS (ARRANQUE / NORMAL / EXCEDENTE / DEFICIT / SEGURO), sin lazo continuo ni filtros.
 *  - EXCEDENTE: topes Pmax ABSOLUTOS por generador, ajustados por ESCALONES (R_STEP_W) con tiempo minimo (DWELL_MS).
 *      escalon arriba (recorta mas): Vbus > 50.4 V        escalon abajo (suelta): bateria descargando > DEF_BAT_W x DEF_CYCLES
 *      se recorta primero la PV (hasta su piso); la eolica solo cuando la PV ya esta en su piso. Sin tope = MPPT.
 *  - Bus de referencia = bateria_conv.vbus (la unica STM que mide el bus). Banda 48 V +-5 % (45.6 .. 50.4).
 *  - RELOJ DE DIA: 1 dia = 6 min = 360 s (DAY_MS).
 *      Las STM de PV y eolica ya generan su propio perfil diario (360 s); el gateway solo lleva un reloj
 *      para etiquetar la hora. Alinealo con el comando  hora <H>  (p.ej. 'hora 6' cuando sale el sol).
 *  - CARGA automatica: alterna RESIDENCIAL / INDUSTRIAL cada LOAD_SEQ_MS (2 min). Un comando manual la pausa.
 *  - Deslastre de carga si SOC < SOC_SHED_PCT (carga al 60 %), se restaura con SOC > SOC_RESTORE_PCT.
 *  - client-id MQTT unico (evita expulsiones por ID duplicado).
 *
 * Direcciones I2C: PV 0x10/0x20, EOLICA 0x11/0x21, BATERIA 0x12/0x22, CARGA 0x13/0x23.
 * Topics: microred/<s>, microred/<s>_conv, microred/<s>_extra, microred_ctrl/<s>, microred_ctrl/<s>_reglas,
 *         microred_ctrl/<s>_online, microred_ctrl/estado (NUEVO, estado del SCADA cada ciclo),
 *         microred_ctrl/sistema (eventos: blackstart, escenario).
 * Comandos MQTT en microred_cmd/<s> y microred_cmd/sistema:
 *   sistema: "blackstart" | "industrial" | "residencial" | "auto_on" | "auto_off" | {"hora":6}
 *   serial : <pv|wind|bat|load> <mppt|normal|limited|reduced|safe> [x] [y] | load res|ind | blackstart
 *            auto on|off | hora <H>
 */
#include <Wire.h>
#include <WiFi.h>
#include <PubSubClient.h>

// ---------------- Configuracion ----------------
#include "secrets.h"   // WIFI_SSID, WIFI_PASS, MQTT_HOST (no se sube a GitHub)
const int   MQTT_PORT = 1883;

#define PIN_SDA   4
#define PIN_SCL   5
#define I2C_HZ    100000
#define POLL_MS   1000

#define EN_PV     1
#define EN_WIND   1
#define EN_BAT    1
#define EN_LOAD   1

// ---- reloj de dia ----
#define DAY_MS          360000UL  // 1 dia = 6 minutos = 360 s  (1 h simulada = 15 s)
#define PV_MAX_CAP_W    100.0f    // un tope por encima de esto se suelta (MPPT); MPP PV ~88.6 W
#define WIND_MAX_CAP_W  100.0f    // idem eolica; Pmax ideal a 25.5 Hz = 88.6 W
#define PV_FLOOR_W      15.0f     // piso: por debajo la PV se va a circuito abierto y se queda pegada
#define WIND_FLOOR_W    13.4f     // piso fisico de la eolica en LIMITED (d_max = 0.92)
#define CAP_RESEND_W    1.0f      // reenviar tope solo si cambia >= 1 W
#define CAP_REFRESH_MS  5000UL

// ---- carga automatica ----
#define LOAD_SEQ_MS     120000UL  // cambia de escenario cada 2 min (= DAY_MS para un dia completo por escenario)

// ---- SCADA: banda y reglas ----
#define BAND_LO_V       45.6f
#define BAND_HI_V       50.4f
#define VBUS_FAST_V     53.0f     // sobretension fuerte: ignora el tiempo minimo
#define R_STEP_W        5.0f
#define R_MAX_W         120.0f
#define R_STEP_MAX_W    30.0f     // tope del escalon proporcional
#define R_STEP_GAIN_WV  10.0f     // W de recorte extra por cada V de Vbus sobre BAND_HI_V
#define DWELL_MS        6000UL    // tiempo minimo entre cambios de R
#define OVER_CYCLES     2
#define DEF_BAT_W       5.0f      // bateria descargando mas que esto = deficit
#define DEF_CYCLES      3
// Deslastre escalonado: nivel k (1..SHED_N) se activa si SOC < SHED_SOC_ON[k-1];
// se baja de nivel k a k-1 si SOC > SHED_SOC_OFF[k-1] (histeresis de 10 puntos).
#define SHED_N          3
const float SHED_LEVEL[SHED_N + 1]  = { 100.0f, 60.0f, 40.0f, 25.0f };   // % del perfil por nivel
const float SHED_SOC_ON[SHED_N]     = { 30.0f, 25.0f, 20.0f };
const float SHED_SOC_OFF[SHED_N]    = { 40.0f, 35.0f, 30.0f };
#define SHED_DWELL_MS   10000UL   // tiempo minimo entre cambios de nivel (SOC va 60x mas rapido que el reloj)

#define CTRL_READ_TRIES     3
#define OFFLINE_AFTER_FAILS 3
#define RESTART_AFTER_FAILS 6

// ---------------- Tramas ----------------
typedef struct __attribute__((packed)) { uint16_t voltage_mV, current_mA; uint32_t power_mW; uint8_t status, checksum; } pv_plant_t;
typedef struct __attribute__((packed)) { uint16_t vref_mV, duty_1e4, p_cW, pmax_dW; uint8_t mode, checksum; } pv_ctrl_t;

typedef struct __attribute__((packed)) { uint16_t v_mV; int16_t i_mA; int32_t p_mW; uint8_t status, checksum; } sub_plant_t;
typedef struct __attribute__((packed)) { uint16_t vin_mV, vbus_mV; int16_t ibus_mA; uint16_t duty_1e4; uint8_t flags, checksum; } conv_t;
typedef struct __attribute__((packed)) { int16_t x1, x2, x3, x4; uint8_t xflags, checksum; } extra_t;
typedef struct __attribute__((packed)) { uint16_t ref_mV, act_1e4; int16_t p_dW; uint16_t lim_dW; uint8_t mode, checksum; } sub_ctrl_t;
static_assert(sizeof(pv_plant_t) == 10 && sizeof(pv_ctrl_t) == 10, "tramas PV");
static_assert(sizeof(sub_plant_t) == 10 && sizeof(conv_t) == 10 && sizeof(extra_t) == 10 && sizeof(sub_ctrl_t) == 10, "tramas sub");

#define F_SAT_MAX 0x01
#define F_SAT_MIN 0x02
#define F_LIMITING 0x04
#define F_REF_LIMIT 0x08
#define F_OVERRUN 0x10
#define F_SAFE 0x20
#define F_IDLE 0x40
#define F_PROT 0x80

enum { S_PV = 0, S_WIND = 1, S_BAT = 2, S_LOAD = 3, N_SUB = 4 };
const char*   NAME[N_SUB]   = { "pv", "eolica", "bateria", "carga" };
const uint8_t ADDR_A[N_SUB] = { 0x10, 0x11, 0x12, 0x13 };
const uint8_t ADDR_B[N_SUB] = { 0x20, 0x21, 0x22, 0x23 };
const bool    ENABLED[N_SUB] = { EN_PV, EN_WIND, EN_BAT, EN_LOAD };

enum { ST_ARRANQUE = 0, ST_NORMAL, ST_EXCEDENTE, ST_DEFICIT, ST_SEGURO };
const char* ST_NAME[] = { "ARRANQUE", "NORMAL", "EXCEDENTE", "DEFICIT", "SEGURO" };

WiFiClient   espClient;
PubSubClient mqtt(espClient);
unsigned long tPoll = 0, tNet = 0;

// ---- estado medido ----
float    gP_W[N_SUB] = { 0, 0, 0, 0 };     // generacion +, carga consumo +, bateria + descarga
float    gVbus_V = 48.0f, gBatV = 42.0f, gSoc = 50.0f, gIchMax = 1.0f;
bool     gHaveBat = false;
uint8_t  gFlags[N_SUB]  = { 0, 0, 0, 0 };
uint8_t  gXFlags[N_SUB] = { 0, 0, 0, 0 };
int16_t  gX3[N_SUB] = { 0, 0, 0, 0 }, gX4[N_SUB] = { 0, 0, 0, 0 };
bool     gHaveFlags[N_SUB] = { false, false, false, false };
bool     online[N_SUB] = { true, true, true, true };
uint8_t  failCnt[N_SUB] = { 0, 0, 0, 0 };
int      gAllDown = 0;

// ---- estado del SCADA ----
int      gState = ST_ARRANQUE;
const char* activeRule();
float    gR = 0;                 // recorte total pedido a los generadores [W]
uint32_t gRt = 0;                // instante del ultimo cambio de R
int      gOverCnt = 0, gDefCnt = 0;
bool     gAuto = true;           // automatismo del SCADA (recorte + carga automatica)
bool     gManualGen = false;     // un comando manual a PV/eolica pausa el recorte hasta auto_on
bool     gForceSend = true;
float    gCapPv = -1, gCapW = -1;   // tope Pmax absoluto por generador [W]; -1 = sin tope (MPPT)
float    lastCapPv = -1, lastCapW = -1;
int      lastModePv = -1, lastModeW = -1;
uint32_t tLastPv = 0, tLastW = 0;
uint32_t gDay0 = 0;

// ---- carga ----
uint8_t  gLoadModeCmd = 0;
int      gLoadNivel = 100;
int      gShedLvl = 0;           // 0 = sin deslastre; 1..SHED_N = nivel activo
uint32_t gShedT = 0;
bool     gLoadAuto = true;
int      gEsc = 0;
uint32_t gEscT = 0;
bool     gEscInit = false;

void setLoadScenario(int esc);
void blackstartStart();
bool gBS_active();

// ---------------- utilidades ----------------
uint8_t xorSum(const uint8_t* p, size_t n) { uint8_t x = 0; for (size_t k = 0; k < n; k++) x ^= p[k]; return x; }
bool chunkOk(const uint8_t* b) { return xorSum(b, 9) == b[9]; }
void pub(const char* topic, const char* payload) { if (mqtt.connected()) mqtt.publish(topic, payload); }
void topicName(char* out, size_t n, const char* prefix, int s, const char* suffix) { snprintf(out, n, "%s/%s%s", prefix, NAME[s], suffix); }

void event(const char* txt) {
  char m[160]; snprintf(m, sizeof(m), "{\"evento\":\"%s\"}", txt);
  pub("microred_ctrl/sistema", m);
  Serial.printf("EVENTO: %s\n", txt);
}

// ---------------- I2C ----------------
void i2cScan() {
  Serial.println("Escaneando I2C...");
  int found = 0;
  for (uint8_t a = 1; a < 127; a++) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0) { Serial.printf("  encontrado 0x%02X\n", a); found++; }
  }
  if (!found) Serial.println("  nada en el bus: revisar GND, pull-ups y SDA/SCL");
}

void i2cRestart() {
  Serial.println("I2C: reiniciando el bus");
  Wire.end(); delay(10);
  Wire.begin(PIN_SDA, PIN_SCL, I2C_HZ);
  Wire.setTimeOut(100);
}

bool i2cRead(uint8_t addr, uint8_t* buf, size_t n) {
  size_t got = Wire.requestFrom((int)addr, (int)n);
  if (got != n) {
    Wire.beginTransmission(addr);
    uint8_t err = Wire.endTransmission();
    Serial.printf("0x%02X: llegaron %u/%u bytes, error I2C=%u\n", addr, (unsigned)got, (unsigned)n, err);
    return false;
  }
  for (size_t k = 0; k < n; k++) buf[k] = Wire.read();
  return true;
}

// Lectura de 10 bytes con checksum; reintenta (la trama de control a veces llega corrupta)
bool i2cReadChk10(uint8_t addr, uint8_t* b) {
  for (int t = 0; t < CTRL_READ_TRIES; t++) {
    size_t got = Wire.requestFrom((int)addr, 10);
    if (got == 10) {
      for (int k = 0; k < 10; k++) b[k] = Wire.read();
      if (chunkOk(b)) return true;
    } else {
      while (Wire.available()) Wire.read();
    }
    delay(3);
  }
  return false;
}

bool sendCmd7(uint8_t addrB, uint8_t cmd, uint8_t mode, uint16_t a, uint16_t b) {
  uint8_t m[7] = { cmd, mode, (uint8_t)(a >> 8), (uint8_t)a, (uint8_t)(b >> 8), (uint8_t)b, 0 };
  m[6] = xorSum(m, 6);
  Wire.beginTransmission(addrB);
  Wire.write(m, sizeof(m));
  return Wire.endTransmission() == 0;
}

bool sendCmdPv(uint8_t mode, float pmaxW) {
  if (pmaxW < 0) pmaxW = 0;
  if (pmaxW > 6553.5f) pmaxW = 6553.5f;
  uint16_t dW = (uint16_t)(pmaxW * 10.0f + 0.5f);
  uint8_t b[5] = { 0x01, mode, (uint8_t)(dW >> 8), (uint8_t)(dW & 0xFF), 0 };
  b[4] = xorSum(b, 4);
  Wire.beginTransmission(ADDR_B[S_PV]);
  Wire.write(b, sizeof(b));
  return Wire.endTransmission() == 0;
}

bool sendWind(uint8_t mode, float pmaxW) {
  if (pmaxW < 0) pmaxW = 0;
  if (pmaxW > 6553.5f) pmaxW = 6553.5f;
  return sendCmd7(ADDR_B[S_WIND], 0x01, mode, (uint16_t)(pmaxW * 10.0f + 0.5f), 0);
}

// Carga: en modo REDUCIDO "a" es el nivel en 0.1 %; b = escenario (0xFFFF = no cambiar)
bool sendLoad(uint8_t mode, int nivel, uint16_t esc) {
  uint16_t a = (mode == 1) ? (uint16_t)(nivel * 10) : 0;
  return sendCmd7(ADDR_B[S_LOAD], 0x01, mode, a, esc);
}

// ---------------- disponibilidad por subsistema ----------------
void publishOnline(int s) {
  char t[48]; topicName(t, sizeof(t), "microred_ctrl", s, "_online");
  pub(t, online[s] ? "1" : "0");
  Serial.printf("** %s %s **\n", NAME[s], online[s] ? "EN LINEA" : "SIN RESPUESTA");
}
void markOk(int s) {
  failCnt[s] = 0;
  if (!online[s]) { online[s] = true; publishOnline(s); }
}
void markFail(int s) {
  if (failCnt[s] < 255) failCnt[s]++;
  if (failCnt[s] >= OFFLINE_AFTER_FAILS && online[s]) {
    online[s] = false; gP_W[s] = 0;
    if (s == S_BAT) gHaveBat = false;
    publishOnline(s);
  }
}

// ---------------- Reglas locales activas ----------------
void protArray(char* prot, size_t n, uint8_t fl, bool sat, bool refl) {
  prot[0] = 0;
  if (sat  && (fl & F_SAT_MAX))   strncat(prot, "\"duty_max\",", n - strlen(prot) - 1);
  if (sat  && (fl & F_SAT_MIN))   strncat(prot, "\"duty_min\",", n - strlen(prot) - 1);
  if (refl && (fl & F_REF_LIMIT)) strncat(prot, "\"vref_limite\",", n - strlen(prot) - 1);
  size_t k = strlen(prot); if (k) prot[k - 1] = 0;
}

void publishRule(int s, const char* id, const char* nom, const char* txt, uint8_t fl, bool sat, bool refl) {
  char prot[64], js[256], t[40];
  protArray(prot, sizeof(prot), fl, sat, refl);
  snprintf(js, sizeof(js), "{\"regla\":\"%s\",\"modo\":\"%s\",\"texto\":\"%s\",\"prot\":[%s]}", id, nom, txt, prot);
  topicName(t, sizeof(t), "microred_ctrl", s, "_reglas");
  pub(t, js);
  Serial.printf("RG %-7s %s %s: %s", NAME[s], id, nom, txt);
  if (prot[0]) Serial.printf("  [%s]", prot);
  Serial.println();
}

void ruleFor(int s, uint8_t mode, uint8_t fl, uint8_t xfl, int16_t x3, int16_t x4) {
  switch (s) {
    case S_PV:
      if (fl & F_OVERRUN)                              publishRule(s, "R5", "FALLA",   "ISR de control fuera de tiempo", fl, true, true);
      else if (mode == 2)                              publishRule(s, "R4", "SAFE",    "V_ref congelada; solo regula tension", fl, true, true);
      else if (mode == 1 && (fl & F_LIMITING))         publishRule(s, "R3", "LIMITED", "Curtailment activo: P regulada a Pmax", fl, true, true);
      else if (mode == 1)                              publishRule(s, "R2", "LIMITED", "Tope Pmax aun no alcanzado: sigue MPPT", fl, true, true);
      else                                             publishRule(s, "R1", "MPPT",    "Seguimiento P&O del punto de maxima potencia", fl, true, true);
      break;
    case S_WIND:
      if (fl & F_OVERRUN)                              publishRule(s, "E7", "FALLA",   "ISR de control fuera de tiempo", fl, true, true);
      else if (fl & F_PROT)                            publishRule(s, "E6", "SOBREVIENTO", "Viento > cut-out: turbina detenida", fl, true, true);
      else if (fl & F_IDLE)                            publishRule(s, "E5", "SIN VIENTO", "Viento < cut-in: sin generacion", fl, true, true);
      else if (mode == 2)                              publishRule(s, "E4", "SAFE",    "V_ref congelada (ultimo valor valido)", fl, true, true);
      else if (mode == 1 && (fl & F_LIMITING))         publishRule(s, "E3", "LIMITED", "Curtailment activo: P regulada a Pmax", fl, true, true);
      else if (mode == 1)                              publishRule(s, "E2", "LIMITED", "Tope Pmax aun no alcanzado: sigue MPPT", fl, true, true);
      else                                             publishRule(s, "E1", "MPPT",    "Seguimiento P&O de la turbina", fl, true, true);
      break;
    case S_BAT:
      if (fl & F_OVERRUN)                              publishRule(s, "B7", "FALLA",   "ISR de control fuera de tiempo", fl, true, false);
      else if (xfl & 0x01)                             publishRule(s, "B6", "BUS FUERA DE BANDA", "V_bus fuera de 48 V +-5 %: alarma", fl, true, false);
      else if (xfl & 0x06)                             publishRule(s, "B5", "PROTECCION", "Limite por SOC/tension del pack activo", fl, true, false);
      else if (mode == 2)                              publishRule(s, "B4", "SAFE",    "Limites conservadores de corriente", fl, true, false);
      else if (mode == 1 && x4 < 70)                   publishRule(s, "B3", "DESCARGA LIMITADA", "Corriente de descarga reducida (deficit)", fl, true, false);
      else if (mode == 1 && x3 < 70)                   publishRule(s, "B2", "CARGA LIMITADA", "Corriente de carga reducida (excedente)", fl, true, false);
      else                                             publishRule(s, "B1", "NORMAL",  "La bateria regula el bus a 48 V", fl, true, false);
      break;
    case S_LOAD:
      if (fl & F_OVERRUN)                              publishRule(s, "C5", "FALLA",   "ISR de control fuera de tiempo", fl, false, false);
      else if (fl & F_PROT)                            publishRule(s, "C4", "SUBTENSION", "Carga abierta por bus bajo", fl, false, false);
      else if (mode == 2 || (fl & F_SAFE))             publishRule(s, "C3", "PERFIL MINIMO", "Modo seguro: demanda minima", fl, false, false);
      else if (mode == 1)                              publishRule(s, "C2", "REDUCIDA", "Nivel de carga recortado por el EMS", fl, false, false);
      else                                             publishRule(s, "C1", "NORMAL",  "La carga sigue su perfil", fl, false, false);
      break;
  }
}

// ---------------- Lectura y publicacion ----------------
void pollPv() {
  uint8_t buf[20];
  if (!i2cRead(ADDR_A[S_PV], buf, 20)) { markFail(S_PV); return; }
  if (!chunkOk(buf)) { Serial.println("PV: checksum planta MALO"); markFail(S_PV); return; }
  markOk(S_PV);
  pv_plant_t t; memcpy(&t, buf, sizeof(t));
  char js[160];
  snprintf(js, sizeof(js), "{\"v\":%.2f,\"i\":%.2f,\"p\":%.2f,\"status\":%u}", t.voltage_mV / 1000.0f, t.current_mA / 1000.0f, t.power_mW / 1000.0f, t.status);
  pub("microred/pv", js);
  Serial.printf("PV  V=%.2f I=%.2f P=%.2f st=%u\n", t.voltage_mV / 1000.0f, t.current_mA / 1000.0f, t.power_mW / 1000.0f, t.status);
  gP_W[S_PV] = t.power_mW / 1000.0f;
  if (chunkOk(buf + 10)) {
    conv_t c; memcpy(&c, buf + 10, sizeof(c));
    gFlags[S_PV] = c.flags; gHaveFlags[S_PV] = true;
    snprintf(js, sizeof(js), "{\"vin\":%.2f,\"vbus\":%.2f,\"ibus\":%.3f,\"d\":%.4f,\"flags\":%u}", c.vin_mV / 1000.0f, c.vbus_mV / 1000.0f, c.ibus_mA / 1000.0f, c.duty_1e4 / 10000.0f, c.flags);
    pub("microred/pv_conv", js);
  }
  delay(5);
  uint8_t b2[10];
  if (!i2cReadChk10(ADDR_B[S_PV], b2)) { Serial.println("PV: control no legible"); return; }
  pv_ctrl_t k; memcpy(&k, b2, sizeof(k));
  snprintf(js, sizeof(js), "{\"vref\":%.3f,\"d\":%.4f,\"p\":%.2f,\"pmax\":%.1f,\"mode\":%u}", k.vref_mV / 1000.0f, k.duty_1e4 / 10000.0f, k.p_cW / 100.0f, k.pmax_dW / 10.0f, k.mode);
  pub("microred_ctrl/pv", js);
  if (gHaveFlags[S_PV]) ruleFor(S_PV, k.mode, gFlags[S_PV], 0, 0, 0);
}

void pollSub(int s) {
  uint8_t buf[30];
  if (!i2cRead(ADDR_A[s], buf, 30)) { markFail(s); return; }
  char js[200], t[40];
  if (!chunkOk(buf)) { Serial.printf("%s: checksum planta MALO\n", NAME[s]); markFail(s); return; }
  markOk(s);
  sub_plant_t p; memcpy(&p, buf, sizeof(p));
  snprintf(js, sizeof(js), "{\"v\":%.2f,\"i\":%.3f,\"p\":%.2f,\"status\":%u}", p.v_mV / 1000.0f, p.i_mA / 1000.0f, p.p_mW / 1000.0f, p.status);
  topicName(t, sizeof(t), "microred", s, ""); pub(t, js);
  Serial.printf("%-7s V=%.2f I=%.3f P=%.2f st=%u\n", NAME[s], p.v_mV / 1000.0f, p.i_mA / 1000.0f, p.p_mW / 1000.0f, p.status);
  gP_W[s] = p.p_mW / 1000.0f;
  if (s == S_BAT) gBatV = p.v_mV / 1000.0f;

  if (chunkOk(buf + 10)) {
    conv_t c; memcpy(&c, buf + 10, sizeof(c));
    gFlags[s] = c.flags; gHaveFlags[s] = true;
    if (s == S_BAT) { gVbus_V = c.vbus_mV / 1000.0f; gHaveBat = true; }
    snprintf(js, sizeof(js), "{\"vin\":%.2f,\"vbus\":%.2f,\"ibus\":%.3f,\"d\":%.4f,\"flags\":%u}", c.vin_mV / 1000.0f, c.vbus_mV / 1000.0f, c.ibus_mA / 1000.0f, c.duty_1e4 / 10000.0f, c.flags);
    topicName(t, sizeof(t), "microred", s, "_conv"); pub(t, js);
    Serial.printf("   CV vin=%.2f vbus=%.2f ibus=%.3f d=%.4f flags=0x%02X\n", c.vin_mV / 1000.0f, c.vbus_mV / 1000.0f, c.ibus_mA / 1000.0f, c.duty_1e4 / 10000.0f, c.flags);
  } else Serial.printf("%s: checksum convertidor MALO\n", NAME[s]);

  if (chunkOk(buf + 20)) {
    extra_t x; memcpy(&x, buf + 20, sizeof(x));
    gXFlags[s] = x.xflags; gX3[s] = x.x3; gX4[s] = x.x4;
    if (s == S_BAT) { gSoc = x.x1 / 10.0f; gIchMax = x.x3 / 10.0f; }
    if (s == S_WIND)
      snprintf(js, sizeof(js), "{\"viento\":%.2f,\"f\":%.2f,\"rpm\":%d,\"vrect\":%.1f}", x.x1 / 100.0f, x.x2 / 100.0f, x.x3, x.x4 / 10.0f);
    else if (s == S_BAT)
      snprintf(js, sizeof(js), "{\"soc\":%.1f,\"temp\":%.1f,\"ich_max\":%.1f,\"idis_max\":%.1f,\"xflags\":%u}", x.x1 / 10.0f, x.x2 / 10.0f, x.x3 / 10.0f, x.x4 / 10.0f, x.xflags);
    else
      snprintf(js, sizeof(js), "{\"nivel\":%.1f,\"fp\":%.3f,\"fase\":%.1f,\"esc\":%d}", x.x1 / 10.0f, x.x2 / 1000.0f, x.x3 / 10.0f, x.x4);
    topicName(t, sizeof(t), "microred", s, "_extra"); pub(t, js);
    Serial.printf("   EX %s\n", js);
  } else Serial.printf("%s: checksum extra MALO\n", NAME[s]);

  delay(5);
  uint8_t b2[10];
  if (!i2cReadChk10(ADDR_B[s], b2)) { Serial.printf("%s: control no legible\n", NAME[s]); return; }
  sub_ctrl_t k; memcpy(&k, b2, sizeof(k));
  snprintf(js, sizeof(js), "{\"ref\":%.3f,\"act\":%.4f,\"p\":%.1f,\"lim\":%.1f,\"mode\":%u}", k.ref_mV / 1000.0f, k.act_1e4 / 10000.0f, k.p_dW / 10.0f, k.lim_dW / 10.0f, k.mode);
  topicName(t, sizeof(t), "microred_ctrl", s, ""); pub(t, js);
  if (gHaveFlags[s]) ruleFor(s, k.mode, gFlags[s], gXFlags[s], gX3[s], gX4[s]);
}

// Acoplamiento de la microred + heartbeat (una vez por ciclo)
void coupleMicrogrid() {
  if (ENABLED[S_BAT] && online[S_BAT]) {
    float pnet = gP_W[S_PV] * ENABLED[S_PV] + gP_W[S_WIND] * ENABLED[S_WIND] - gP_W[S_LOAD] * ENABLED[S_LOAD];
    if (pnet > 3270.0f) pnet = 3270.0f;
    if (pnet < -3270.0f) pnet = -3270.0f;
    sendCmd7(ADDR_B[S_BAT], 0x02, 0, (uint16_t)(int16_t)lroundf(pnet * 10.0f), 0);
  }
  if (ENABLED[S_LOAD] && online[S_LOAD]) {
    uint16_t v = (uint16_t)lroundf((gHaveBat ? gVbus_V : 48.0f) * 10.0f);
    sendCmd7(ADDR_B[S_LOAD], 0x02, 0, v, 0);
  }
  if (ENABLED[S_WIND] && online[S_WIND]) sendCmd7(ADDR_B[S_WIND], 0x03, 0, 0, 0);
}

// ---------------- Reloj de dia (solo etiqueta la hora; las STM generan su propio perfil) ----------------
float hourNow() { return (float)((millis() - gDay0) % DAY_MS) * 24.0f / (float)DAY_MS; }
void setHour(float h) {
  if (h < 0.0f) h = 0.0f;
  if (h >= 24.0f) h = 23.99f;
  gDay0 = millis() - (uint32_t)(h / 24.0f * (float)DAY_MS);
}

// ---------------- Topes de generacion: absolutos y por escalones ----------------
// Escalon de recorte proporcional al exceso de tension (5 W en el borde de la banda, hasta R_STEP_MAX_W)
float propStep() {
  float s = R_STEP_W + R_STEP_GAIN_WV * fmaxf(0.0f, gVbus_V - BAND_HI_V);
  return fminf(s, R_STEP_MAX_W);
}

float effGen(float cap, float p) { return (cap >= 0.0f) ? fminf(cap, p) : p; }
bool  anyCap() { return gCapPv >= 0.0f || gCapW >= 0.0f; }

// Baja la generacion w [W]: primero la PV (hasta su piso), el resto a la eolica. Devuelve lo realmente recortado.
float cutGen(float w) {
  float w0 = w;
  float basePv = effGen(gCapPv, gP_W[S_PV]);
  float c = fminf(w, fmaxf(0.0f, basePv - PV_FLOOR_W));
  if (c > 0.0f) { gCapPv = basePv - c; w -= c; }
  float baseW = effGen(gCapW, gP_W[S_WIND]);
  c = fminf(w, fmaxf(0.0f, baseW - WIND_FLOOR_W));
  if (c > 0.0f) { gCapW = baseW - c; w -= c; }
  return w0 - w;
}

// Sube los topes w [W]: primero la eolica (se limito de ultima), luego la PV. Un tope que ya no limita
// (P medida muy por debajo) o que llega al maximo se suelta (-1 = vuelve a MPPT).
void relaxGen(float w) {
  if (gCapW >= 0.0f) {
    if (gCapW > gP_W[S_WIND] + 10.0f) gCapW = -1.0f;
    else { gCapW += w; w = 0.0f; if (gCapW >= WIND_MAX_CAP_W) gCapW = -1.0f; }
  }
  if (gCapPv >= 0.0f) {
    if (gCapPv > gP_W[S_PV] + 10.0f) gCapPv = -1.0f;
    else if (w > 0.0f) { gCapPv += w; if (gCapPv >= PV_MAX_CAP_W) gCapPv = -1.0f; }
  }
}

void applyGenCaps() {
  if (!gAuto || gManualGen || gState == ST_ARRANQUE) return;
  uint32_t now = millis();
  if (ENABLED[S_PV] && online[S_PV]) {
    uint8_t mode = (gCapPv >= 0.0f) ? 1 : 0;
    float cap = mode ? gCapPv : 0.0f;
    if (gForceSend || (int)mode != lastModePv || fabsf(cap - lastCapPv) >= CAP_RESEND_W || (mode && now - tLastPv >= CAP_REFRESH_MS)) {
      if (sendCmdPv(mode, cap)) { lastModePv = mode; lastCapPv = cap; tLastPv = now; }
    }
  }
  if (ENABLED[S_WIND] && online[S_WIND]) {
    uint8_t mode = (gCapW >= 0.0f) ? 1 : 0;
    float cap = mode ? gCapW : 0.0f;
    if (gForceSend || (int)mode != lastModeW || fabsf(cap - lastCapW) >= CAP_RESEND_W || (mode && now - tLastW >= CAP_REFRESH_MS)) {
      if (sendWind(mode, cap)) { lastModeW = mode; lastCapW = cap; tLastW = now; }
    }
  }
  gForceSend = false;
}

// ---------------- SCADA: maquina de estados ----------------
void scadaService() {
  if (gBS_active()) { gState = ST_ARRANQUE; return; }
  if (!gAuto) return;

  // SEGURO: sin bateria no hay bus que supervisar; se deja el control local y no se recorta
  if (!ENABLED[S_BAT] || !online[S_BAT] || !gHaveBat) {
    if (gState != ST_SEGURO) event("S8 SEGURO: bateria sin respuesta, se suspende el recorte");
    gState = ST_SEGURO; gR = 0; gCapPv = gCapW = -1.0f; gOverCnt = gDefCnt = 0;
    return;
  }

  float pgen   = gP_W[S_PV] + gP_W[S_WIND];
  float pload  = gP_W[S_LOAD];
  bool  over   = gVbus_V > BAND_HI_V;
  bool  deficit = (gP_W[S_BAT] > DEF_BAT_W) || (gVbus_V < BAND_LO_V);
  gOverCnt = over    ? gOverCnt + 1 : 0;
  gDefCnt  = deficit ? gDefCnt  + 1 : 0;
  uint32_t now = millis();
  bool dwellOk = (now - gRt >= DWELL_MS) || (gVbus_V > VBUS_FAST_V);

  if (!anyCap()) {
    // entrada a EXCEDENTE: bus alto sostenido
    if (gOverCnt >= OVER_CYCLES) {
      float absorb  = fmaxf(0.0f, gIchMax) * gBatV;
      float surplus = pgen - pload - absorb;
      float need    = fminf(R_MAX_W, fmaxf(propStep(), surplus));
      float done    = cutGen(need);
      gR = done; gRt = now; gOverCnt = 0;
      char m[110]; snprintf(m, sizeof(m), "S1 ENTRADA a EXCEDENTE: Vbus>50.4 V, recorte %.0f W (tope PV %.0f / eolica %.0f)", done, gCapPv, gCapW); event(m);
    }
  } else if (dwellOk) {
    if (gOverCnt >= OVER_CYCLES) {                  // el bus sigue alto: un escalon mas
      float done = cutGen(propStep());
      gR += done; gRt = now; gOverCnt = 0;
      char m[110]; snprintf(m, sizeof(m), "S2 ESCALON ARRIBA: recorte +%.0f W (tope PV %.0f / eolica %.0f)", done, gCapPv, gCapW); event(m);
    } else if (gDefCnt >= DEF_CYCLES) {             // la bateria cubre deficit: se suelta un escalon
      relaxGen(R_STEP_W);
      gR = fmaxf(0.0f, gR - R_STEP_W); gRt = now; gDefCnt = 0;
      char m[110]; snprintf(m, sizeof(m), "S3 ESCALON ABAJO: bateria cubre deficit (tope PV %.0f / eolica %.0f)", gCapPv, gCapW); event(m);
      if (!anyCap()) { gR = 0; event("S4 LIBERAR: EXCEDENTE terminado, generadores en MPPT"); }
    }
  }

  if (anyCap())                   gState = ST_EXCEDENTE;
  else if (gDefCnt >= DEF_CYCLES) gState = ST_DEFICIT;
  else                            gState = ST_NORMAL;

  // deslastre escalonado de carga por SOC bajo (S6 sube de nivel, S7 baja de nivel)
  if (ENABLED[S_LOAD] && online[S_LOAD] && (now - gShedT >= SHED_DWELL_MS)) {
    int want = gShedLvl;
    if (gShedLvl < SHED_N && gSoc < SHED_SOC_ON[gShedLvl])        want = gShedLvl + 1;
    else if (gShedLvl > 0 && gSoc > SHED_SOC_OFF[gShedLvl - 1])   want = gShedLvl - 1;
    if (want != gShedLvl) {
      bool up = want > gShedLvl;
      gShedLvl = want; gShedT = now;
      gLoadNivel = (int)SHED_LEVEL[want]; gLoadModeCmd = (want > 0) ? 1 : 0;
      sendLoad(gLoadModeCmd, gLoadNivel, 0xFFFF);
      char m[110];
      snprintf(m, sizeof(m), up ? "S6 DESLASTRE nivel %d: SOC bajo, carga al %d %%" : "S7 RESTAURAR nivel %d: SOC recuperado, carga al %d %%", want, gLoadNivel);
      event(m);
    }
  }
}

// ---------------- Carga automatica ----------------
void setLoadScenario(int esc) {
  if (!ENABLED[S_LOAD]) return;
  gEsc = esc;
  bool ok = sendLoad(gLoadModeCmd, gLoadNivel, (uint16_t)esc);
  Serial.printf("escenario carga -> %s: %s\n", esc ? "INDUSTRIAL" : "RESIDENCIAL", ok ? "OK" : "FALLO I2C");
  pub("microred_ctrl/sistema", esc ? "{\"escenario\":\"industrial\"}" : "{\"escenario\":\"residencial\"}");
}

void loadSeqService() {
  if (!ENABLED[S_LOAD] || !online[S_LOAD] || !gAuto || !gLoadAuto || gBS_active()) return;
  uint32_t now = millis();
  if (!gEscInit) { gEscInit = true; gEscT = now; setLoadScenario(0); return; }
  if (now - gEscT >= LOAD_SEQ_MS) { gEscT = now; setLoadScenario(gEsc ? 0 : 1); }
}

// ---------------- Estado publicado ----------------
// Regla SCADA activa: S0 NORMAL | S1 EXCEDENTE | S5 DEFICIT | S6 deslastre | S8 SEGURO | S9 ARRANQUE
// (eventos puntuales: S2 escalon arriba, S3 escalon abajo, S4 liberar, S7 restaurar)
const char* activeRule() {
  switch (gState) {
    case ST_ARRANQUE:  return "S9";
    case ST_SEGURO:    return "S8";
    case ST_EXCEDENTE: return "S1";
    case ST_DEFICIT:   return "S5";
    default:           return (gShedLvl > 0) ? "S6" : "S0";
  }
}

void publishState() {
  char js[400];
  snprintf(js, sizeof(js),
    "{\"estado\":\"%s\",\"regla\":\"%s\",\"hora\":%.1f,\"recorte_w\":%.1f,\"cap_pv\":%.1f,\"cap_eol\":%.1f,\"esc\":%d,\"shed\":%d,"
    "\"auto\":%d,\"vbus\":%.2f,\"soc\":%.1f,\"pgen\":%.1f,\"pcarga\":%.1f,\"pbat\":%.1f}",
    ST_NAME[gState], activeRule(), hourNow(), gR, gCapPv, gCapW, gEsc, gShedLvl, gAuto ? 1 : 0, gVbus_V, gSoc,
    gP_W[S_PV] + gP_W[S_WIND], gP_W[S_LOAD], gP_W[S_BAT]);
  pub("microred_ctrl/estado", js);
  Serial.printf("SCADA %-9s [%s] h=%04.1f R=%.0f W capPV=%.0f capEol=%.0f Vbus=%.2f SOC=%.1f esc=%s%s\n",
    ST_NAME[gState], activeRule(), hourNow(), gR, gCapPv, gCapW, gVbus_V, gSoc, gEsc ? "IND" : "RES", gAuto ? "" : " [AUTO OFF]");
}

// ---------------- Comandos (MQTT / Serial) ----------------
bool parseNumber(const char* s, const char* key, float* out) {
  const char* p = strstr(s, key);
  if (!p) return false;
  p = strchr(p, ':');
  if (!p) return false;
  *out = (float)atof(p + 1);
  return true;
}

int subFromName(const char* n) {
  if (!strcmp(n, "pv")) return S_PV;
  if (!strcmp(n, "eolica") || !strcmp(n, "wind")) return S_WIND;
  if (!strcmp(n, "bateria") || !strcmp(n, "bat")) return S_BAT;
  if (!strcmp(n, "carga") || !strcmp(n, "load")) return S_LOAD;
  return -1;
}

void applyCommand(int s, int mode, float x, float y, bool haveY) {
  bool ok = false;
  uint16_t a = 0, b = 0;
  if (s == S_PV) {
    ok = sendCmdPv((uint8_t)mode, x);
  } else {
    if (s == S_WIND)      { a = (uint16_t)(x * 10.0f + 0.5f); b = 0; }
    else if (s == S_BAT)  { a = (uint16_t)(x * 10.0f + 0.5f); b = (uint16_t)(y * 10.0f + 0.5f); if (mode == 1 && !haveY) b = a; }
    else                  { a = (uint16_t)(x * 10.0f + 0.5f); b = haveY ? (uint16_t)y : 0xFFFF; }
    ok = sendCmd7(ADDR_B[s], 0x01, (uint8_t)mode, a, b);
  }
  if (s == S_PV || s == S_WIND) { gManualGen = true; Serial.println("(recorte automatico en pausa: 'auto on' para reanudar)"); }
  if (s == S_LOAD && ok) { gLoadModeCmd = (uint8_t)mode; if (mode == 1 && x > 0) gLoadNivel = (int)x; gLoadAuto = false; gShedLvl = 0; }
  Serial.printf("comando -> %s: modo %d x=%.1f y=%.1f: %s\n", NAME[s], mode, x, y, ok ? "OK" : "FALLO I2C");
  char ack[48]; snprintf(ack, sizeof(ack), "microred_ctrl/%s_cmd_ack", NAME[s]);
  pub(ack, ok ? "{\"ok\":true}" : "{\"ok\":false}");
}

void autoOn() {
  gAuto = true; gManualGen = false; gLoadAuto = true; gR = 0; gCapPv = gCapW = -1.0f; gOverCnt = gDefCnt = 0;
  gForceSend = true; gEscInit = false;
  event("AUTO ON: SCADA y carga automatica reanudados");
}
void autoOff() { gAuto = false; event("AUTO OFF: SCADA en pausa (solo comandos manuales)"); }

int modeFromWord(const char* w) {
  if (!strcmp(w, "mppt") || !strcmp(w, "normal")) return 0;
  if (!strcmp(w, "limited") || !strcmp(w, "reduced") || !strcmp(w, "reducida")) return 1;
  if (!strcmp(w, "safe")) return 2;
  return -1;
}

void onMqtt(char* topic, byte* payload, unsigned int len) {
  const char* slash = strrchr(topic, '/');
  char msg[120];
  if (len >= sizeof(msg)) len = sizeof(msg) - 1;
  memcpy(msg, payload, len); msg[len] = 0;
  if (slash && !strcmp(slash + 1, "sistema")) {
    if      (strstr(msg, "blackstart"))  blackstartStart();
    else if (strstr(msg, "industrial"))  { gLoadAuto = false; setLoadScenario(1); }
    else if (strstr(msg, "residencial")) { gLoadAuto = false; setLoadScenario(0); }
    else if (strstr(msg, "auto_on"))     autoOn();
    else if (strstr(msg, "auto_off"))    autoOff();
    else if (strstr(msg, "hora"))        { float h; if (parseNumber(msg, "\"hora\"", &h)) { setHour(h); event("Reloj de dia alineado"); } }
    return;
  }
  int s = slash ? subFromName(slash + 1) : -1;
  if (s < 0 || !ENABLED[s]) { Serial.printf("MQTT: topic sin subsistema: %s\n", topic); return; }
  int mode = 0; float num = 0, x = 0, y = 0; bool haveY = false;
  if      (strstr(msg, "safe"))    mode = 2;
  else if (strstr(msg, "limited") || strstr(msg, "reduced")) mode = 1;
  else if (strstr(msg, "mppt") || strstr(msg, "normal"))     mode = 0;
  else if (parseNumber(msg, "\"mode\"", &num)) mode = (int)num;
  else { Serial.printf("MQTT cmd ignorado: %s\n", msg); return; }
  if (mode < 0 || mode > 2) return;
  if (s == S_PV || s == S_WIND) parseNumber(msg, "\"pmax\"", &x);
  else if (s == S_BAT)  { parseNumber(msg, "\"ich\"", &x); haveY = parseNumber(msg, "\"idis\"", &y); }
  else                  { parseNumber(msg, "\"nivel\"", &x); haveY = parseNumber(msg, "\"esc\"", &y); }
  applyCommand(s, mode, x, y, haveY);
}

// ---------------- BLACK START (SCADA/EMS) ----------------
//  0) todo en SAFE   1) la BATERIA forma el bus   2) PV y EOLICA reconectan (MPPT)   3) la CARGA reconecta -> regimen normal
int      gBS = -1;
uint32_t gBSt = 0;
const uint32_t BS_STEP_MS = 6000;
bool gBS_active() { return gBS >= 0; }

void bsSay(const char* txt) {
  Serial.printf("BLACKSTART paso %d: %s\n", gBS, txt);
  char m[120]; snprintf(m, sizeof(m), "{\"blackstart\":%d,\"txt\":\"%s\"}", gBS, txt);
  pub("microred_ctrl/sistema", m);
}
void bsSetAll(uint8_t mode, bool pv, bool wind, bool bat, bool load) {
  if (pv   && ENABLED[S_PV])   sendCmdPv(mode, 0);
  if (wind && ENABLED[S_WIND]) sendCmd7(ADDR_B[S_WIND], 0x01, mode, 0, 0);
  if (bat  && ENABLED[S_BAT])  sendCmd7(ADDR_B[S_BAT],  0x01, mode, 0, 0);
  if (load && ENABLED[S_LOAD]) { gLoadModeCmd = mode; gLoadNivel = 100; gShedLvl = 0; sendLoad(mode, 100, 0xFFFF); }
}
void blackstartStart() { gBS = 0; gBSt = millis(); gR = 0; gCapPv = gCapW = -1.0f; bsSetAll(2, true, true, true, true); bsSay("TODO EN SAFE (bus caido)"); }
void blackstartService() {
  if (gBS < 0 || millis() - gBSt < BS_STEP_MS) return;
  gBSt = millis(); gBS++;
  if (gBS == 1)      { bsSetAll(0, false, false, true, false); bsSay("la bateria forma el bus"); }
  else if (gBS == 2) { bsSetAll(0, true, true, false, false);  bsSay("PV y eolica reconectan (MPPT)"); }
  else if (gBS == 3) { bsSetAll(0, false, false, false, true); bsSay("carga reconectada: regimen normal"); }
  else {
    gBS = -1; gForceSend = true; gState = ST_NORMAL; gRt = millis();
    pub("microred_ctrl/sistema", "{\"blackstart\":-1,\"txt\":\"completado\"}");
    Serial.println("BLACKSTART completado");
  }
}

void handleSerial() {
  if (!Serial.available()) return;
  String line = Serial.readStringUntil('\n');
  line.trim(); line.toLowerCase();
  if (line.length() == 0) return;
  char buf[96]; line.toCharArray(buf, sizeof(buf));
  char* tok[5]; int n = 0;
  for (char* p = strtok(buf, " "); p && n < 5; p = strtok(NULL, " ")) tok[n++] = p;
  if (n > 0 && !strcmp(tok[0], "blackstart")) { blackstartStart(); return; }
  if (n > 1 && !strcmp(tok[0], "auto")) { if (!strcmp(tok[1], "on")) autoOn(); else autoOff(); return; }
  if (n > 1 && !strcmp(tok[0], "hora")) { setHour((float)atof(tok[1])); Serial.printf("reloj alineado: hora %.1f\n", hourNow()); return; }
  if (n > 1 && !strcmp(tok[0], "load") && (!strcmp(tok[1], "res") || !strcmp(tok[1], "ind"))) { gLoadAuto = false; setLoadScenario(!strcmp(tok[1], "ind")); return; }
  int s = n > 0 ? subFromName(tok[0]) : -1;
  int mode = n > 1 ? modeFromWord(tok[1]) : -1;
  if (s < 0 || mode < 0 || !ENABLED[s]) { Serial.println("Uso: <pv|wind|bat|load> <mppt|normal|limited|reduced|safe> [x] [y] | load res|ind | auto on|off | hora <H> | blackstart"); return; }
  float x = n > 2 ? atof(tok[2]) : 0, y = n > 3 ? atof(tok[3]) : 0;
  applyCommand(s, mode, x, y, n > 3);
}

// ---------------- WiFi/MQTT sin bloquear ----------------
void netService() {
  if (WiFi.status() != WL_CONNECTED) {
    if (millis() - tNet >= 8000) { tNet = millis(); Serial.println("WiFi: (re)conectando..."); WiFi.disconnect(); WiFi.begin(WIFI_SSID, WIFI_PASS); }
    return;
  }
  if (!mqtt.connected()) {
    if (millis() - tNet >= 5000) {
      tNet = millis();
      char id[32]; snprintf(id, sizeof(id), "esp32s3_gw_%04X", (unsigned)(ESP.getEfuseMac() & 0xFFFF));
      if (mqtt.connect(id)) { mqtt.subscribe("microred_cmd/#"); Serial.printf("MQTT conectado (%s), suscrito a microred_cmd/#\n", id); }
      else Serial.printf("MQTT fallo rc=%d (revisa MQTT_HOST y Mosquitto en 0.0.0.0)\n", mqtt.state());
    }
    return;
  }
  mqtt.loop();
}

void setup() {
  Serial.begin(115200);
  unsigned long t0 = millis();
  while (!Serial && millis() - t0 < 2000) delay(10);
  Serial.println("\n== Gateway MICRORED v7 (PV + eolica + bateria + carga) ==");
  Serial.println("Comandos: <pv|wind|bat|load> <mppt|normal|limited|reduced|safe> [x] [y] | load res|ind | auto on|off | hora <H> | blackstart");
  Wire.begin(PIN_SDA, PIN_SCL, I2C_HZ);
  Wire.setTimeOut(100);
  i2cScan();
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setCallback(onMqtt);
  mqtt.setBufferSize(512);
  gDay0 = millis();
  gRt = millis();
  gState = ST_NORMAL;
}

void loop() {
  handleSerial();
  blackstartService();
  netService();
  if (millis() - tPoll >= POLL_MS) {
    tPoll = millis();
    if (EN_PV)   { pollPv();        delay(5); }
    if (EN_WIND) { pollSub(S_WIND); delay(5); }
    if (EN_LOAD) { pollSub(S_LOAD); delay(5); }
    if (EN_BAT)  { pollSub(S_BAT);  delay(5); }
    coupleMicrogrid();
    scadaService();
    applyGenCaps();
    loadSeqService();
    publishState();
    Serial.println();

    int nOn = 0;
    for (int s = 0; s < N_SUB; s++) if (ENABLED[s] && online[s]) nOn++;
    gAllDown = (nOn == 0) ? gAllDown + 1 : 0;
    if (gAllDown >= RESTART_AFTER_FAILS) { i2cRestart(); gAllDown = 0; }
  }
}