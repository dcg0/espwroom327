// DC ECU — panel web en ESP32 WROOM con ELM327 Bluetooth Classic (SPP).
//
// Arranque:
//   1. Lee de NVS la MAC y el PIN del último adaptador usado.
//   2. Levanta WiFi AP + servidor web (el panel siempre responde).
//   3. Un hilo de Bluetooth intenta conectar solo, con reintentos y backoff.
//   4. Si el enlace se cae, vuelve a intentar; nunca se queda colgado.
//   5. /api/status publica estado, intentos, cuenta atrás y bitácora real.
#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <LittleFS.h>
#include <BluetoothSerial.h>
#include <Preferences.h>

// ----------------------------- Configuración --------------------------------
// SSID solo ASCII: los emojis rompen el punto de acceso en muchos telefonos.
static const char *AP_SSID = "Buss-PANEL";
static const char *AP_PASS = "Juanita123";
static const char *BT_NAME = "Buss-PANEL";
static const char *DEFAULT_PIN = "1234";
static const uint32_t PID_INTERVAL_MS = 45;
static const uint16_t PID_TIMEOUT_MS = 160;
static const uint32_t SCAN_MS = 4000;
static const uint32_t RETRY_BASE_MS = 5000;
static const uint32_t RETRY_MAX_MS = 60000;
static const uint8_t MAX_FAILS_BEFORE_DROP = 3;

WebServer server(WEBSERVER_PORT);
BluetoothSerial SerialBT;
Preferences prefs;

// ------------------------------- PIDs OBD-II --------------------------------
struct PidDef {
  const char *pid;
  const char *nombre;
  const char *unidad;
  uint8_t dec;
};

static const PidDef PIDS[] = {
  {"0C", "RPM", "rpm", 0},
  {"0D", "Velocidad", "km/h", 0},
  {"05", "Temp. refrigerante", "C", 0},
  {"04", "Carga calculada", "%", 1},
  {"11", "Acelerador (TPS)", "%", 1},
  {"0E", "Avance de encendido", "deg", 1},
  {"42", "Voltaje del modulo", "V", 2},
  {"0B", "Presion MAP", "kPa", 0},
  {"0F", "Temp. admision", "C", 0},
  {"10", "Flujo MAF", "g/s", 2},
  {"33", "Presion barometrica", "kPa", 0},
  {"06", "Trim corto B1", "%", 1},
  {"07", "Trim largo B1", "%", 1},
  {"2F", "Nivel de combustible", "%", 1},
  {"5E", "Consumo de combustible", "L/h", 2},
  {"0A", "Presion de combustible", "kPa", 0},
  {"14", "Sensor O2 B1S1", "V", 3},
  {"15", "Sensor O2 B1S2", "V", 3},
  {"5C", "Temp. del aceite", "C", 0},
  {"43", "Carga absoluta", "%", 1},
};
static const uint8_t PID_COUNT = sizeof(PIDS) / sizeof(PIDS[0]);

static float gValue[PID_COUNT];
static bool gValid[PID_COUNT];
static uint8_t gCursor = 0;
static uint32_t gLastPoll = 0;

// ------------------------------ Estado del enlace ---------------------------
enum LinkState : uint8_t { ST_BOOT, ST_SCANNING, ST_CONNECTING, ST_READY, ST_RETRY, ST_OFF };

static volatile LinkState gState = ST_BOOT;
static bool gReady = false;
static bool gAutoConnect = true;
static String gSavedMac;
static String gSavedName;
static String gPin = DEFAULT_PIN;
static String gProtocol = "--";
static String gLastError;
static uint32_t gAttempts = 0;
static uint32_t gNextRetryAt = 0;
static uint32_t gNextAutoScanAt = 0;
static uint32_t gConnectedAt = 0;
static uint8_t gConsecutiveFails = 0;

// Escaneo asíncrono: el servidor web nunca se bloquea esperando Bluetooth.
struct FoundDevice {
  String name;
  String mac;
  int rssi;
};
static const uint8_t MAX_FOUND = 12;
static FoundDevice gFound[MAX_FOUND];
static volatile uint8_t gFoundCount = 0;
static volatile bool gScanRunning = false;
static volatile uint32_t gScanStartedAt = 0;

