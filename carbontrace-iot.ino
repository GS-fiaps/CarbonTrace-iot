// ============================================================
//  CarbonTrace v3.0 — Sensor de Qualidade do Ar
//  FIAP Global Solution 2026/1 | Disruptive Architectures
//
//  Sensores:  DHT22  (GPIO4)  — Temperatura e Umidade
//             MQ-135 (GPIO34) — CO2 / Qualidade do Ar (ADC analógico)
//             PM2.5  (GPIO35) — Material Particulado  (ADC analógico)
//  Saidas:    LED Verde (GPIO18) | LED Vermelho (GPIO19) | Buzzer (GPIO23)
//  Controle:  Botão Reset (GPIO14)
//  Interface: LCD 16x2 I2C (SDA=21 / SCL=22)
//  Rede:      Wi-Fi + WebServer porta 80 + HTTP POST para backend externo
//
//  Endpoints:
//    GET  /api/sensors     — leitura atual de todos os sensores
//    GET  /api/status      — status do dispositivo e configurações
//    GET  /api/air-quality — índice de qualidade do ar e recomendações
//    GET  /api/history     — histórico de leituras (últimas 10)
//    POST /api/config      — altera thresholds e URL do backend
//    GET  /api/docs        — documentação da API (HTML)
//    GET  /dashboard       — painel HTML em tempo real
//
//  Caso de uso: Correlacionar desmatamento com qualidade do ar;
//               dados complementares ao monitoramento satelital.
// ============================================================

#include <WiFi.h>
#include <WebServer.h>
#include <HTTPClient.h>   // ESP32 Arduino core — client HTTP para backend
#include <DHT.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <ArduinoJson.h>

// ── Credenciais Wi-Fi ─────────────────────────────────────────
const char* SSID     = "Wokwi-GUEST";
const char* PASSWORD = "";

// ── Pinagem ──────────────────────────────────────────────────
#define DHT_PIN      4    // DHT22 — temperatura e umidade
#define DHT_TYPE     DHT22
#define MQ135_PIN    34   // MQ-135 — CO2 / gases (GPIO input-only, ADC1)
#define PM25_PIN     35   // PM2.5  — partículas  (GPIO input-only, ADC1)
#define BTN_PIN      14   // Botão reset de alertas (INPUT_PULLUP)
#define LED_GREEN    18   // LED status NORMAL / qualidade BOA
#define LED_RED      19   // LED status ALERTA / qualidade RUIM
#define BUZZER_PIN   23   // Buzzer sonoro de alerta

// ── Thresholds de Alerta (configuráveis via POST /api/config) ─
int   co2AlertThreshold  = 1000;   // ppm   — acima: ar comprometido
float pm25AlertThreshold = 35.0f;  // µg/m³ — acima: não saudável (EPA)
float tempAlertThreshold = 40.0f;  // °C    — acima: temperatura crítica

// ── Backend Externo ──────────────────────────────────────────
// Configurar via POST /api/config com campo "backend_url"
// Envio automático a cada BACKEND_INTERVAL milissegundos
String backendUrl = "";
const  unsigned long BACKEND_INTERVAL = 30000UL; // 30 segundos
unsigned long lastBackendPost = 0;

// ── Objetos Globais ──────────────────────────────────────────
DHT               dht(DHT_PIN, DHT_TYPE);
LiquidCrystal_I2C lcd(0x27, 16, 2);
WebServer         server(80);

// ── Estrutura de Leitura ─────────────────────────────────────
struct SensorReading {
  float         temperature;   // °C
  float         humidity;      // %
  int           co2Raw;        // ADC bruto (0-4095)
  float         co2Ppm;        // ppm estimado
  int           pm25Raw;       // ADC bruto (0-4095)
  float         pm25UgM3;      // µg/m³ estimado
  String        airQuality;    // GOOD | MODERATE | UNHEALTHY | HAZARDOUS
  bool          alert;
  String        status;        // NORMAL | ALERTA
  unsigned long uptimeS;       // segundos desde o boot
};

// Histórico circular (últimas 10 leituras)
SensorReading history[10];
int historyIndex = 0;
int historyCount = 0;

SensorReading current;
unsigned long lastRead    = 0;
unsigned long startMillis = 0;
const int     READ_INTERVAL = 5000; // ms

// Controle do botão
bool lastBtnState = HIGH;

// Controle do buzzer (soa por 1 segundo no alerta)
unsigned long buzzerStart = 0;
bool          buzzerOn    = false;

// Warm-up do MQ-135 (~20s para estabilização do elemento sensor)
bool          mq135Ready  = false;
unsigned long warmupStart = 0;
const unsigned long WARMUP_MS = 20000UL;

// ── Protótipos ────────────────────────────────────────────────
void readSensors();
float calcCO2Ppm(int raw);
float calcPM25UgM3(int raw);
String calcAirQuality(float co2, float pm25);
void updateLEDs();
void updateLCD();
void updateBuzzer();
void checkButton();
void resetAlerts();
void saveToHistory(SensorReading& r);
void sendToBackend();
void handleDashboard();
void handleSensors();
void handleStatus();
void handleAirQuality();
void handleHistory();
void handleConfig();
void handleDocs();
void handleNotFound();

