# 🌿 CarbonTrace — Sensor de Qualidade do Ar (IoT)

> **FIAP Global Solution 2026/1** · Disruptive Architectures: IoT, IoB & Generative IA
> Turma: 2TDSPG

---

## 👥 Integrantes

| Nome | RM |
|---|---|
| Gabriel Neris Losano | RM564093 |
| João Vitor Biribilli Ravelli | RM565594 |
| Pedro de Matos Previtali | RM564184 |
| Pietro Paranhos Wilhelm *(representante)* | RM561378 |
| Felipe Monte de Sousa | RM562019 |

---

## 📌 Sobre o Projeto

O **CarbonTrace** é uma plataforma de monitoramento ambiental com foco na correlação entre
desmatamento e qualidade do ar.

Este repositório contém o **protótipo IoT v3.0** desenvolvido em **ESP32**, que monitora em
tempo real a qualidade do ar por meio de três sensores complementares:
- **MQ-135** — concentração de CO₂ e gases nocivos (ppm)
- **DHT22** — temperatura (°C) e umidade relativa (%)
- **PM2.5** — material particulado fino (µg/m³)

Os dados são disponibilizados via **API REST** com dashboard web e podem ser enviados
automaticamente a um **backend externo** configurável via API, sem reiniciar o dispositivo.

> O módulo IoT é independente da API .NET do projeto — a integração se dá exclusivamente
> via HTTP POST para o endpoint configurado em `/api/config`.

---

## 🛰️ Conexão com o Tema (Economia Espacial / Desmatamento)

Satélites de observação terrestre (Sentinel/ESA, Landsat/NASA) monitoram a cobertura vegetal
globalmente. Áreas desmatadas alteram significativamente a qualidade do ar local:
- ↑ CO₂ e gases de combustão (queimadas)
- ↑ Material particulado PM2.5 (fumaça, poeira exposta)
- ↑ Temperatura (ausência do dossel)
- ↓ Umidade relativa (perda de evapotranspiração)

O protótipo IoT simula uma **estação de campo autônoma** que captura essas métricas,
**validando e enriquecendo** os dados satelitais com medições in-situ.

---

## ⚙️ Hardware

### Componentes

| Componente | Função | Pino ESP32 |
|---|---|---|
| MQ-135 | Qualidade do ar — CO₂ / gases (ADC analógico) | GPIO34 (ADC1 CH6) |
| PM2.5 (GP2Y1010) | Material particulado fino (ADC analógico) | GPIO35 (ADC1 CH7) |
| DHT22 | Temperatura e umidade | GPIO4 |
| Push Button | Reset manual de alertas | GPIO14 (INPUT_PULLUP) |
| LED Verde | Qualidade GOOD / NORMAL | GPIO18 |
| LED Vermelho | Qualidade UNHEALTHY / HAZARDOUS / ALERTA | GPIO19 |
| Buzzer | Alarme sonoro ao detectar alerta | GPIO23 |
| LCD 16×2 I2C | Interface local — CO₂ (ppm) + PM2.5 (µg/m³) | SDA=21 / SCL=22 |

> **GPIO34 e GPIO35** são pinos *input-only* do ESP32, ideais para leitura ADC sem interferência
> de saída. No Wokwi, ambos são simulados por **potenciômetros** que representam
> a tensão analógica dos sensores reais.

### Diagrama de Conexões

```
ESP32 GPIO4   ──→ DHT22 DATA
ESP32 GPIO34  ──→ MQ-135 AOUT   (potenciômetro no simulador)
ESP32 GPIO35  ──→ PM2.5 AOUT    (potenciômetro no simulador)
ESP32 GPIO14  ──→ Push Button   (INPUT_PULLUP — outra perna ao GND)
ESP32 GPIO18  ──→ Resistor 220Ω → LED Verde
ESP32 GPIO19  ──→ Resistor 220Ω → LED Vermelho
ESP32 GPIO23  ──→ Buzzer (+)
ESP32 GPIO21  ──→ LCD SDA (I2C)
ESP32 GPIO22  ──→ LCD SCL (I2C)
```

---

## 🧠 Lógica de Funcionamento