// Bitácora en memoria: el panel ve actividad real, no una pantalla vacía.
static const uint8_t LOG_MAX = 14;
static String gLogMsg[LOG_MAX];
static uint32_t gLogAt[LOG_MAX];
static uint8_t gLogCount = 0;
static uint8_t gLogHead = 0;

static void addLog(const String &msg) {
  gLogMsg[gLogHead] = msg;
  gLogAt[gLogHead] = millis();
  gLogHead = (uint8_t)((gLogHead + 1) % LOG_MAX);
  if (gLogCount < LOG_MAX) gLogCount++;
  Serial.printf("[BT] %s\n", msg.c_str());
}

static const char *stateName() {
  switch (gState) {
    case ST_SCANNING: return "escaneando";
    case ST_CONNECTING: return "conectando";
    case ST_READY: return "enlazado";
    case ST_RETRY: return "reintentando";
    case ST_OFF: return "apagado";
    default: return "arrancando";
  }
}

// ------------------------------ Guardado (NVS) ------------------------------
static void loadSettings() {
  prefs.begin("dcecu", false);
  gSavedMac = prefs.getString("mac", "");
  gSavedName = prefs.getString("name", "");
  gPin = prefs.getString("pin", DEFAULT_PIN);
  gAutoConnect = prefs.getBool("auto", true);
  if (gPin.length() < 4) gPin = DEFAULT_PIN;
  addLog("config: " + (gSavedMac.length() ? gSavedMac : String("sin adaptador guardado")));
}

static void saveAdapter(const String &mac, const String &name) {
  gSavedMac = mac;
  gSavedName = name;
  prefs.putString("mac", mac);
  prefs.putString("name", name);
}

// --------------------------- Enlace con el ELM327 ---------------------------
static String sendCmd(const String &cmd, uint16_t timeout = PID_TIMEOUT_MS) {
  while (SerialBT.available()) SerialBT.read();
  SerialBT.print(cmd);
  SerialBT.print('\r');
  uint32_t start = millis();
  String out;
  while (millis() - start < timeout) {
    while (SerialBT.available()) {
      char c = (char)SerialBT.read();
      if (c == '\r' || c == '\n') {
        out.trim();
        if (out.length()) return out;
        out = "";
      } else if (c != ' ' || out.length()) {
        out += c;
      }
    }
    delay(2);
  }
  out.trim();
  return out;
}

static float decodePid(uint8_t index, uint8_t a, uint8_t b) {
  switch (index) {
    case 0: return ((a * 256.0f) + b) / 4.0f;
    case 1: return a;
    case 2: return a - 40.0f;
    case 3: return a * 100.0f / 255.0f;
    case 4: return a * 100.0f / 255.0f;
    case 5: return a / 2.0f - 64.0f;
    case 6: return ((a * 256.0f) + b) / 1000.0f;
    case 7: return a;
    case 8: return a - 40.0f;
    case 9: return ((a * 256.0f) + b) / 100.0f;
    case 10: return a;
    case 11: return (a - 128.0f) * 100.0f / 128.0f;
    case 12: return (a - 128.0f) * 100.0f / 128.0f;
    case 13: return a * 100.0f / 255.0f;
    case 14: return ((a * 256.0f) + b) / 20.0f;
    case 15: return a * 3.0f;
    case 16: return a / 200.0f;
    case 17: return a / 200.0f;
    case 18: return a - 40.0f;
    case 19: return a * 100.0f / 255.0f;
    default: return 0;
  }
}

static bool parsePidLine(const String &line, uint8_t index, uint8_t &a, uint8_t &b) {
  String hex = line;
  hex.replace(" ", "");
  if (hex.length() < 6 || !hex.startsWith("41")) return false;
  if (!hex.substring(2, 4).equalsIgnoreCase(PIDS[index].pid)) return false;
  a = (uint8_t)strtol(hex.substring(4, 6).c_str(), nullptr, 16);
  b = hex.length() >= 8 ? (uint8_t)strtol(hex.substring(6, 8).c_str(), nullptr, 16) : 0;
  return true;
}

