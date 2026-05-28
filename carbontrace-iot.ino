// ============================================================
//  CarbonTrace v2.0 — ESP32 IoT Prototype
//  FIAP Global Solution 2026/1 | Disruptive Architectures
//
//  Entradas : DHT22 (GPIO4) | LDR (GPIO34) | Botão Reset (GPIO14)
//  Saidas   : LED Verde (GPIO18) | LED Vermelho (GPIO19) | Buzzer (GPIO23)
//  Interface: LCD 16x2 I2C (SDA=21 / SCL=22)
//  Rede     : Wi-Fi + WebServer porta 80
//  Endpoints:
//    GET  /api/sensors  — leitura atual
//    GET  /api/status   — status do dispositivo
//    GET  /api/carbon   — estimativa de CO2
//    GET  /api/history  — historico de leituras
//    POST /api/config   — altera thresholds dinamicamente
//    GET  /api/docs     — documentacao da API
//    GET  /dashboard    — painel HTML
// ============================================================

#include <WiFi.h>
#include <WebServer.h>
#include <DHT.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <ArduinoJson.h>

// ── Credenciais Wi-Fi ────────────────────────────────────────
const char* SSID     = "Wokwi-GUEST";
const char* PASSWORD = "";

// ── Pinagem ─────────────────────────────────────────────────
#define DHT_PIN      4
#define DHT_TYPE     DHT22
#define LDR_PIN      34
#define BTN_PIN      14
#define LED_GREEN    18
#define LED_RED      19
#define BUZZER_PIN   23

// ── Thresholds (alteráveis via POST /api/config) ─────────────
int   ldrAlertThreshold  = 2500;
float tempAlertThreshold = 35.0;

// ── Objetos globais ──────────────────────────────────────────
DHT dht(DHT_PIN, DHT_TYPE);
LiquidCrystal_I2C lcd(0x27, 16, 2);
WebServer server(80);

// ── Estrutura de leitura ─────────────────────────────────────
struct SensorReading {
  float temperature;
  float humidity;
  int   ldrRaw;
  float coveragePct;
  float co2Estimate;
  bool  alert;
  String status;
  unsigned long uptimeS;
};

// Histórico circular (últimas 10 leituras)
SensorReading history[10];
int historyIndex = 0;
int historyCount = 0;

SensorReading current;
unsigned long lastRead    = 0;
unsigned long startMillis = 0;
const int READ_INTERVAL   = 5000;

// Controle do botão
bool lastBtnState = HIGH;

// Controle do buzzer
unsigned long buzzerStart = 0;
bool buzzerOn = false;

// ── Protótipos ───────────────────────────────────────────────
void readSensors();
void updateLEDs();
void updateLCD();
void updateBuzzer();
void checkButton();
void resetAlerts();
void saveToHistory(SensorReading& r);
float calcCoverage(int ldrRaw);
float calcCO2(float coveragePct);
void handleDashboard();
void handleSensors();
void handleStatus();
void handleCarbon();
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
  lcd.print("CarbonTrace v2.0");
  lcd.setCursor(0, 1);
  lcd.print("Conectando...");

  Serial.println("\n=============================");
  Serial.println("  CarbonTrace ESP32 v2.0");
  Serial.println("  FIAP Global Solution 2026");
  Serial.println("=============================");

  WiFi.begin(SSID, PASSWORD);
  Serial.print("Conectando ao Wi-Fi");
  int tries = 0;
  while (WiFi.status() != WL_CONNECTED && tries < 20) {
    delay(500);
    Serial.print(".");
    tries++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWi-Fi conectado!");
    Serial.println("IP: " + WiFi.localIP().toString());
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("IP:");
    lcd.setCursor(0, 1);
    lcd.print(WiFi.localIP().toString());
    delay(2000);
  } else {
    Serial.println("\nSem Wi-Fi — modo offline");
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("Modo Offline");
  }

  // Rotas
  server.on("/dashboard",   HTTP_GET,  handleDashboard);
  server.on("/api/sensors", HTTP_GET,  handleSensors);
  server.on("/api/status",  HTTP_GET,  handleStatus);
  server.on("/api/carbon",  HTTP_GET,  handleCarbon);
  server.on("/api/history", HTTP_GET,  handleHistory);
  server.on("/api/config",  HTTP_POST, handleConfig);
  server.on("/api/docs",    HTTP_GET,  handleDocs);
  server.onNotFound(handleNotFound);
  server.begin();

  Serial.println("WebServer iniciado na porta 80");
  Serial.println("-----------------------------");
  Serial.println("Endpoints disponíveis:");
  Serial.println("  GET  /dashboard");
  Serial.println("  GET  /api/sensors");
  Serial.println("  GET  /api/status");
  Serial.println("  GET  /api/carbon");
  Serial.println("  GET  /api/history");
  Serial.println("  POST /api/config");
  Serial.println("  GET  /api/docs");
  Serial.println("-----------------------------");

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

  unsigned long now = millis();
  if (now - lastRead >= READ_INTERVAL) {
    lastRead = now;
    readSensors();
    updateLEDs();
    updateLCD();

    Serial.printf("[%3lus] T=%.1f°C H=%.1f%% LDR=%4d Cob=%.0f%% CO2=%.2ft/ha [%s]\n",
      current.uptimeS,
      current.temperature,
      current.humidity,
      current.ldrRaw,
      current.coveragePct,
      current.co2Estimate,
      current.status.c_str()
    );
  }
}

