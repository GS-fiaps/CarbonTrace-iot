// ============================================================
//  CarbonTrace — ESP32 IoT Prototype
//  FIAP Global Solution 2026/1 | Disruptive Architectures
//
//  Entradas : DHT22 (GPIO4)  |  LDR (GPIO34)
//  Saidas   : LED Verde (GPIO18)  |  LED Vermelho (GPIO19)
//  Interface: LCD 16x2 I2C (SDA=21 / SCL=22)
//  Rede     : Wi-Fi + WebServer porta 80
//  Endpoints: /api/sensors  |  /api/status  |  /api/carbon
//             /dashboard (HTML)
// ============================================================

#include <WiFi.h>
#include <WebServer.h>
#include <DHT.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <ArduinoJson.h>

// ── Credenciais Wi-Fi ────────────────────────────────────────
// Wokwi usa "Wokwi-GUEST" sem senha
// Hardware real: troque pelas suas credenciais
const char* SSID     = "Wokwi-GUEST";
const char* PASSWORD = "";

// ── Pinagem ─────────────────────────────────────────────────
#define DHT_PIN      4
#define DHT_TYPE     DHT22
#define LDR_PIN      34
#define LED_GREEN    18
#define LED_RED      19

// ── Limites de alerta ────────────────────────────────────────
// LDR: quanto MAIOR o valor → mais luz → menos dossel → risco
#define LDR_ALERT_THRESHOLD  2500
#define TEMP_ALERT_THRESHOLD 35.0

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
  unsigned long timestamp;
};

// Histórico circular (últimas 10 leituras)
SensorReading history[10];
int historyIndex = 0;
int historyCount = 0;

SensorReading current;
unsigned long lastRead    = 0;
unsigned long startMillis = 0;
const int READ_INTERVAL   = 5000;

// ── Protótipos ───────────────────────────────────────────────
void readSensors();
void updateLEDs();
void updateLCD();
void saveToHistory(SensorReading& r);
float calcCoverage(int ldrRaw);
float calcCO2(float coveragePct);
void handleDashboard();
void handleSensors();
void handleStatus();
void handleCarbon();
void handleNotFound();