static String protocolName(int number) {
  switch (number) {
    case 1: return "SAE J1850 PWM";
    case 2: return "SAE J1850 VPW";
    case 3: case 4: case 5: case 6: return "ISO 15765-4 CAN";
    case 7: return "ISO 9141-2";
    case 8: case 9: return "ISO 14230-4 KWP";
    default: return "AUTO";
  }
}

static bool initAdapter() {
  addLog("inicializando adaptador");
  sendCmd("ATZ", 900);
  sendCmd("ATE0");
  sendCmd("ATL0");
  sendCmd("ATS0");
  sendCmd("ATSP0", 600);
  String dpn = sendCmd("ATDPN", 800);
  if (dpn.indexOf("ERROR") >= 0 || dpn.length() == 0) {
    gLastError = "el adaptador no respondio a ATDPN";
    return false;
  }
  int number = 0;
  for (size_t i = 0; i < dpn.length(); i++) {
    if (isDigit(dpn[i])) {
      number = dpn.substring(i).toInt();
      break;
    }
  }
  gProtocol = protocolName(number);
  addLog("protocolo " + gProtocol);
  return true;
}

static void scheduleRetry(const String &reason) {
  gReady = false;
  gState = ST_RETRY;
  gLastError = reason;
  gAttempts++;
  uint32_t shift = gAttempts > 4 ? 4 : gAttempts;
  uint32_t wait = RETRY_BASE_MS << (shift > 0 ? shift - 1 : 0);
  if (wait > RETRY_MAX_MS) wait = RETRY_MAX_MS;
  gNextRetryAt = millis() + wait;
  addLog(reason + " · reintento " + String(gAttempts) + " en " + String(wait / 1000) + " s");
}

static bool connectAdapter(const String &mac, const String &name) {
  gState = ST_CONNECTING;
  gLastError = "";
  addLog("emparejando " + (name.length() ? name : mac));
  SerialBT.setPin(gPin.c_str());
  if (SerialBT.connected()) SerialBT.disconnect();
  BTAddress addr(mac.c_str());
  if (!SerialBT.connect(addr)) {
    scheduleRetry("no se pudo abrir el enlace SPP");
    return false;
  }
  addLog("enlace SPP abierto, PIN " + gPin);
  if (!initAdapter()) {
    SerialBT.disconnect();
    scheduleRetry(gLastError.length() ? gLastError : "inicializacion fallida");
    return false;
  }
  saveAdapter(mac, name);
  gReady = true;
  gState = ST_READY;
  gConnectedAt = millis();
  gConsecutiveFails = 0;
  addLog("listo · " + gProtocol);
  return true;
}

// Escaneo en su propio hilo: discover() bloquea, el servidor web no.
static void scanTask(void *) {
  gScanRunning = true;
  gScanStartedAt = millis();
  gFoundCount = 0;
  gState = ST_SCANNING;
  addLog("escaneando " + String(SCAN_MS / 1000) + " s");
  if (SerialBT.connected()) {
    addLog("escaneo omitido: hay enlace activo");
    gScanRunning = false;
    gState = gReady ? ST_READY : ST_RETRY;
    vTaskDelete(nullptr);
    return;
  }
  SerialBT.discover(SCAN_MS);
  int count = SerialBT.getDeviceCount();
  uint8_t stored = 0;
  for (int i = 0; i < count && stored < MAX_FOUND; i++) {
    String mac = SerialBT.getDeviceAddress(i).toString();
    if (!mac.length()) continue;
    gFound[stored].name = SerialBT.getDeviceName(i);
    gFound[stored].mac = mac;
    gFound[stored].rssi = SerialBT.getDeviceRSSI(i);
    stored++;
  }
  gFoundCount = stored;
  addLog("escaneo listo: " + String(stored) + " dispositivos");
  gScanRunning = false;
  gState = gReady ? ST_READY : (gAutoConnect && gSavedMac.length() ? ST_RETRY : ST_OFF);
  vTaskDelete(nullptr);
}