// ════════════════════════════════════════════════════════════
//  SETUP
// ════════════════════════════════════════════════════════════
void setup() {
  Serial.begin(115200);
  delay(100);

  dht.begin();
  pinMode(LED_GREEN,  OUTPUT);
  pinMode(LED_RED,    OUTPUT);
  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(BTN_PIN,    INPUT_PULLUP);

  Wire.begin(21, 22);
  lcd.init();
  lcd.backlight();
  lcd.setCursor(0, 0);
  lcd.print("CarbonTrace v3.0");
  lcd.setCursor(0, 1);
  lcd.print("MQ135 warm-up...");

  Serial.println(F("\n=============================="));
  Serial.println(F("  CarbonTrace ESP32 v3.0"));
  Serial.println(F("  Sensor de Qualidade do Ar"));
  Serial.println(F("  FIAP Global Solution 2026"));
  Serial.println(F("=============================="));
  Serial.println(F("[MQ-135] Aguardando 20s warm-up..."));

  warmupStart = millis();

  // Conectar ao Wi-Fi
  WiFi.begin(SSID, PASSWORD);
  Serial.print(F("Conectando ao Wi-Fi"));
  int tries = 0;
  while (WiFi.status() != WL_CONNECTED && tries < 20) {
    delay(500);
    Serial.print(F("."));
    tries++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println(F("\nWi-Fi conectado!"));
    Serial.println("IP: " + WiFi.localIP().toString());
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("IP:");
    lcd.setCursor(0, 1);
    lcd.print(WiFi.localIP().toString());
    delay(2000);
  } else {
    Serial.println(F("\nSem Wi-Fi - modo offline"));
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("Modo Offline");
    delay(1000);
  }

  // Registrar rotas
  server.on("/dashboard",       HTTP_GET,  handleDashboard);
  server.on("/api/sensors",     HTTP_GET,  handleSensors);
  server.on("/api/status",      HTTP_GET,  handleStatus);
  server.on("/api/air-quality", HTTP_GET,  handleAirQuality);
  server.on("/api/history",     HTTP_GET,  handleHistory);
  server.on("/api/config",      HTTP_POST, handleConfig);
  server.on("/api/docs",        HTTP_GET,  handleDocs);
  server.onNotFound(handleNotFound);
  server.begin();

  Serial.println(F("WebServer iniciado na porta 80"));
  Serial.println(F("------------------------------"));
  Serial.println(F("Endpoints:"));
  Serial.println(F("  GET  /dashboard"));
  Serial.println(F("  GET  /api/sensors"));
  Serial.println(F("  GET  /api/status"));
  Serial.println(F("  GET  /api/air-quality"));
  Serial.println(F("  GET  /api/history"));
  Serial.println(F("  POST /api/config"));
  Serial.println(F("  GET  /api/docs"));
  Serial.println(F("------------------------------"));

  startMillis = millis();
  readSensors();
  updateLEDs();
  updateLCD();
}

// ════════════════════════════════════════════════════════════
//  LOOP
// ════════════════════════════════════════════════════════════
void loop() {
  server.handleClient();
  checkButton();
  updateBuzzer();

  // Verificar warm-up do MQ-135
  if (!mq135Ready && (millis() - warmupStart >= WARMUP_MS)) {
    mq135Ready = true;
    Serial.println(F("[MQ-135] Warm-up concluido - leituras confiaveis."));
  }

  // Leitura periódica dos sensores
  unsigned long now = millis();
  if (now - lastRead >= READ_INTERVAL) {
    lastRead = now;
    readSensors();
    updateLEDs();
    updateLCD();

    Serial.printf(
      "[%4lus] T=%.1fC  H=%.0f%%  CO2=%.0fppm  PM2.5=%.1fug/m3  [%s | %s]%s\n",
      current.uptimeS,
      current.temperature,
      current.humidity,
      current.co2Ppm,
      current.pm25UgM3,
      current.status.c_str(),
      current.airQuality.c_str(),
      mq135Ready ? "" : " (warm-up)"
    );
  }

  // Envio periódico ao backend externo
  if (backendUrl.length() > 0 &&
      (millis() - lastBackendPost >= BACKEND_INTERVAL)) {
    lastBackendPost = millis();
    sendToBackend();
  }
}

// ════════════════════════════════════════════════════════════
//  CALCULOS DE SENSORES
// ════════════════════════════════════════════════════════════

// CO2 em ppm — proxy MQ-135 (ADC 0-4095 -> 400-5000 ppm)
// Nota: relação real é logarítmica com calibração em gas de referência.
// Aqui usamos mapeamento linear para simulação (Wokwi / prototipagem).
float calcCO2Ppm(int raw) {
  return constrain(400.0f + ((float)raw / 4095.0f) * 4600.0f, 400.0f, 5000.0f);
}

// PM2.5 em µg/m³ — proxy sensor óptico GP2Y1010 (ADC 0-4095 -> 0-500 µg/m³)
// Baseado na curva de resposta típica do Sharp GP2Y1010AU0F.
float calcPM25UgM3(int raw) {
  return constrain(((float)raw / 4095.0f) * 500.0f, 0.0f, 500.0f);
}

// Índice de Qualidade do Ar — baseado em PM2.5 e CO2
// Referência: padrões EPA/CONAMA adaptados para monitoramento IoT
String calcAirQuality(float co2, float pm25) {
  if (pm25 > 150.0f || co2 > 2000.0f) return "HAZARDOUS";
  if (pm25 >  55.0f || co2 > 1500.0f) return "UNHEALTHY";
  if (pm25 >  35.0f || co2 > 1000.0f) return "MODERATE";
  return "GOOD";
}