```
MQ-135   lê tensão analógica (ADC 0–4095)
  ↓  calcCO2Ppm()   → mapeamento linear: 400–5000 ppm
DHT22    lê temperatura e umidade
PM2.5    lê tensão analógica (ADC 0–4095)
  ↓  calcPM25UgM3() → mapeamento linear: 0–500 µg/m³

calcAirQuality(co2_ppm, pm25):
  GOOD       → CO₂ ≤ 1000 ppm  E  PM2.5 ≤ 35 µg/m³
  MODERATE   → CO₂ ≤ 1500 ppm  E  PM2.5 ≤ 55 µg/m³
  UNHEALTHY  → CO₂ ≤ 2000 ppm  E  PM2.5 ≤ 150 µg/m³
  HAZARDOUS  → CO₂ > 2000 ppm  OU PM2.5 > 150 µg/m³

ALERTA se: CO₂ > 1000 ppm  OU  PM2.5 > 35 µg/m³  OU  Temp > 40°C
  → LED Vermelho + Buzzer (1s) + LCD exibe dados críticos

Envio automático ao backend a cada 30s (se backend_url configurado)
```

**Referências:** EPA Air Quality Index (AQI) · CONAMA Resolução 491/2018 · IPCC AR6

---

## 🌐 API REST — Endpoints

Base URL (Wokwi): `http://localhost:8180`

### `GET /api/sensors`
Leitura atual de todos os sensores e estado dos atuadores.

```json
{
  "temperature": "27.5",
  "humidity": "72.0",
  "co2_raw": 410,
  "co2_ppm": "861",
  "pm25_raw": 328,
  "pm25_ug_m3": "40.0",
  "air_quality": "MODERATE",
  "alert": false,
  "status": "NORMAL",
  "led_green": true,
  "led_red": false,
  "mq135_ready": true,
  "uptime_s": 120
}
```

---

### `GET /api/air-quality`
Índice de qualidade do ar com descrição e recomendações para a população.

```json
{
  "air_quality": "MODERATE",
  "color_hex": "#fbbf24",
  "co2_ppm": "861",
  "pm25_ug_m3": "40.0",
  "temperature_c": "27.5",
  "humidity_pct": "72.0",
  "alert": false,
  "description": "Qualidade do ar moderada",
  "recommendation": "Grupos sensiveis devem reduzir exposicao prolongada.",
  "co2_threshold_ppm": 1000,
  "pm25_threshold_ugm3": 35.0,
  "mq135_ready": true
}
```

`air_quality`: `GOOD` · `MODERATE` · `UNHEALTHY` · `HAZARDOUS`

---

### `GET /api/status`
Status do dispositivo, thresholds ativos, backend configurado e histórico.

```json
{
  "device": "CarbonTrace-ESP32",
  "firmware": "3.0.0",
  "wifi_ssid": "Wokwi-GUEST",
  "ip": "10.10.0.2",
  "uptime_s": 120,
  "alert": false,
  "mq135_ready": true,
  "co2_threshold_ppm": 1000,
  "pm25_threshold_ugm3": 35.0,
  "temp_threshold_c": 40.0,
  "backend_url": "(nao configurado)",
  "history_count": 5
}
```

---

### `GET /api/history`
Últimas 10 leituras armazenadas em buffer circular com timestamp de uptime.

```json
{
  "total_records": 3,
  "readings": [
    {
      "uptime_s": 5,
      "temp": "27.5",
      "humidity": "72.0",
      "co2_ppm": "861",
      "pm25_ug_m3": "40.0",
      "air_quality": "MODERATE",
      "status": "NORMAL",
      "alert": false
    }
  ]
}
```

---

### `POST /api/config`
Altera thresholds e URL do backend em tempo real, sem reiniciar o dispositivo.

**Body (todos os campos são opcionais individualmente):**
```json
{
  "co2_threshold": 800,
  "pm25_threshold": 25.0,
  "temp_threshold": 38.0,
  "backend_url": "http://meu-servidor.com/api/iot/data"
}
```

**Resposta:**
```json
{
  "message": "Configuracao atualizada",
  "co2_threshold_ppm": 800,
  "pm25_threshold_ugm3": 25.0,
  "temp_threshold_c": 38.0,
  "backend_url": "http://meu-servidor.com/api/iot/data"
}
```