// ════════════════════════════════════════════════════════════
//  SETUP
// ════════════════════════════════════════════════════════════
void setup() {
  Serial.begin(115200);
  delay(100);

  dht.begin();
  pinMode(LED_GREEN, OUTPUT);
  pinMode(LED_RED,   OUTPUT);

  Wire.begin(21, 22);
  lcd.init();
  lcd.backlight();
  lcd.setCursor(0, 0);
  lcd.print("CarbonTrace v1.0");
  lcd.setCursor(0, 1);
  lcd.print("Conectando...");

  Serial.println("\n== CarbonTrace ESP32 ==");

  WiFi.begin(SSID, PASSWORD);
  Serial.print("Conectando ao Wi-Fi");
  int tries = 0;
  while (WiFi.status() != WL_CONNECTED && tries < 20) {
    delay(500);
    Serial.print(".");
    tries++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nConectado! IP: " + WiFi.localIP().toString());
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

  server.on("/dashboard",   HTTP_GET, handleDashboard);
  server.on("/api/sensors", HTTP_GET, handleSensors);
  server.on("/api/status",  HTTP_GET, handleStatus);
  server.on("/api/carbon",  HTTP_GET, handleCarbon);
  server.onNotFound(handleNotFound);
  server.begin();
  Serial.println("WebServer iniciado na porta 80");

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

  unsigned long now = millis();
  if (now - lastRead >= READ_INTERVAL) {
    lastRead = now;
    readSensors();
    updateLEDs();
    updateLCD();

    Serial.printf("[%lus] Temp=%.1f°C Hum=%.1f%% LDR=%d Cover=%.0f%% CO2=%.2f t/ha Status=%s\n",
      (now - startMillis) / 1000,
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

  int ldr        = analogRead(LDR_PIN);
  float coverage = calcCoverage(ldr);
  float co2      = calcCO2(coverage);
  bool  alert    = (ldr > LDR_ALERT_THRESHOLD || t > TEMP_ALERT_THRESHOLD);
  String status  = alert ? "ALERTA" : "NORMAL";

  current = { t, h, ldr, coverage, co2, alert, status, millis() };
  saveToHistory(current);
}

float calcCoverage(int ldrRaw) {
  // LDR 0-4095 → invertido e mapeado para 0-100%
  float pct = 100.0 - ((float)ldrRaw / 4095.0 * 100.0);
  return constrain(pct, 0.0, 100.0);
}

float calcCO2(float coveragePct) {
  // cobertura 100% → 0 ton/ha | cobertura 0% → 150 ton/ha
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
//  ENDPOINTS DA API
// ════════════════════════════════════════════════════════════

// GET /api/sensors — leitura atual de todos os sensores
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
  doc["uptime_s"]     = (millis() - startMillis) / 1000;

  String out;
  serializeJson(doc, out);
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.send(200, "application/json", out);
}

// GET /api/status — status do dispositivo + histórico
void handleStatus() {
  StaticJsonDocument<1024> doc;
  doc["device"]    = "CarbonTrace-ESP32";
  doc["firmware"]  = "1.0.0";
  doc["wifi_ssid"] = SSID;
  doc["ip"]        = WiFi.localIP().toString();
  doc["uptime_s"]  = (millis() - startMillis) / 1000;
  doc["alert"]     = current.alert;

  JsonArray hist = doc.createNestedArray("history");
  int start = (historyCount < 10) ? 0 : historyIndex;
  for (int i = 0; i < historyCount; i++) {
    int idx = (start + i) % 10;
    JsonObject entry = hist.createNestedObject();
    entry["temp"]     = serialized(String(history[idx].temperature, 1));
    entry["humidity"] = serialized(String(history[idx].humidity, 1));
    entry["ldr"]      = history[idx].ldrRaw;
    entry["coverage"] = serialized(String(history[idx].coveragePct, 1));
    entry["co2"]      = serialized(String(history[idx].co2Estimate, 2));
    entry["status"]   = history[idx].status;
  }

  String out;
  serializeJson(doc, out);
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.send(200, "application/json", out);
}

// GET /api/carbon — estimativa de emissão de carbono
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
  doc["description"]          = "Estimativa baseada em LDR (proxy de cobertura vegetal)";

  String out;
  serializeJson(doc, out);
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.send(200, "application/json", out);
}

// GET /dashboard — painel HTML com gráficos em tempo real
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
  .card{background:#1e2130;border-radius:12px;padding:16px;border:1px solid #2d3748}
  .card.alert{border-color:#f87171}
  .card.ok{border-color:#34d399}
  .label{font-size:11px;color:#64748b;text-transform:uppercase;letter-spacing:.05em;margin-bottom:6px}
  .value{font-size:28px;font-weight:600;color:#fff}
  .unit{font-size:13px;color:#94a3b8;margin-left:2px}
  .badge{display:inline-block;padding:3px 10px;border-radius:20px;font-size:12px;font-weight:600;margin-top:8px}
  .badge.NORMAL{background:#064e3b;color:#34d399}
  .badge.ALERTA{background:#450a0a;color:#f87171}
  .badge.LOW{background:#064e3b;color:#34d399}
  .badge.MEDIUM{background:#78350f;color:#fbbf24}
  .badge.HIGH{background:#450a0a;color:#f87171}
  .chart-wrap{background:#1e2130;border-radius:12px;padding:16px;border:1px solid #2d3748;margin-bottom:16px}
  .chart-title{font-size:13px;color:#94a3b8;margin-bottom:12px;font-weight:500}
  canvas{max-height:180px}
  .led-row{display:flex;gap:12px;margin-top:8px}
  .led{width:14px;height:14px;border-radius:50%;background:#334155}
  .led.on-green{background:#34d399;box-shadow:0 0 8px #34d39988}
  .led.on-red{background:#f87171;box-shadow:0 0 8px #f8717188}
  footer{font-size:11px;color:#334155;text-align:center;margin-top:24px}
</style>
</head>
<body>
<h1>🌿 CarbonTrace Dashboard</h1>
<p class="sub">ESP32 IoT · FIAP Global Solution 2026/1 · atualiza a cada 5s</p>

<div class="grid">
  <div class="card"><div class="label">Temperatura</div><div class="value" id="temp">--<span class="unit">°C</span></div></div>
  <div class="card"><div class="label">Umidade</div><div class="value" id="hum">--<span class="unit">%</span></div></div>
  <div class="card"><div class="label">LDR (bruto)</div><div class="value" id="ldr">--</div></div>
  <div class="card"><div class="label">Cobertura vegetal</div><div class="value" id="cov">--<span class="unit">%</span></div></div>
  <div class="card"><div class="label">CO₂ estimado</div><div class="value" id="co2">--<span class="unit">t/ha</span></div></div>
  <div class="card" id="status-card">
    <div class="label">Status</div>
    <div class="value" id="status-val">--</div>
    <div class="led-row">
      <div class="led" id="led-green" title="LED Verde"></div>
      <div class="led" id="led-red"   title="LED Vermelho"></div>
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

<footer>CarbonTrace · ESP32 WebServer · /api/sensors</footer>

<script>
const mkChart = (id, label, color) => new Chart(document.getElementById(id), {
  type: 'line',
  data: {
    labels: [],
    datasets: [{
      label, data: [],
      borderColor: color,
      backgroundColor: color + '22',
      borderWidth: 2,
      pointRadius: 3,
      tension: 0.4,
      fill: true
    }]
  },
  options: {
    responsive: true,
    animation: false,
    scales: {
      y: { grid: { color: '#2d3748' }, ticks: { color: '#64748b' } },
      x: { grid: { color: '#2d3748' }, ticks: { color: '#64748b' } }
    },
    plugins: { legend: { display: false } }
  }
});

const chartTemp = mkChart('chartTemp', 'Temperatura', '#60a5fa');
const chartCov  = mkChart('chartCov',  'Cobertura',   '#34d399');
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

let tick = 0;
async function fetchData() {
  try {
    const r = await fetch('/api/sensors');
    const d = await r.json();
    tick++;
    const lbl = tick * 5 + 's';

    document.getElementById('temp').innerHTML    = d.temperature + '<span class="unit">°C</span>';
    document.getElementById('hum').innerHTML     = d.humidity    + '<span class="unit">%</span>';
    document.getElementById('ldr').textContent   = d.ldr_raw;
    document.getElementById('cov').innerHTML     = d.coverage_pct + '<span class="unit">%</span>';
    document.getElementById('co2').innerHTML     = d.co2_ton_ha   + '<span class="unit">t/ha</span>';
    document.getElementById('status-val').innerHTML =
      '<span class="badge ' + d.status + '">' + d.status + '</span>';
    document.getElementById('status-card').className = 'card ' + (d.alert ? 'alert' : 'ok');
    document.getElementById('led-green').className = 'led ' + (d.led_green ? 'on-green' : '');
    document.getElementById('led-red').className   = 'led ' + (d.led_red   ? 'on-red'   : '');

    addPoint(chartTemp, lbl, parseFloat(d.temperature));
    addPoint(chartCov,  lbl, parseFloat(d.coverage_pct));
  } catch(e) {
    console.warn('Erro ao buscar dados:', e);
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
  doc["endpoints"] = "/dashboard | /api/sensors | /api/status | /api/carbon";
  String out;
  serializeJson(doc, out);
  server.send(404, "application/json", out);
}