// ════════════════════════════════════════════════════════════
//  LEITURA DOS SENSORES
// ════════════════════════════════════════════════════════════
void readSensors() {
  float t = dht.readTemperature();
  float h = dht.readHumidity();
  if (isnan(t)) t = 0.0f;
  if (isnan(h)) h = 0.0f;

  int    co2Raw  = analogRead(MQ135_PIN);
  int    pm25Raw = analogRead(PM25_PIN);
  float  co2Ppm  = calcCO2Ppm(co2Raw);
  float  pm25    = calcPM25UgM3(pm25Raw);
  String aq      = calcAirQuality(co2Ppm, pm25);

  bool alert = (co2Ppm > (float)co2AlertThreshold ||
                pm25   > pm25AlertThreshold        ||
                t      > tempAlertThreshold);

  unsigned long uptime = (millis() - startMillis) / 1000;

  current = { t, h, co2Raw, co2Ppm, pm25Raw, pm25,
              aq, alert, (alert ? "ALERTA" : "NORMAL"), uptime };

  saveToHistory(current);

  if (alert && !buzzerOn) {
    buzzerOn    = true;
    buzzerStart = millis();
    digitalWrite(BUZZER_PIN, HIGH);
  }
}

void saveToHistory(SensorReading& r) {
  history[historyIndex] = r;
  historyIndex = (historyIndex + 1) % 10;
  if (historyCount < 10) historyCount++;
}

// ════════════════════════════════════════════════════════════
//  ENVIO PARA BACKEND EXTERNO
// ════════════════════════════════════════════════════════════
void sendToBackend() {
  if (WiFi.status() != WL_CONNECTED) return;

  HTTPClient http;
  if (!http.begin(backendUrl)) {
    Serial.println("[BACKEND] Falha ao iniciar: " + backendUrl);
    return;
  }
  http.addHeader("Content-Type", "application/json");
  http.setTimeout(5000);

  // Payload com todos os dados de qualidade do ar
  StaticJsonDocument<384> doc;
  doc["device"]      = "CarbonTrace-ESP32";
  doc["timestamp_s"] = current.uptimeS;
  doc["temperature"] = serialized(String(current.temperature, 1));
  doc["humidity"]    = serialized(String(current.humidity,    1));
  doc["co2_ppm"]     = serialized(String(current.co2Ppm,      0));
  doc["pm25_ug_m3"]  = serialized(String(current.pm25UgM3,    1));
  doc["air_quality"] = current.airQuality;
  doc["alert"]       = current.alert;
  doc["status"]      = current.status;

  String payload;
  serializeJson(doc, payload);

  int code = http.POST(payload);
  if (code > 0) {
    Serial.printf("[BACKEND] POST %d -> %s\n", code, backendUrl.c_str());
  } else {
    Serial.printf("[BACKEND] Erro: %s\n", http.errorToString(code).c_str());
  }
  http.end();
}

// ════════════════════════════════════════════════════════════
//  ATUADORES
// ════════════════════════════════════════════════════════════
void updateLEDs() {
  digitalWrite(LED_GREEN, current.alert ? LOW  : HIGH);
  digitalWrite(LED_RED,   current.alert ? HIGH : LOW);
}

void updateBuzzer() {
  if (buzzerOn && (millis() - buzzerStart >= 1000)) {
    buzzerOn = false;
    digitalWrite(BUZZER_PIN, LOW);
  }
}

void updateLCD() {
  lcd.clear();

  // Linha 0: Temperatura e Umidade
  char line0[17];
  snprintf(line0, sizeof(line0), "T:%.1fC  H:%.0f%%",
    current.temperature, current.humidity);
  lcd.setCursor(0, 0);
  lcd.print(line0);

  // Linha 1: CO2 (ppm) e PM2.5 (ug/m3)
  char line1[17];
  snprintf(line1, sizeof(line1), "CO2:%.0f PM:%.0f",
    current.co2Ppm, current.pm25UgM3);
  lcd.setCursor(0, 1);
  lcd.print(line1);
}

// ════════════════════════════════════════════════════════════
//  BOTAO DE RESET
// ════════════════════════════════════════════════════════════
void checkButton() {
  bool state = digitalRead(BTN_PIN);
  if (state == LOW && lastBtnState == HIGH) {
    delay(50);
    if (digitalRead(BTN_PIN) == LOW) resetAlerts();
  }
  lastBtnState = state;
}

void resetAlerts() {
  Serial.println(F("[BTN] Reset de alertas acionado!"));
  historyIndex = 0;
  historyCount = 0;
  digitalWrite(LED_RED,    LOW);
  digitalWrite(LED_GREEN,  HIGH);
  digitalWrite(BUZZER_PIN, LOW);
  buzzerOn = false;

  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Alertas zerados!");
  lcd.setCursor(0, 1);
  lcd.print("Historico limpo");
  delay(1500);

  readSensors();
  updateLEDs();
  updateLCD();
}

// ════════════════════════════════════════════════════════════
//  API — GET /api/sensors
// ════════════════════════════════════════════════════════════
void handleSensors() {
  StaticJsonDocument<384> doc;
  doc["temperature"] = serialized(String(current.temperature, 1));
  doc["humidity"]    = serialized(String(current.humidity,    1));
  doc["co2_raw"]     = current.co2Raw;
  doc["co2_ppm"]     = serialized(String(current.co2Ppm,      0));
  doc["pm25_raw"]    = current.pm25Raw;
  doc["pm25_ug_m3"]  = serialized(String(current.pm25UgM3,    1));
  doc["air_quality"] = current.airQuality;
  doc["alert"]       = current.alert;
  doc["status"]      = current.status;
  doc["led_green"]   = !current.alert;
  doc["led_red"]     = current.alert;
  doc["mq135_ready"] = mq135Ready;
  doc["uptime_s"]    = current.uptimeS;

  String out;
  serializeJson(doc, out);
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.send(200, "application/json", out);
}