static void startScan() {
  if (gScanRunning) return;
  xTaskCreatePinnedToCore(scanTask, "btScan", 4096, nullptr, 1, nullptr, 0);
}

// Hilo de enlace: mantiene la conexión viva y reintenta sin bloquear.
static void btTask(void *) {
  for (;;) {
    if (!gAutoConnect) {
      // Con la reconexión pausada el escaneo sigue disponible al encender.
      if (!gScanRunning && (int32_t)(millis() - gNextAutoScanAt) >= 0) {
        startScan();
        gNextAutoScanAt = millis() + 15000;
      }
      gState = ST_OFF;
      vTaskDelay(pdMS_TO_TICKS(500));
      continue;
    }
    if (gReady) {
      if (!SerialBT.connected()) {
        addLog("enlace perdido");
        scheduleRetry("el adaptador cerro el enlace");
      }
      vTaskDelay(pdMS_TO_TICKS(500));
      continue;
    }
    if (gState == ST_SCANNING || gState == ST_CONNECTING || gScanRunning) {
      vTaskDelay(pdMS_TO_TICKS(200));
      continue;
    }
    if (gSavedMac.length() == 0) {
      // Sin adaptador guardado: escanea solo al encender y cada 15 s, para que
      // el panel muestre dispositivos cercanos sin tocar nada.
      if ((int32_t)(millis() - gNextAutoScanAt) >= 0) {
        startScan();
        gNextAutoScanAt = millis() + 15000;
      }
      gState = ST_OFF;
      vTaskDelay(pdMS_TO_TICKS(500));
      continue;
    }
    if ((int32_t)(millis() - gNextRetryAt) < 0) {
      vTaskDelay(pdMS_TO_TICKS(200));
      continue;
    }
    connectAdapter(gSavedMac, gSavedName);
    vTaskDelay(pdMS_TO_TICKS(200));
  }
}

// -------------------------------- Sondeo OBD --------------------------------
static void pollOnce() {
  if (!gReady) return;
  if (millis() - gLastPoll < PID_INTERVAL_MS) return;
  gLastPoll = millis();

  String cmd = "01";
  cmd += PIDS[gCursor].pid;
  String res = sendCmd(cmd);
  bool answered = false;
  if (res.indexOf("NO DATA") < 0 && res.indexOf("STOPPED") < 0 && res.length() > 0) {
    uint8_t a = 0, b = 0;
    if (parsePidLine(res, gCursor, a, b)) {
      gValue[gCursor] = decodePid(gCursor, a, b);
      gValid[gCursor] = true;
      answered = true;
    }
  }
  if (!answered) {
    gValid[gCursor] = false;
    gConsecutiveFails++;
    if (gConsecutiveFails >= MAX_FAILS_BEFORE_DROP) {
      addLog("sin respuestas del ELM327");
      SerialBT.disconnect();
      scheduleRetry("el ELM327 dejo de responder");
    }
  } else {
    gConsecutiveFails = 0;
  }
  gCursor = (uint8_t)((gCursor + 1) % PID_COUNT);
}

// -------------------------------- API JSON ----------------------------------
static String logJson() {
  String out = "[";
  for (uint8_t i = 0; i < gLogCount; i++) {
    uint8_t index = (uint8_t)((gLogHead + LOG_MAX - gLogCount + i) % LOG_MAX);
    if (i) out += ',';
    out += "{\"t\":" + String(gLogAt[index]) + ",\"msg\":\"" + gLogMsg[index] + "\"}";
  }
  out += "]";
  return out;
}