// ════════════════════════════════════════════════════════════
//  SENSORES
// ════════════════════════════════════════════════════════════
void readSensors() {
  float t = dht.readTemperature();
  float h = dht.readHumidity();

  if (isnan(t)) t = 0.0;
  if (isnan(h)) h = 0.0;

  int   ldr      = analogRead(LDR_PIN);
  float coverage = calcCoverage(ldr);
  float co2      = calcCO2(coverage);
  bool  alert    = (ldr > ldrAlertThreshold || t > tempAlertThreshold);
  String status  = alert ? "ALERTA" : "NORMAL";
  unsigned long uptime = (millis() - startMillis) / 1000;

  current = { t, h, ldr, coverage, co2, alert, status, uptime };
  saveToHistory(current);

  if (alert && !buzzerOn) {
    buzzerOn    = true;
    buzzerStart = millis();
    digitalWrite(BUZZER_PIN, HIGH);
  }
}

float calcCoverage(int ldrRaw) {
  float pct = 100.0 - ((float)ldrRaw / 4095.0 * 100.0);
  return constrain(pct, 0.0, 100.0);
}

float calcCO2(float coveragePct) {
  return (1.0 - coveragePct / 100.0) * 150.0;
}

void saveToHistory(SensorReading& r) {
  history[historyIndex] = r;
  historyIndex = (historyIndex + 1) % 10;
  if (historyCount < 10) historyCount++;
}

// ════════════════════════════════════════════════════════════
//  ATUADORES
// ════════════════════════════════════════════════════════════
void updateLEDs() {
  digitalWrite(LED_GREEN, !current.alert ? HIGH : LOW);
  digitalWrite(LED_RED,    current.alert ? HIGH : LOW);
}

void updateBuzzer() {
  if (buzzerOn && millis() - buzzerStart >= 1000) {
    buzzerOn = false;
    digitalWrite(BUZZER_PIN, LOW);
  }
}

void updateLCD() {
  lcd.clear();

  char line0[17];
  snprintf(line0, sizeof(line0), "T:%.1fC H:%.0f%%",
    current.temperature, current.humidity);
  lcd.setCursor(0, 0);
  lcd.print(line0);

  char line1[17];
  snprintf(line1, sizeof(line1), "Cob:%.0f%% %s",
    current.coveragePct,
    current.alert ? "!ALERT" : "OK    ");
  lcd.setCursor(0, 1);
  lcd.print(line1);
}

// ════════════════════════════════════════════════════════════
//  BOTÃO DE RESET
// ════════════════════════════════════════════════════════════
void checkButton() {
  bool btnState = digitalRead(BTN_PIN);
  if (btnState == LOW && lastBtnState == HIGH) {
    delay(50);
    if (digitalRead(BTN_PIN) == LOW) {
      resetAlerts();
    }
  }
  lastBtnState = btnState;
}