// ════════════════════════════════════════════════════════════
//  API — GET /api/status
// ════════════════════════════════════════════════════════════
void handleStatus() {
  StaticJsonDocument<512> doc;
  doc["device"]              = "CarbonTrace-ESP32";
  doc["firmware"]            = "3.0.0";
  doc["wifi_ssid"]           = SSID;
  doc["ip"]                  = WiFi.localIP().toString();
  doc["uptime_s"]            = (millis() - startMillis) / 1000;
  doc["alert"]               = current.alert;
  doc["mq135_ready"]         = mq135Ready;
  doc["co2_threshold_ppm"]   = co2AlertThreshold;
  doc["pm25_threshold_ugm3"] = pm25AlertThreshold;
  doc["temp_threshold_c"]    = tempAlertThreshold;
  doc["backend_url"]         = (backendUrl.length() > 0) ? backendUrl : "(nao configurado)";
  doc["history_count"]       = historyCount;

  String out;
  serializeJson(doc, out);
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.send(200, "application/json", out);
}

// ════════════════════════════════════════════════════════════
//  API — GET /api/air-quality
// ════════════════════════════════════════════════════════════
void handleAirQuality() {
  const char* desc;
  const char* recommendation;
  const char* color;

  if (current.airQuality == "GOOD") {
    desc           = "Qualidade do ar excelente";
    recommendation = "Nenhuma restricao. Atividades ao ar livre recomendadas.";
    color          = "#34d399";
  } else if (current.airQuality == "MODERATE") {
    desc           = "Qualidade do ar moderada";
    recommendation = "Grupos sensiveis devem reduzir exposicao prolongada.";
    color          = "#fbbf24";
  } else if (current.airQuality == "UNHEALTHY") {
    desc           = "Qualidade do ar nao saudavel";
    recommendation = "Evitar atividades ao ar livre. Considere usar mascara.";
    color          = "#f97316";
  } else {
    desc           = "Qualidade do ar perigosa";
    recommendation = "Emergencia ambiental. Permanecer em ambientes fechados.";
    color          = "#ef4444";
  }

  StaticJsonDocument<512> doc;
  doc["air_quality"]          = current.airQuality;
  doc["color_hex"]            = color;
  doc["co2_ppm"]              = serialized(String(current.co2Ppm,      0));
  doc["pm25_ug_m3"]           = serialized(String(current.pm25UgM3,    1));
  doc["temperature_c"]        = serialized(String(current.temperature, 1));
  doc["humidity_pct"]         = serialized(String(current.humidity,    1));
  doc["alert"]                = current.alert;
  doc["description"]          = desc;
  doc["recommendation"]       = recommendation;
  doc["co2_threshold_ppm"]    = co2AlertThreshold;
  doc["pm25_threshold_ugm3"]  = pm25AlertThreshold;
  doc["mq135_ready"]          = mq135Ready;

  String out;
  serializeJson(doc, out);
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.send(200, "application/json", out);
}

// ════════════════════════════════════════════════════════════
//  API — GET /api/history
// ════════════════════════════════════════════════════════════
void handleHistory() {
  StaticJsonDocument<1024> doc;
  doc["total_records"] = historyCount;

  JsonArray hist = doc.createNestedArray("readings");
  int start = (historyCount < 10) ? 0 : historyIndex;

  for (int i = 0; i < historyCount; i++) {
    int idx = (start + i) % 10;
    JsonObject entry = hist.createNestedObject();
    entry["uptime_s"]    = history[idx].uptimeS;
    entry["temp"]        = serialized(String(history[idx].temperature, 1));
    entry["humidity"]    = serialized(String(history[idx].humidity,    1));
    entry["co2_ppm"]     = serialized(String(history[idx].co2Ppm,      0));
    entry["pm25_ug_m3"]  = serialized(String(history[idx].pm25UgM3,    1));
    entry["air_quality"] = history[idx].airQuality;
    entry["status"]      = history[idx].status;
    entry["alert"]       = history[idx].alert;
  }

  String out;
  serializeJson(doc, out);
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.send(200, "application/json", out);
}

// ════════════════════════════════════════════════════════════
//  API — POST /api/config
// ════════════════════════════════════════════════════════════
void handleConfig() {
  if (!server.hasArg("plain")) {
    server.send(400, "application/json", "{\"error\":\"Body JSON obrigatorio\"}");
    return;
  }

  StaticJsonDocument<256> req;
  if (deserializeJson(req, server.arg("plain"))) {
    server.send(400, "application/json", "{\"error\":\"JSON invalido\"}");
    return;
  }

  bool changed = false;
  if (req.containsKey("co2_threshold"))  { co2AlertThreshold  = req["co2_threshold"].as<int>();   changed = true; }
  if (req.containsKey("pm25_threshold")) { pm25AlertThreshold = req["pm25_threshold"].as<float>(); changed = true; }
  if (req.containsKey("temp_threshold")) { tempAlertThreshold = req["temp_threshold"].as<float>(); changed = true; }
  if (req.containsKey("backend_url"))    { backendUrl = req["backend_url"].as<String>();           changed = true; }

  if (!changed) {
    server.send(400, "application/json", "{\"error\":\"Nenhum campo valido\"}");
    return;
  }

  Serial.printf("[CONFIG] CO2=%dppm | PM25=%.1f ug/m3 | Temp=%.1fC | Backend=%s\n",
    co2AlertThreshold, pm25AlertThreshold, tempAlertThreshold,
    backendUrl.length() ? backendUrl.c_str() : "(desativado)");

  StaticJsonDocument<256> res;
  res["message"]             = "Configuracao atualizada";
  res["co2_threshold_ppm"]   = co2AlertThreshold;
  res["pm25_threshold_ugm3"] = pm25AlertThreshold;
  res["temp_threshold_c"]    = tempAlertThreshold;
  res["backend_url"]         = (backendUrl.length() > 0) ? backendUrl : "(desativado)";

  String out;
  serializeJson(res, out);
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.send(200, "application/json", out);
}