static String statusJson() {
  int32_t left = (int32_t)(gNextRetryAt - millis());
  if (left < 0) left = 0;
  String out = "{";
  out += "\"ssid\":\"" + String(AP_SSID) + "\"";
  out += ",\"ip\":\"" + WiFi.softAPIP().toString() + "\"";
  out += ",\"state\":\"" + String(stateName()) + "\"";
  out += ",\"ready\":";
  out += gReady ? "true" : "false";
  out += ",\"auto\":";
  out += gAutoConnect ? "true" : "false";
  out += ",\"device\":\"" + gSavedMac + "\"";
  out += ",\"deviceName\":\"" + gSavedName + "\"";
  out += ",\"pin\":\"" + gPin + "\"";
  out += ",\"protocol\":\"" + gProtocol + "\"";
  out += ",\"attempts\":" + String(gAttempts);
  out += ",\"retryInMs\":" + String(left);
  out += ",\"uptimeMs\":" + String(millis());
  out += ",\"connectedMs\":" + String(gReady ? millis() - gConnectedAt : 0);
  out += ",\"error\":\"" + gLastError + "\"";
  out += ",\"scan\":{\"running\":";
  out += gScanRunning ? "true" : "false";
  out += ",\"elapsedMs\":" + String(gScanRunning ? millis() - gScanStartedAt : 0);
  out += ",\"devices\":[";
  for (uint8_t i = 0; i < gFoundCount; i++) {
    if (i) out += ',';
    out += "{\"nombre\":\"" + gFound[i].name + "\",\"mac\":\"" + gFound[i].mac +
           "\",\"rssi\":" + String(gFound[i].rssi) + "}";
  }
  out += "]}";
  out += ",\"log\":" + logJson();
  out += "}";
  return out;
}

static String valuesJson() {
  String out = "{\"connected\":";
  out += gReady ? "true" : "false";
  out += ",\"protocol\":\"" + gProtocol + "\",\"device\":\"" + gSavedMac + "\",\"v\":{";
  for (uint8_t i = 0; i < PID_COUNT; i++) {
    if (i) out += ',';
    out += '"' + String(PIDS[i].pid) + "\":";
    if (gValid[i]) out += String(gValue[i], PIDS[i].dec);
    else out += "null";
  }
  out += "}}";
  return out;
}

static void handleValues() { server.send(200, "application/json", valuesJson()); }

static void handleStatus() { server.send(200, "application/json", statusJson()); }

static void handlePids() {
  String out = "[";
  for (uint8_t i = 0; i < PID_COUNT; i++) {
    if (i) out += ',';
    out += "{\"pid\":\"" + String(PIDS[i].pid) + "\",\"nombre\":\"" + String(PIDS[i].nombre) +
           "\",\"unidad\":\"" + String(PIDS[i].unidad) + "\",\"dec\":" + String(PIDS[i].dec) + "}";
  }
  out += "]";
  server.send(200, "application/json", out);
}

static void handleScanStart() {
  if (gReady) {
    server.send(409, "application/json", "{\"error\":\"desconecta antes de buscar\"}");
    return;
  }
  startScan();
  server.send(202, "application/json", "{\"started\":true}");
}

static void handleConnect() {
  String mac = server.arg("mac");
  if (!mac.length()) {
    server.send(400, "application/json", "{\"error\":\"falta mac\"}");
    return;
  }
  String name = "";
  for (uint8_t i = 0; i < gFoundCount; i++) {
    if (gFound[i].mac.equalsIgnoreCase(mac)) name = gFound[i].name;
  }
  saveAdapter(mac, name);
  gAutoConnect = true;
  prefs.putBool("auto", true);
  gNextRetryAt = millis();
  server.send(202, "application/json", "{\"queued\":true}");
}

static void handleDisconnect() {
  gAutoConnect = false;
  prefs.putBool("auto", false);
  if (SerialBT.connected()) SerialBT.disconnect();
  gReady = false;
  gState = ST_OFF;
  for (uint8_t i = 0; i < PID_COUNT; i++) gValid[i] = false;
  addLog("desconexion manual · autoapagado");
  server.send(200, "application/json", "{\"ok\":true}");
}

static void handleAuto() {
  gAutoConnect = server.arg("on") != "0";
  prefs.putBool("auto", gAutoConnect);
  gNextRetryAt = millis();
  addLog(gAutoConnect ? "reconexion automatica activada" : "reconexion automatica pausada");
  server.send(200, "application/json", "{\"ok\":true}");
}