> Após configurar `backend_url`, o ESP32 envia automaticamente um **POST JSON**
> a cada 30 segundos com todos os dados dos sensores para integração com o backend .NET.

---

### `GET /dashboard`
Painel HTML com gráficos em tempo real (atualiza a cada 5s).
Inclui indicador de warm-up do MQ-135 e formulário para alterar thresholds + backend URL.

### `GET /api/docs`
Documentação completa da API em HTML.

---

## 🖥️ Como Simular no Wokwi

### Pré-requisitos
- [VS Code](https://code.visualstudio.com/)
- Extensão [Wokwi for VS Code](https://marketplace.visualstudio.com/items?itemName=wokwi.wokwi-vscode)
- [Arduino IDE](https://www.arduino.cc/en/software) com core ESP32 instalado

### Bibliotecas necessárias (Arduino Library Manager)

| Biblioteca | Autor |
|---|---|
| DHT sensor library | Adafruit |
| Adafruit Unified Sensor | Adafruit |
| LiquidCrystal I2C | Frank de Brabander |
| ArduinoJson | Benoit Blanchon |

> `WiFi`, `WebServer`, `Wire`, `HTTPClient` já vêm com o core ESP32.

### Passo a Passo

**1. Abrir o projeto no Arduino IDE**

- Abra `carbontrace-iot.ino`
- Selecione a placa: `ESP32 Dev Module`

**2. Compilar**

```
Sketch → Verify/Compile (Ctrl+R)
```

Os arquivos `.elf` e `.bin` serão gerados em:
```
build/esp32.esp32.esp32doit-devkit-v1/
```

**3. Iniciar a simulação no VS Code**

- Pressione `F1` → `Wokwi: Start Simulator`
- Aguarde o ESP32 conectar ao Wi-Fi (`Wokwi-GUEST`)
- O IP aparecerá no Serial Monitor

**4. Acessar o dashboard**

```
http://localhost:8180/dashboard
```

### Simulando os sensores no Wokwi

| Sensor | Componente Wokwi | Como simular |
|---|---|---|
| MQ-135 (CO₂) | Potenciômetro em GPIO34 | Gire para cima → mais CO₂ (ppm) |
| PM2.5 | Potenciômetro em GPIO35 | Gire para cima → mais partículas (µg/m³) |
| DHT22 | wokwi-dht22 | Edite `attrs.temperature` e `attrs.humidity` no `diagram.json` |

---

## 🧪 Testando os Endpoints

```bash
# Leitura atual
curl http://localhost:8180/api/sensors

# Índice de qualidade do ar
curl http://localhost:8180/api/air-quality

# Status do dispositivo
curl http://localhost:8180/api/status

# Histórico (últimas 10 leituras)
curl http://localhost:8180/api/history

# Alterar thresholds + configurar backend externo
curl -X POST http://localhost:8180/api/config \
  -H "Content-Type: application/json" \
  -d '{
    "co2_threshold": 800,
    "pm25_threshold": 25.0,
    "temp_threshold": 38.0,
    "backend_url": "http://meu-backend.com/api/iot/data"
  }'
```

---

## 📡 Integração com Backend Externo

Após configurar `backend_url` via `POST /api/config`, o ESP32 envia automaticamente a cada 30s:

```json
{
  "device": "CarbonTrace-ESP32",
  "timestamp_s": 120,
  "temperature": "27.5",
  "humidity": "72.0",
  "co2_ppm": "861",
  "pm25_ug_m3": "40.0",
  "air_quality": "MODERATE",
  "alert": false,
  "status": "NORMAL"
}
```

Para desativar o envio, envie `"backend_url": ""` via `POST /api/config`.

---

## 📁 Estrutura do Repositório

```
carbontrace-iot/
├── carbontrace-iot.ino   # Código principal ESP32 (v3.0)
├── diagram.json          # Circuito Wokwi
├── wokwi.toml            # Configuração do simulador
├── libraries.txt         # Dependências
└── README.md
```

---

## 🔗 Links

- 🎥 Vídeo de demonstração: *(link YouTube)*
- 📡 Repositório API .NET: *(link GitHub)*