// ════════════════════════════════════════════════════════════
//  GET /api/docs
// ════════════════════════════════════════════════════════════
void handleDocs() {
  String html = R"rawhtml(
<!DOCTYPE html><html lang="pt-BR"><head>
<meta charset="UTF-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>CarbonTrace API Docs</title>
<style>
  *{box-sizing:border-box;margin:0;padding:0}
  body{font-family:system-ui,sans-serif;background:#0f1117;color:#e2e8f0;padding:24px;max-width:820px;margin:0 auto}
  h1{font-size:22px;font-weight:600;color:#fff;margin-bottom:4px}
  .sub{font-size:12px;color:#64748b;margin-bottom:32px}
  .ep{background:#1e2130;border:1px solid #2d3748;border-radius:12px;padding:20px;margin-bottom:16px}
  .row{display:flex;align-items:center;gap:12px;margin-bottom:10px}
  .m{padding:3px 10px;border-radius:6px;font-size:12px;font-weight:700;font-family:monospace}
  .get{background:#1e3a5f;color:#60a5fa}.post{background:#3b1f00;color:#fb923c}
  .path{font-family:monospace;font-size:15px;font-weight:600;color:#fff}
  .desc{font-size:13px;color:#94a3b8;margin-bottom:12px}
  .lbl{font-size:11px;font-weight:600;color:#64748b;text-transform:uppercase;letter-spacing:.05em;margin-bottom:6px}
  pre{background:#0f1117;border:1px solid #2d3748;border-radius:8px;padding:12px;font-size:12px;color:#86efac;overflow-x:auto;white-space:pre-wrap}
  .back{display:inline-block;margin-bottom:24px;color:#60a5fa;font-size:13px;text-decoration:none}
  .note{font-size:12px;color:#fb923c;background:#3b1f0033;border:1px solid #fb923c44;border-radius:8px;padding:10px 14px;margin-top:10px}
  .tag{display:inline-block;padding:2px 8px;border-radius:4px;font-size:11px;font-weight:600;margin-right:4px;margin-bottom:4px}
  .tg{background:#065f46;color:#34d399}.tm{background:#78350f;color:#fbbf24}
  .tu{background:#7c2d12;color:#f97316}.th{background:#7f1d1d;color:#ef4444}
</style></head><body>
<a class="back" href="/dashboard">&#8592; voltar ao dashboard</a>
<h1>&#127788; CarbonTrace API — Qualidade do Ar</h1>
<p class="sub">ESP32 WebServer · v3.0.0 · MQ-135 + DHT22 + PM2.5 · FIAP Global Solution 2026/1</p>

<div style="margin-bottom:20px">
  <span class="tag tg">GOOD</span>
  <span class="tag tm">MODERATE</span>
  <span class="tag tu">UNHEALTHY</span>
  <span class="tag th">HAZARDOUS</span>
  <span style="font-size:12px;color:#64748b">— níveis de qualidade do ar</span>
</div>

<div class="ep">
  <div class="row"><span class="m get">GET</span><span class="path">/api/sensors</span></div>
  <p class="desc">Leitura atual de todos os sensores: temperatura, umidade, CO2 (ppm), PM2.5 (µg/m³), qualidade do ar e estado dos atuadores.</p>
  <div class="lbl">Resposta</div>
  <pre>{ "temperature":"27.5","humidity":"72.0","co2_raw":410,"co2_ppm":"861",
  "pm25_raw":328,"pm25_ug_m3":"40.0","air_quality":"MODERATE",
  "alert":false,"status":"NORMAL","led_green":true,"led_red":false,
  "mq135_ready":true,"uptime_s":120 }</pre>
</div>

<div class="ep">
  <div class="row"><span class="m get">GET</span><span class="path">/api/air-quality</span></div>
  <p class="desc">Índice de qualidade do ar com descrição e recomendações para a população.</p>
  <div class="lbl">Resposta</div>
  <pre>{ "air_quality":"MODERATE","color_hex":"#fbbf24",
  "co2_ppm":"861","pm25_ug_m3":"40.0","temperature_c":"27.5","humidity_pct":"72.0",
  "alert":false,"description":"Qualidade do ar moderada",
  "recommendation":"Grupos sensiveis devem reduzir exposicao prolongada.",
  "co2_threshold_ppm":1000,"pm25_threshold_ugm3":35.0,"mq135_ready":true }</pre>
</div>

<div class="ep">
  <div class="row"><span class="m get">GET</span><span class="path">/api/status</span></div>
  <p class="desc">Status do dispositivo, thresholds ativos, backend configurado e contagem do histórico.</p>
  <div class="lbl">Resposta</div>
  <pre>{ "device":"CarbonTrace-ESP32","firmware":"3.0.0","ip":"10.10.0.2",
  "uptime_s":120,"alert":false,"mq135_ready":true,
  "co2_threshold_ppm":1000,"pm25_threshold_ugm3":35.0,"temp_threshold_c":40.0,
  "backend_url":"(nao configurado)","history_count":5 }</pre>
</div>

<div class="ep">
  <div class="row"><span class="m get">GET</span><span class="path">/api/history</span></div>
  <p class="desc">Últimas 10 leituras armazenadas em buffer circular, com timestamp de uptime.</p>
  <div class="lbl">Resposta</div>
  <pre>{ "total_records":3,
  "readings":[
    { "uptime_s":5,"temp":"27.5","humidity":"72.0","co2_ppm":"861",
      "pm25_ug_m3":"40.0","air_quality":"MODERATE","status":"NORMAL","alert":false }
  ]}</pre>
</div>

<div class="ep">
  <div class="row"><span class="m post">POST</span><span class="path">/api/config</span></div>
  <p class="desc">Altera thresholds de alerta e URL do backend em tempo real, sem reiniciar o dispositivo.</p>
  <div class="lbl">Body (JSON) — todos os campos são opcionais individualmente</div>
  <pre>{ "co2_threshold": 800,
  "pm25_threshold": 25.0,
  "temp_threshold": 38.0,
  "backend_url": "http://meu-servidor.com/api/iot/data" }</pre>
  <div class="lbl" style="margin-top:12px">Resposta</div>
  <pre>{ "message":"Configuracao atualizada",
  "co2_threshold_ppm":800,"pm25_threshold_ugm3":25.0,
  "temp_threshold_c":38.0,"backend_url":"http://meu-servidor.com/api/iot/data" }</pre>
  <div class="note">Após configurar backend_url, o ESP32 fará POST automático a cada 30 s com todos os dados dos sensores.</div>
</div>

</body></html>)rawhtml";

  server.send(200, "text/html", html);
}

// ════════════════════════════════════════════════════════════
//  GET /dashboard
// ════════════════════════════════════════════════════════════
void handleDashboard() {
  String html = R"rawhtml(
<!DOCTYPE html><html lang="pt-BR"><head>
<meta charset="UTF-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>CarbonTrace — Qualidade do Ar</title>
<script src="https://cdn.jsdelivr.net/npm/chart.js@4.4.0/dist/chart.umd.min.js"></script>
<style>
  *{box-sizing:border-box;margin:0;padding:0}
  body{font-family:system-ui,sans-serif;background:#0f1117;color:#e2e8f0;min-height:100vh;padding:20px}
  h1{font-size:22px;font-weight:600;margin-bottom:4px;color:#fff}
  .sub{font-size:13px;color:#64748b;margin-bottom:20px}
  .grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(150px,1fr));gap:12px;margin-bottom:24px}
  .card{background:#1e2130;border-radius:12px;padding:16px;border:1px solid #2d3748;transition:border-color .3s,background .3s}
  .card.alert{border-color:#ef4444}.card.ok{border-color:#34d399}
  .card.aqi-good{border-color:#34d399;background:#041f0e}
  .card.aqi-moderate{border-color:#fbbf24;background:#1c1500}
  .card.aqi-unhealthy{border-color:#f97316;background:#1c0d00}
  .card.aqi-hazardous{border-color:#ef4444;background:#1c0000}
  .lbl{font-size:11px;color:#64748b;text-transform:uppercase;letter-spacing:.05em;margin-bottom:6px}
  .val{font-size:28px;font-weight:600;color:#fff}
  .unit{font-size:13px;color:#94a3b8;margin-left:2px}
  .badge{display:inline-block;padding:3px 10px;border-radius:20px;font-size:12px;font-weight:700;margin-top:6px}
  .badge.GOOD{background:#065f46;color:#34d399}
  .badge.MODERATE{background:#78350f;color:#fbbf24}
  .badge.UNHEALTHY{background:#7c2d12;color:#f97316}
  .badge.HAZARDOUS{background:#7f1d1d;color:#ef4444}
  .badge.NORMAL{background:#064e3b;color:#34d399}
  .badge.ALERTA{background:#450a0a;color:#f87171}
  .aqi-main{font-size:22px;font-weight:700;margin-top:4px}
  .aqi-tip{font-size:11px;color:#64748b;margin-top:6px;line-height:1.4}
  .chart-wrap{background:#1e2130;border-radius:12px;padding:16px;border:1px solid #2d3748;margin-bottom:14px}
  .ctitle{font-size:13px;color:#94a3b8;margin-bottom:12px;font-weight:500}
  canvas{max-height:175px}
  .led-row{display:flex;gap:12px;margin-top:8px;align-items:center}
  .led{width:14px;height:14px;border-radius:50%;background:#334155;transition:all .3s}
  .led.on-green{background:#34d399;box-shadow:0 0 8px #34d39988}
  .led.on-red{background:#ef4444;box-shadow:0 0 8px #ef444488}
  .led-lbl{font-size:11px;color:#64748b}
  .warmup{background:#1c1400;border:1px solid #92400e;border-radius:8px;padding:8px 14px;
    font-size:12px;color:#fbbf24;margin-bottom:16px;display:none}
  .cfg-wrap{background:#1e2130;border-radius:12px;padding:16px;border:1px solid #2d3748;margin-bottom:16px}
  .cfg-title{font-size:13px;color:#94a3b8;margin-bottom:14px;font-weight:500}
  .cfg-row{display:grid;grid-template-columns:repeat(auto-fit,minmax(155px,1fr));gap:12px;align-items:end}
  .fld{display:flex;flex-direction:column;gap:4px}
  .fld label{font-size:11px;color:#64748b;text-transform:uppercase;letter-spacing:.05em}
  .fld input{background:#0f1117;border:1px solid #2d3748;color:#e2e8f0;padding:8px 10px;
    border-radius:8px;font-size:13px;outline:none;width:100%}
  .fld input:focus{border-color:#60a5fa}
  .fld.wide{grid-column:1/-1}
  .btn{padding:9px 20px;border-radius:8px;border:none;background:#1e3a5f;
    color:#60a5fa;font-size:13px;font-weight:600;cursor:pointer;align-self:end}
  .btn:hover{background:#2a4a7f}
  .ref{font-size:11px;color:#475569;margin-top:10px;line-height:1.6}
  .msg{font-size:12px;margin-top:8px;color:#34d399;min-height:16px}
  .nav{display:flex;gap:10px;margin-bottom:20px;flex-wrap:wrap}
  .nav a{font-size:13px;color:#60a5fa;text-decoration:none;padding:6px 14px;
    border:1px solid #1e3a5f;border-radius:8px}
  .nav a:hover{background:#1e3a5f}
  footer{font-size:11px;color:#334155;text-align:center;margin-top:24px}
</style>
</head><body>
<h1>&#127788; CarbonTrace — Qualidade do Ar</h1>
<p class="sub">ESP32 · MQ-135 + DHT22 + PM2.5 · FIAP Global Solution 2026/1 · atualiza a cada 5s</p>

<div id="warmup-banner" class="warmup">
  &#9888;&#65039; MQ-135 em warm-up (20s) — leituras iniciais podem ser imprecisas.
</div>

<div class="nav">
  <a href="/api/docs">&#128196; Docs</a>
  <a href="/api/sensors" target="_blank">&#9889; /sensors</a>
  <a href="/api/air-quality" target="_blank">&#127788; /air-quality</a>
  <a href="/api/history" target="_blank">&#128203; /history</a>
</div>

<div class="grid">
  <div class="card" id="aqi-card">
    <div class="lbl">Qualidade do Ar (IQA)</div>
    <div class="aqi-main" id="aqi-val">--</div>
    <div id="aqi-badge"></div>
    <div class="aqi-tip" id="aqi-tip">--</div>
  </div>
  <div class="card">
    <div class="lbl">CO&#8322; estimado (MQ-135)</div>
    <div class="val" id="co2">--<span class="unit">ppm</span></div>
  </div>
  <div class="card">
    <div class="lbl">PM2.5</div>
    <div class="val" id="pm25">--<span class="unit">&#956;g/m&#179;</span></div>
  </div>
  <div class="card">
    <div class="lbl">Temperatura</div>
    <div class="val" id="temp">--<span class="unit">&#176;C</span></div>
  </div>
  <div class="card">
    <div class="lbl">Umidade</div>
    <div class="val" id="hum">--<span class="unit">%</span></div>
  </div>
  <div class="card" id="status-card">
    <div class="lbl">Status</div>
    <div id="status-val">--</div>
    <div class="led-row">
      <div class="led" id="led-green"></div><span class="led-lbl">SAFE</span>
      <div class="led" id="led-red"></div><span class="led-lbl">ALERT</span>
    </div>
  </div>
</div>

<div class="chart-wrap">
  <div class="ctitle">CO&#8322; Estimado (ppm) — MQ-135</div>
  <canvas id="cCO2"></canvas>
</div>
<div class="chart-wrap">
  <div class="ctitle">PM2.5 — Material Particulado (&#956;g/m&#179;)</div>
  <canvas id="cPM25"></canvas>
</div>
<div class="chart-wrap">
  <div class="ctitle">Temperatura (&#176;C) + Umidade (%)</div>
  <canvas id="cTemp"></canvas>
</div>

<div class="cfg-wrap">
  <div class="cfg-title">&#9881;&#65039; Configurar Thresholds e Backend — POST /api/config</div>
  <div class="cfg-row">
    <div class="fld">
      <label>CO&#8322; threshold (ppm)</label>
      <input type="number" id="cfg-co2" placeholder="ex: 1000" min="400" max="5000">
    </div>
    <div class="fld">
      <label>PM2.5 threshold (&#956;g/m&#179;)</label>
      <input type="number" id="cfg-pm25" placeholder="ex: 35" step="0.5" min="0">
    </div>
    <div class="fld">
      <label>Temp. threshold (&#176;C)</label>
      <input type="number" id="cfg-temp" placeholder="ex: 40" step="0.5">
    </div>
    <div class="fld wide">
      <label>Backend URL (POST a cada 30s — deixe vazio para desativar)</label>
      <input type="url" id="cfg-url" placeholder="http://meu-servidor.com/api/iot/data">
    </div>
    <button class="btn" onclick="sendCfg()">Aplicar</button>
  </div>
  <div class="ref">
    Referências EPA/CONAMA:
    CO&#8322; &gt;1000ppm = ar comprometido &nbsp;|&nbsp;
    PM2.5 &gt;35&#956;g/m&#179; = grupos sensíveis &nbsp;|&nbsp;
    PM2.5 &gt;150 = perigoso
  </div>
  <div class="msg" id="cfg-msg"></div>
</div>

<footer>CarbonTrace v3.0 · ESP32 + MQ-135 + DHT22 + PM2.5 · FIAP Global Solution 2026/1</footer>

<script>
const AQI_COLORS={GOOD:'#34d399',MODERATE:'#fbbf24',UNHEALTHY:'#f97316',HAZARDOUS:'#ef4444'};
const AQI_CLASS={GOOD:'aqi-good',MODERATE:'aqi-moderate',UNHEALTHY:'aqi-unhealthy',HAZARDOUS:'aqi-hazardous'};
const AQI_TIPS={
  GOOD:'Ar limpo — atividades ao ar livre recomendadas',
  MODERATE:'Grupos sensíveis devem limitar exposição',
  UNHEALTHY:'Evitar atividades externas prolongadas',
  HAZARDOUS:'Emergência! Permaneça em ambientes fechados'
};

const mkC=(id,lbl,col)=>new Chart(document.getElementById(id),{
  type:'line',
  data:{labels:[],datasets:[{label:lbl,data:[],borderColor:col,backgroundColor:col+'22',
    borderWidth:2,pointRadius:3,tension:.4,fill:true}]},
  options:{responsive:true,animation:false,
    scales:{y:{grid:{color:'#2d3748'},ticks:{color:'#64748b'}},
            x:{grid:{color:'#2d3748'},ticks:{color:'#64748b'}}},
    plugins:{legend:{display:false}}}
});

const cCO2=mkC('cCO2','CO2 ppm','#60a5fa');
const cPM25=mkC('cPM25','PM2.5','#a78bfa');
const cTemp=mkC('cTemp','Temp C','#fb923c');
const MAX=20;

function addPt(ch,l,v){
  ch.data.labels.push(l);ch.data.datasets[0].data.push(v);
  if(ch.data.labels.length>MAX){ch.data.labels.shift();ch.data.datasets[0].data.shift();}
  ch.update();
}

async function fetch5s(){
  try{
    const d=await(await fetch('/api/sensors')).json();
    const l=d.uptime_s+'s';

    document.getElementById('warmup-banner').style.display=d.mq135_ready?'none':'block';

    document.getElementById('co2').innerHTML=d.co2_ppm+'<span class="unit">ppm</span>';
    document.getElementById('pm25').innerHTML=d.pm25_ug_m3+'<span class="unit">&#956;g/m&#179;</span>';
    document.getElementById('temp').innerHTML=d.temperature+'<span class="unit">&#176;C</span>';
    document.getElementById('hum').innerHTML=d.humidity+'<span class="unit">%</span>';

    const ac=document.getElementById('aqi-card');
    ac.className='card '+(AQI_CLASS[d.air_quality]||'');
    const av=document.getElementById('aqi-val');
    av.textContent=d.air_quality;
    av.style.color=AQI_COLORS[d.air_quality]||'#fff';
    document.getElementById('aqi-badge').innerHTML=
      '<span class="badge '+d.air_quality+'">'+d.air_quality+'</span>';
    document.getElementById('aqi-tip').textContent=AQI_TIPS[d.air_quality]||'';

    document.getElementById('status-val').innerHTML=
      '<span class="badge '+d.status+'">'+d.status+'</span>';
    document.getElementById('status-card').className='card '+(d.alert?'alert':'ok');
    document.getElementById('led-green').className='led '+(d.led_green?'on-green':'');
    document.getElementById('led-red').className='led '+(d.led_red?'on-red':'');

    addPt(cCO2,l,parseFloat(d.co2_ppm));
    addPt(cPM25,l,parseFloat(d.pm25_ug_m3));
    addPt(cTemp,l,parseFloat(d.temperature));
  }catch(e){console.warn('fetch',e);}
}

async function sendCfg(){
  const co2=document.getElementById('cfg-co2').value;
  const pm25=document.getElementById('cfg-pm25').value;
  const tmp=document.getElementById('cfg-temp').value;
  const url=document.getElementById('cfg-url').value.trim();
  const msg=document.getElementById('cfg-msg');
  if(!co2&&!pm25&&!tmp&&!url){msg.style.color='#ef4444';msg.textContent='Preencha ao menos um campo.';return;}
  const body={};
  if(co2)body.co2_threshold=parseInt(co2);
  if(pm25)body.pm25_threshold=parseFloat(pm25);
  if(tmp)body.temp_threshold=parseFloat(tmp);
  if(url)body.backend_url=url;
  try{
    const d=await(await fetch('/api/config',{method:'POST',
      headers:{'Content-Type':'application/json'},body:JSON.stringify(body)})).json();
    msg.style.color='#34d399';
    msg.textContent='&#10003; '+d.message;
  }catch(e){msg.style.color='#ef4444';msg.textContent='Erro ao enviar.';}
}

fetch5s();setInterval(fetch5s,5000);
</script>
</body></html>)rawhtml";

  server.send(200, "text/html", html);
}

// ════════════════════════════════════════════════════════════
//  404
// ════════════════════════════════════════════════════════════
void handleNotFound() {
  StaticJsonDocument<128> doc;
  doc["error"]     = "Endpoint nao encontrado";
  doc["endpoints"] = "/dashboard | /api/sensors | /api/status | /api/air-quality | /api/history | /api/config | /api/docs";
  String out;
  serializeJson(doc, out);
  server.send(404, "application/json", out);
}