void resetAlerts() {
  Serial.println("[BTN] Reset de alertas acionado!");

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
//  ENDPOINTS DA API
// ════════════════════════════════════════════════════════════

// GET /api/sensors
void handleSensors() {
  StaticJsonDocument<256> doc;
  doc["temperature"]  = serialized(String(current.temperature, 1));
  doc["humidity"]     = serialized(String(current.humidity, 1));
  doc["ldr_raw"]      = current.ldrRaw;
  doc["coverage_pct"] = serialized(String(current.coveragePct, 1));
  doc["co2_ton_ha"]   = serialized(String(current.co2Estimate, 2));
  doc["alert"]        = current.alert;
  doc["status"]       = current.status;
  doc["led_green"]    = !current.alert;
  doc["led_red"]      = current.alert;
  doc["uptime_s"]     = current.uptimeS;

  String out;
  serializeJson(doc, out);
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.send(200, "application/json", out);
}

// GET /api/status
void handleStatus() {
  StaticJsonDocument<512> doc;
  doc["device"]           = "CarbonTrace-ESP32";
  doc["firmware"]         = "2.0.0";
  doc["wifi_ssid"]        = SSID;
  doc["ip"]               = WiFi.localIP().toString();
  doc["uptime_s"]         = (millis() - startMillis) / 1000;
  doc["alert"]            = current.alert;
  doc["ldr_threshold"]    = ldrAlertThreshold;
  doc["temp_threshold_c"] = tempAlertThreshold;
  doc["history_count"]    = historyCount;

  String out;
  serializeJson(doc, out);
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.send(200, "application/json", out);
}

// GET /api/carbon
void handleCarbon() {
  StaticJsonDocument<256> doc;

  String riskLevel;
  if      (current.coveragePct >= 70) riskLevel = "LOW";
  else if (current.coveragePct >= 40) riskLevel = "MEDIUM";
  else                                 riskLevel = "HIGH";

  doc["coverage_pct"]         = serialized(String(current.coveragePct, 1));
  doc["estimated_co2_ton_ha"] = serialized(String(current.co2Estimate, 2));
  doc["risk_level"]           = riskLevel;
  doc["ldr_raw"]              = current.ldrRaw;
  doc["threshold_pct"]        = 70;
  doc["description"]          = "Estimativa baseada em LDR como proxy de cobertura vegetal";

  String out;
  serializeJson(doc, out);
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.send(200, "application/json", out);
}

// GET /api/history
void handleHistory() {
  StaticJsonDocument<1024> doc;
  doc["total_records"] = historyCount;

  JsonArray hist = doc.createNestedArray("readings");
  int start = (historyCount < 10) ? 0 : historyIndex;
  for (int i = 0; i < historyCount; i++) {
    int idx = (start + i) % 10;
    JsonObject entry = hist.createNestedObject();
    entry["uptime_s"] = history[idx].uptimeS;
    entry["temp"]     = serialized(String(history[idx].temperature, 1));
    entry["humidity"] = serialized(String(history[idx].humidity, 1));
    entry["ldr"]      = history[idx].ldrRaw;
    entry["coverage"] = serialized(String(history[idx].coveragePct, 1));
    entry["co2"]      = serialized(String(history[idx].co2Estimate, 2));
    entry["status"]   = history[idx].status;
    entry["alert"]    = history[idx].alert;
  }

  String out;
  serializeJson(doc, out);
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.send(200, "application/json", out);
}

// POST /api/config
void handleConfig() {
  if (!server.hasArg("plain")) {
    server.send(400, "application/json", "{\"error\":\"Body JSON obrigatorio\"}");
    return;
  }

  StaticJsonDocument<128> req;
  DeserializationError err = deserializeJson(req, server.arg("plain"));
  if (err) {
    server.send(400, "application/json", "{\"error\":\"JSON invalido\"}");
    return;
  }

  bool changed = false;
  if (req.containsKey("ldr_threshold")) {
    ldrAlertThreshold = req["ldr_threshold"].as<int>();
    changed = true;
  }
  if (req.containsKey("temp_threshold")) {
    tempAlertThreshold = req["temp_threshold"].as<float>();
    changed = true;
  }

  if (!changed) {
    server.send(400, "application/json", "{\"error\":\"Nenhum campo valido enviado\"}");
    return;
  }

  Serial.printf("[CONFIG] Novo threshold LDR=%d | Temp=%.1f\n",
    ldrAlertThreshold, tempAlertThreshold);

  StaticJsonDocument<128> res;
  res["message"]        = "Configuracao atualizada";
  res["ldr_threshold"]  = ldrAlertThreshold;
  res["temp_threshold"] = tempAlertThreshold;

  String out;
  serializeJson(res, out);
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.send(200, "application/json", out);
}

// GET /api/docs
void handleDocs() {
  String html = R"rawhtml(
<!DOCTYPE html>
<html lang="pt-BR">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>CarbonTrace API Docs</title>
<style>
  *{box-sizing:border-box;margin:0;padding:0}
  body{font-family:system-ui,sans-serif;background:#0f1117;color:#e2e8f0;padding:24px;max-width:800px;margin:0 auto}
  h1{font-size:22px;font-weight:600;color:#fff;margin-bottom:4px}
  .version{font-size:12px;color:#64748b;margin-bottom:32px}
  .endpoint{background:#1e2130;border:1px solid #2d3748;border-radius:12px;padding:20px;margin-bottom:16px}
  .method-row{display:flex;align-items:center;gap:12px;margin-bottom:10px}
  .method{padding:3px 10px;border-radius:6px;font-size:12px;font-weight:700;font-family:monospace}
  .get{background:#1e3a5f;color:#60a5fa}
  .post{background:#3b1f00;color:#fb923c}
  .path{font-family:monospace;font-size:15px;font-weight:600;color:#fff}
  .desc{font-size:13px;color:#94a3b8;margin-bottom:12px}
  .label{font-size:11px;font-weight:600;color:#64748b;text-transform:uppercase;letter-spacing:.05em;margin-bottom:6px}
  pre{background:#0f1117;border:1px solid #2d3748;border-radius:8px;padding:12px;font-size:12px;color:#86efac;overflow-x:auto;white-space:pre-wrap}
  .back{display:inline-block;margin-bottom:24px;color:#60a5fa;font-size:13px;text-decoration:none}
  .note{font-size:12px;color:#fb923c;background:#3b1f0033;border:1px solid #fb923c44;border-radius:8px;padding:10px 14px;margin-top:10px}
</style>
</head>
<body>
<a class="back" href="/dashboard">← voltar ao dashboard</a>
<h1>🌿 CarbonTrace API</h1>
<p class="version">ESP32 WebServer · v2.0.0 · FIAP Global Solution 2026/1</p>

<div class="endpoint">
  <div class="method-row"><span class="method get">GET</span><span class="path">/api/sensors</span></div>
  <p class="desc">Leitura atual de todos os sensores e estado dos atuadores.</p>
  <div class="label">Resposta</div>
  <pre>{
  "temperature": "27.4",
  "humidity": "68.2",
  "ldr_raw": 1820,
  "coverage_pct": "55.6",
  "co2_ton_ha": "66.60",
  "alert": false,
  "status": "NORMAL",
  "led_green": true,
  "led_red": false,
  "uptime_s": 120
}</pre>
</div>

<div class="endpoint">
  <div class="method-row"><span class="method get">GET</span><span class="path">/api/status</span></div>
  <p class="desc">Status do dispositivo e configurações ativas.</p>
  <div class="label">Resposta</div>
  <pre>{
  "device": "CarbonTrace-ESP32",
  "firmware": "2.0.0",
  "ip": "10.10.0.2",
  "uptime_s": 120,
  "alert": false,
  "ldr_threshold": 2500,
  "temp_threshold_c": 35.0,
  "history_count": 5
}</pre>
</div>

<div class="endpoint">
  <div class="method-row"><span class="method get">GET</span><span class="path">/api/carbon</span></div>
  <p class="desc">Estimativa de emissão de carbono baseada na cobertura vegetal.</p>
  <div class="label">Resposta</div>
  <pre>{
  "coverage_pct": "55.6",
  "estimated_co2_ton_ha": "66.60",
  "risk_level": "MEDIUM",
  "ldr_raw": 1820,
  "threshold_pct": 70,
  "description": "Estimativa baseada em LDR como proxy de cobertura vegetal"
}</pre>
</div>

<div class="endpoint">
  <div class="method-row"><span class="method get">GET</span><span class="path">/api/history</span></div>
  <p class="desc">Últimas 10 leituras armazenadas em memória com timestamp de uptime.</p>
  <div class="label">Resposta</div>
  <pre>{
  "total_records": 3,
  "readings": [
    { "uptime_s": 5, "temp": "27.4", "humidity": "68.0",
      "ldr": 1820, "coverage": "55.5", "co2": "66.75",
      "status": "NORMAL", "alert": false }
  ]
}</pre>
</div>

<div class="endpoint">
  <div class="method-row"><span class="method post">POST</span><span class="path">/api/config</span></div>
  <p class="desc">Altera os thresholds de alerta em tempo real sem reiniciar o dispositivo.</p>
  <div class="label">Body (JSON)</div>
  <pre>{
  "ldr_threshold": 2000,
  "temp_threshold": 32.0
}</pre>
  <div class="label" style="margin-top:12px">Resposta</div>
  <pre>{
  "message": "Configuracao atualizada",
  "ldr_threshold": 2000,
  "temp_threshold": 32.0
}</pre>
  <div class="note">Envie ao menos um dos campos. Ambos são opcionais individualmente.</div>
</div>

</body>
</html>
)rawhtml";

  server.send(200, "text/html", html);
}

// GET /dashboard
void handleDashboard() {
  String html = R"rawhtml(
<!DOCTYPE html>
<html lang="pt-BR">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>CarbonTrace Dashboard</title>
<script src="https://cdn.jsdelivr.net/npm/chart.js@4.4.0/dist/chart.umd.min.js"></script>
<style>
  *{box-sizing:border-box;margin:0;padding:0}
  body{font-family:system-ui,sans-serif;background:#0f1117;color:#e2e8f0;min-height:100vh;padding:20px}
  h1{font-size:22px;font-weight:600;margin-bottom:4px;color:#fff}
  .sub{font-size:13px;color:#64748b;margin-bottom:24px}
  .grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(160px,1fr));gap:12px;margin-bottom:24px}
  .card{background:#1e2130;border-radius:12px;padding:16px;border:1px solid #2d3748;transition:border-color .3s}
  .card.alert{border-color:#f87171}
  .card.ok{border-color:#34d399}
  .label{font-size:11px;color:#64748b;text-transform:uppercase;letter-spacing:.05em;margin-bottom:6px}
  .value{font-size:28px;font-weight:600;color:#fff}
  .unit{font-size:13px;color:#94a3b8;margin-left:2px}
  .badge{display:inline-block;padding:3px 10px;border-radius:20px;font-size:12px;font-weight:600;margin-top:8px}
  .badge.NORMAL{background:#064e3b;color:#34d399}
  .badge.ALERTA{background:#450a0a;color:#f87171}
  .chart-wrap{background:#1e2130;border-radius:12px;padding:16px;border:1px solid #2d3748;margin-bottom:16px}
  .chart-title{font-size:13px;color:#94a3b8;margin-bottom:12px;font-weight:500}
  canvas{max-height:180px}
  .led-row{display:flex;gap:12px;margin-top:8px;align-items:center}
  .led{width:14px;height:14px;border-radius:50%;background:#334155;transition:all .3s}
  .led.on-green{background:#34d399;box-shadow:0 0 8px #34d39988}
  .led.on-red{background:#f87171;box-shadow:0 0 8px #f8717188}
  .led-label{font-size:11px;color:#64748b}
  .config-wrap{background:#1e2130;border-radius:12px;padding:16px;border:1px solid #2d3748;margin-bottom:16px}
  .config-title{font-size:13px;color:#94a3b8;margin-bottom:14px;font-weight:500}
  .config-row{display:flex;gap:12px;align-items:flex-end;flex-wrap:wrap}
  .config-field{display:flex;flex-direction:column;gap:4px;flex:1;min-width:140px}
  .config-field label{font-size:11px;color:#64748b;text-transform:uppercase;letter-spacing:.05em}
  .config-field input{background:#0f1117;border:1px solid #2d3748;color:#e2e8f0;padding:8px 10px;border-radius:8px;font-size:13px;outline:none}
  .config-field input:focus{border-color:#60a5fa}
  .btn{padding:8px 18px;border-radius:8px;border:none;background:#1e3a5f;color:#60a5fa;font-size:13px;font-weight:600;cursor:pointer}
  .btn:hover{background:#2a4a7f}
  .msg{font-size:12px;margin-top:8px;color:#34d399;min-height:16px}
  .nav{display:flex;gap:12px;margin-bottom:20px;flex-wrap:wrap}
  .nav a{font-size:13px;color:#60a5fa;text-decoration:none;padding:6px 14px;border:1px solid #1e3a5f;border-radius:8px}
  .nav a:hover{background:#1e3a5f}
  footer{font-size:11px;color:#334155;text-align:center;margin-top:24px}
</style>
</head>
<body>
<h1>🌿 CarbonTrace Dashboard</h1>
<p class="sub">ESP32 IoT · FIAP Global Solution 2026/1 · atualiza a cada 5s</p>

<div class="nav">
  <a href="/api/docs">📄 API Docs</a>
  <a href="/api/sensors" target="_blank">⚡ /api/sensors</a>
  <a href="/api/history" target="_blank">📋 /api/history</a>
  <a href="/api/carbon"  target="_blank">🌱 /api/carbon</a>
</div>

<div class="grid">
  <div class="card"><div class="label">Temperatura</div><div class="value" id="temp">--<span class="unit">°C</span></div></div>
  <div class="card"><div class="label">Umidade</div><div class="value" id="hum">--<span class="unit">%</span></div></div>
  <div class="card"><div class="label">LDR (bruto)</div><div class="value" id="ldr">--</div></div>
  <div class="card"><div class="label">Cobertura vegetal</div><div class="value" id="cov">--<span class="unit">%</span></div></div>
  <div class="card"><div class="label">CO₂ estimado</div><div class="value" id="co2">--<span class="unit">t/ha</span></div></div>
  <div class="card" id="status-card">
    <div class="label">Status</div>
    <div id="status-val">--</div>
    <div class="led-row">
      <div class="led" id="led-green"></div><span class="led-label">SAFE</span>
      <div class="led" id="led-red"></div><span class="led-label">ALERT</span>
    </div>
  </div>
</div>

<div class="chart-wrap">
  <div class="chart-title">Temperatura (°C)</div>
  <canvas id="chartTemp"></canvas>
</div>
<div class="chart-wrap">
  <div class="chart-title">Cobertura vegetal (%)</div>
  <canvas id="chartCov"></canvas>
</div>
<div class="chart-wrap">
  <div class="chart-title">CO₂ estimado (ton/ha)</div>
  <canvas id="chartCO2"></canvas>
</div>

<div class="config-wrap">
  <div class="config-title">⚙️ Configurar thresholds — POST /api/config</div>
  <div class="config-row">
    <div class="config-field">
      <label>LDR threshold (0–4095)</label>
      <input type="number" id="cfg-ldr" placeholder="ex: 2500" min="0" max="4095">
    </div>
    <div class="config-field">
      <label>Temperatura threshold (°C)</label>
      <input type="number" id="cfg-temp" placeholder="ex: 35" step="0.5">
    </div>
    <button class="btn" onclick="sendConfig()">Aplicar</button>
  </div>
  <div class="msg" id="cfg-msg"></div>
</div>

<footer>CarbonTrace v2.0 · ESP32 WebServer · Global Solution FIAP 2026/1</footer>

<script>
const mkChart = (id, label, color) => new Chart(document.getElementById(id), {
  type: 'line',
  data: {
    labels: [],
    datasets: [{
      label, data: [],
      borderColor: color,
      backgroundColor: color + '22',
      borderWidth: 2, pointRadius: 3, tension: 0.4, fill: true
    }]
  },
  options: {
    responsive: true, animation: false,
    scales: {
      y: { grid: { color: '#2d3748' }, ticks: { color: '#64748b' } },
      x: { grid: { color: '#2d3748' }, ticks: { color: '#64748b' } }
    },
    plugins: { legend: { display: false } }
  }
});

const chartTemp = mkChart('chartTemp', 'Temperatura', '#60a5fa');
const chartCov  = mkChart('chartCov',  'Cobertura',   '#34d399');
const chartCO2  = mkChart('chartCO2',  'CO2',         '#fb923c');
const MAX_PTS   = 20;

function addPoint(chart, label, value) {
  chart.data.labels.push(label);
  chart.data.datasets[0].data.push(value);
  if (chart.data.labels.length > MAX_PTS) {
    chart.data.labels.shift();
    chart.data.datasets[0].data.shift();
  }
  chart.update();
}

async function fetchData() {
  try {
    const r = await fetch('/api/sensors');
    const d = await r.json();
    const lbl = d.uptime_s + 's';

    document.getElementById('temp').innerHTML   = d.temperature  + '<span class="unit">°C</span>';
    document.getElementById('hum').innerHTML    = d.humidity     + '<span class="unit">%</span>';
    document.getElementById('ldr').textContent  = d.ldr_raw;
    document.getElementById('cov').innerHTML    = d.coverage_pct + '<span class="unit">%</span>';
    document.getElementById('co2').innerHTML    = d.co2_ton_ha   + '<span class="unit">t/ha</span>';
    document.getElementById('status-val').innerHTML =
      '<span class="badge ' + d.status + '">' + d.status + '</span>';
    document.getElementById('status-card').className = 'card ' + (d.alert ? 'alert' : 'ok');
    document.getElementById('led-green').className = 'led ' + (d.led_green ? 'on-green' : '');
    document.getElementById('led-red').className   = 'led ' + (d.led_red   ? 'on-red'   : '');

    addPoint(chartTemp, lbl, parseFloat(d.temperature));
    addPoint(chartCov,  lbl, parseFloat(d.coverage_pct));
    addPoint(chartCO2,  lbl, parseFloat(d.co2_ton_ha));
  } catch(e) { console.warn('fetch error', e); }
}

async function sendConfig() {
  const ldr  = document.getElementById('cfg-ldr').value;
  const temp = document.getElementById('cfg-temp').value;
  const msg  = document.getElementById('cfg-msg');

  if (!ldr && !temp) {
    msg.style.color = '#f87171';
    msg.textContent = 'Preencha ao menos um campo.';
    return;
  }

  const body = {};
  if (ldr)  body.ldr_threshold  = parseInt(ldr);
  if (temp) body.temp_threshold = parseFloat(temp);

  try {
    const r = await fetch('/api/config', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(body)
    });
    const d = await r.json();
    msg.style.color = '#34d399';
    msg.textContent = '✓ ' + d.message + ' — LDR: ' + d.ldr_threshold + ' | Temp: ' + d.temp_threshold + '°C';
  } catch(e) {
    msg.style.color = '#f87171';
    msg.textContent = 'Erro ao enviar configuração.';
  }
}

fetchData();
setInterval(fetchData, 5000);
</script>
</body>
</html>
)rawhtml";

  server.send(200, "text/html", html);
}

// 404
void handleNotFound() {
  StaticJsonDocument<128> doc;
  doc["error"]     = "Endpoint nao encontrado";
  doc["endpoints"] = "/dashboard | /api/sensors | /api/status | /api/carbon | /api/history | /api/config | /api/docs";
  String out;
  serializeJson(doc, out);
  server.send(404, "application/json", out);
}