static void handlePin() {
  String pin = server.arg("pin");
  if (pin.length() != 4) {
    server.send(400, "application/json", "{\"error\":\"el PIN debe tener 4 digitos\"}");
    return;
  }
  gPin = pin;
  prefs.putString("pin", gPin);
  SerialBT.setPin(gPin.c_str());
  addLog("PIN actualizado");
  server.send(200, "application/json", "{\"ok\":true}");
}

static void handleDtc() {
  String out = "{\"stored\":[";
  String res = sendCmd("03", 1200);
  res.replace(" ", "");
  bool first = true;
  for (size_t i = 0; i + 4 <= res.length(); i += 4) {
    if (res.substring(i, i + 2) == "43") continue;
    String code = res.substring(i, i + 4);
    if (code == "0000") continue;
    if (!first) out += ',';
    first = false;
    out += '"' + code + '"';
  }
  out += "],\"pending\":[],\"permanent\":[]}";
  server.send(200, "application/json", out);
}

static void handleDtcClear() {
  String res = sendCmd("04", 1500);
  server.send(200, "application/json",
              res.indexOf("44") >= 0 ? "{\"ok\":true}" : "{\"error\":\"sin confirmacion\"}");
}

// ------------------------------ Servidor web --------------------------------
static bool serveFile(const char *path, const char *type) {
  if (!LittleFS.exists(path)) return false;
  File file = LittleFS.open(path, "r");
  server.streamFile(file, type);
  file.close();
  return true;
}

static void handleRoot() {
  if (!serveFile("/index.html", "text/html; charset=utf-8")) {
    server.send(500, "text/plain", "Falta index.html: sube el sistema de archivos (pio run -t uploadfs)");
  }
}

void setup() {
  Serial.begin(115200);
  LittleFS.begin(true);
  loadSettings();

  WiFi.mode(WIFI_AP);
  bool apOk = WiFi.softAP(AP_SSID, AP_PASS, 1, 0, 4);
  if (!apOk) {
    delay(400);
    apOk = WiFi.softAP(AP_SSID, AP_PASS, 1, 0, 4);
  }
  Serial.printf("AP \"%s\" %s · IP %s\n", AP_SSID, apOk ? "activo" : "FALLO",
                WiFi.softAPIP().toString().c_str());
  SerialBT.begin(BT_NAME, true);
  SerialBT.setPin(gPin.c_str());

  server.on("/", HTTP_GET, handleRoot);
  server.on("/api/values", HTTP_GET, handleValues);
  server.on("/api/status", HTTP_GET, handleStatus);
  server.on("/api/pids", HTTP_GET, handlePids);
  server.on("/api/scan", HTTP_POST, handleScanStart);
  server.on("/api/scan", HTTP_GET, handleStatus);
  server.on("/api/connect", HTTP_POST, handleConnect);
  server.on("/api/disconnect", HTTP_POST, handleDisconnect);
  server.on("/api/auto", HTTP_POST, handleAuto);
  server.on("/api/pin", HTTP_POST, handlePin);
  server.on("/api/dtc", HTTP_GET, handleDtc);
  server.on("/api/dtc/clear", HTTP_POST, handleDtcClear);
  server.serveStatic("/", LittleFS, "/");
  server.begin();

  addLog("panel listo en " + WiFi.softAPIP().toString());

  // Hilo de enlace: reintenta para siempre, con backoff, sin bloquear el loop.
  xTaskCreatePinnedToCore(btTask, "btLink", 4096, nullptr, 1, nullptr, 0);

  if (gSavedMac.length() && gAutoConnect) {
    gNextRetryAt = millis() + 1500;
    addLog("autoarranque con adaptador guardado");
  } else {
    gNextAutoScanAt = millis() + 1500;
    addLog("sin adaptador guardado · escaneo automatico al arrancar");
  }
  Serial.printf("Panel en http://%s/ (PIN ELM327: %s)\n",
                WiFi.softAPIP().toString().c_str(), gPin.c_str());
}

void loop() {
  server.handleClient();
  pollOnce();
}
