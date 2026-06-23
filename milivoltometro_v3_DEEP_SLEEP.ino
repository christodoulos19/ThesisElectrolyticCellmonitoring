// ============================================================
// Millivoltmeter Data Logger  –  v3 (Deep Sleep Edition)
// Hardware: ESP32 + MCP3424 ADC
// ============================================================
//
// GPIO 21 (SDA) / GPIO 22 (SCL) => MCP3424 I2C
// GPIO 34 => Battery voltage divider (ADC input)
//
// ── Deep Sleep Flow ─────────────────────────────────────────
//   Wake → setup() → I2C init → WiFi (Static IP) → takeReading()
//        → loop() handles web clients for ACTIVE_WINDOW_MS (10s)
//        → Deep Sleep για SLEEP_DURATION_US (10 λεπτά)
//   RTC Slow Memory διατηρεί readings, timestamps & στατιστικά.
// ============================================================
 
#include <WiFi.h>
#include <WebServer.h>
#include <Wire.h>
#include <MCP342x.h>
#include <esp_sleep.h>
 
// ── Deep Sleep timing ────────────────────────────────────────
#define ACTIVE_WINDOW_MS   10000UL               // 10 δευτερόλεπτα ενεργό
#define SLEEP_DURATION_US  ((9ULL * 60) + 50) * 1000000 // 9 λεπτά και 50 δευτερόλεπτα σε µs
 
// ── WiFi credentials ─────────────────────────────────────────
const char* ssid     = "sensors";
const char* password = "sensorslab";
 
// ── Static IP configuration ──────────────────────────────────
// Αλλάξτε αυτές τις τιμές ώστε να ταιριάζουν με το δίκτυό σας.
IPAddress STATIC_IP (192, 168, 1, 101);  // Σταθερή IP του ESP32
IPAddress GATEWAY   (192, 168, 1,   1);  // Gateway (router)
IPAddress SUBNET    (255, 255, 255, 0);  // Subnet mask
IPAddress DNS1      (8,   8,   8,   8);  // Primary DNS (Google)
 
// WiFi timeout: αν δεν συνδεθεί σε τόσο ms, πάμε σε AP mode
#define WIFI_TIMEOUT_MS 4000
 
// ── MCP3424 ──────────────────────────────────────────────────
MCP342x adc = MCP342x(0x68);
 
// ── Web Server ───────────────────────────────────────────────
WebServer server(80);
 
// ── RTC Slow Memory – survives Deep Sleep ────────────────────
// Όλες οι μεταβλητές που πρέπει να παραμείνουν μεταξύ των κύκλων
// ύπνου ορίζονται με RTC_DATA_ATTR.
 
const int MAX_READINGS = 500;
 
RTC_DATA_ATTR float         readings[MAX_READINGS];
RTC_DATA_ATTR unsigned long timestamps[MAX_READINGS];
RTC_DATA_ATTR int           readingIndex  = 0;
RTC_DATA_ATTR int           totalReadings = 0;
 
RTC_DATA_ATTR float minVoltage = 0;
RTC_DATA_ATTR float maxVoltage = 0;
RTC_DATA_ATTR float avgVoltage = 0;
 
// ── Regular RAM – ανακτώνται κάθε φορά ──────────────────────
float currentVoltage = 0;
 
// Χρονόμετρο για το active window
unsigned long wakeTime = 0;
 
// ── Battery ──────────────────────────────────────────────────
const int batteryPin = 34;
 
float getBatteryPercentage() {
  long sumRaw = 0;
  for (int i = 0; i < 15; i++) {
    sumRaw += analogRead(batteryPin);
    delay(2);
  }
  float rawValue = sumRaw / 15.0;
  float voltage  = (rawValue / 4095.0) * 3.3 * 2.2;
  float percentage = (voltage - 3.5) / (4.2 - 3.5) * 100.0;
  if (percentage > 100) return 100.0;
  if (percentage < 0)   return 0.0;
  return percentage;
}
 
// ── Statistics ───────────────────────────────────────────────
void calculateStats() {
  if (totalReadings == 0) return;
 
  int count = min(totalReadings, MAX_READINGS);
  minVoltage = readings[0];
  maxVoltage = readings[0];
  float sum = 0;
 
  for (int i = 0; i < count; i++) {
    if (readings[i] < minVoltage) minVoltage = readings[i];
    if (readings[i] > maxVoltage) maxVoltage = readings[i];
    sum += readings[i];
  }
  avgVoltage = sum / count;
}
 
// ── Take reading ─────────────────────────────────────────────
void takeReading() {
  long value = 0;
  MCP342x::Config status;
 
  uint8_t err = adc.convertAndRead(MCP342x::channel3, MCP342x::oneShot,
                                    MCP342x::resolution18, MCP342x::gain1,
                                    1000000, value, status);
 
  if (err == 0) {
    float voltage_mV = value * 0.015625;
 
    // Αποθήκευση στον κυκλικό buffer (RTC memory)
    readings[readingIndex]   = voltage_mV;
    timestamps[readingIndex] = millis() / 1000;
    readingIndex = (readingIndex + 1) % MAX_READINGS;
    if (totalReadings < MAX_READINGS) totalReadings++;
 
    currentVoltage = voltage_mV;
    calculateStats();
 
    Serial.print("SUCCESS! Reading: ");
    Serial.print(voltage_mV, 3);
    Serial.println(" mV");
  } else {
    Serial.print("ADC ERROR CODE: ");
    Serial.print(err);
    if (err == 4) Serial.println(" (Device not found - Check I2C Wiring/Address)");
    else if (err == 1) Serial.println(" (Data too long)");
    else Serial.println(" (Unknown Error)");
  }
}
 
// ── Web: Main page ───────────────────────────────────────────
void handleRoot() {
  String html = R"rawliteral(
    <!DOCTYPE html>
    <html>
    <head>
    <meta charset='UTF-8'>
    <meta name='viewport' content='width=device-width, initial-scale=1'>
    <title>Millivoltmeter Monitor</title>
    <style>
        * { margin: 0; padding: 0; box-sizing: border-box; }
        body { font-family: 'Segoe UI', Arial, sans-serif; background: linear-gradient(135deg, #667eea 0%, #764ba2 100%); padding: 20px; min-height: 100vh; }
        .container { background: white; padding: 30px; border-radius: 15px; max-width: 1200px; margin: auto; box-shadow: 0 10px 40px rgba(0,0,0,0.2); }
        h1 { color: #333; margin-bottom: 10px; font-size: 32px; }
        .status { display: inline-block; padding: 5px 15px; border-radius: 20px; font-size: 14px; font-weight: bold; margin-bottom: 20px; background: #4CAF50; color: white; }
        .current-reading { background: linear-gradient(135deg, #667eea 0%, #764ba2 100%); color: white; padding: 40px; border-radius: 10px; text-align: center; margin: 20px 0; box-shadow: 0 5px 20px rgba(102, 126, 234, 0.4); }
        .current-reading .label { font-size: 18px; opacity: 0.9; margin-bottom: 10px; }
        .current-reading .value { font-size: 64px; font-weight: bold; text-shadow: 2px 2px 4px rgba(0,0,0,0.2); }
        .stats { display: grid; grid-template-columns: repeat(auto-fit, minmax(180px, 1fr)); gap: 15px; margin: 20px 0; }
        .stat { background: #f8f9fa; padding: 15px; border-radius: 10px; border-left: 4px solid #667eea; transition: transform 0.2s; }
        .stat:hover { transform: translateY(-3px); box-shadow: 0 4px 10px rgba(0,0,0,0.05); }
        .stat .label { color: #666; font-size: 13px; margin-bottom: 5px; }
        .stat .value { color: #333; font-size: 22px; font-weight: bold; }
        .chart-container { margin: 20px 0; background: #f8f9fa; padding: 20px; border-radius: 10px; }
        canvas { width: 100% !important; height: auto !important; }
        .controls { display: flex; gap: 10px; margin: 20px 0; }
        .button { background: #667eea; color: white; padding: 12px 24px; text-decoration: none; border-radius: 8px; border: none; cursor: pointer; font-weight: 600; transition: 0.3s; box-shadow: 0 4px 6px rgba(102, 126, 234, 0.3); }
        .button:hover { background: #5568d3; transform: translateY(-2px); }
        .footer { margin-top: 30px; color: #666; font-size: 14px; text-align: center; border-top: 1px solid #eee; padding-top: 15px; }
        @keyframes pulse { 0%, 100% { opacity: 1; } 50% { opacity: 0.7; } }
        .updating { animation: pulse 0.5s; }
    </style>
    </head>
    <body>
    <div class='container'>
        <h1>⚡ Millivoltmeter Logger</h1>
        <span class='status' id='status-indicator'>🟢 Live</span>
        <div class='current-reading'>
            <div class='label'>Current Reading</div>
            <div class='value' id='current'>---.--- mV</div>
        </div>
        <div class='stats'>
            <div class='stat'><div class='label'>Battery</div><div class='value' id='batt'>--- %</div></div>
            <div class='stat'><div class='label'>Total Readings</div><div class='value' id='total'>0</div></div>
            <div class='stat'><div class='label'>Average</div><div class='value' id='avg'>--- mV</div></div>
            <div class='stat'><div class='label'>Minimum</div><div class='value' id='min'>--- mV</div></div>
            <div class='stat'><div class='label'>Maximum</div><div class='value' id='max'>--- mV</div></div>
        </div>
        <div class='chart-container'><canvas id='chart' width='800' height='300'></canvas></div>
        <div class='controls'>
            <a href='/download' class='button'>📥 Download CSV</a>
            <button class='button' onclick='clearChart()'>🗑️ Clear Chart</button>
        </div>
        <div class='footer'>🔄 Auto-refresh every 2s | ⏱️ Uptime: <span id='uptime'>0</span>s</div>
    </div>
    <script>
        let chartData = [];
        const MAX_POINTS = 50;
        const canvas = document.getElementById('chart');
        const ctx = canvas.getContext('2d');
 
        function updateData() {
            fetch('/data')
                .then(res => res.json())
                .then(data => {
                    document.getElementById('current').innerText = data.current.toFixed(3) + ' mV';
                    document.getElementById('batt').innerText = data.batt + '%';
                    document.getElementById('total').innerText = data.total;
                    document.getElementById('avg').innerText = data.avg.toFixed(3) + ' mV';
                    document.getElementById('min').innerText = data.min.toFixed(3) + ' mV';
                    document.getElementById('max').innerText = data.max.toFixed(3) + ' mV';
                    document.getElementById('uptime').innerText = data.uptime;
 
                    document.getElementById('batt').style.color = (data.batt < 20) ? '#ff3d3d' : '#2ecc71';
 
                    document.getElementById('current').classList.add('updating');
                    setTimeout(() => document.getElementById('current').classList.remove('updating'), 500);
 
                    chartData.push(data.current);
                    if (chartData.length > MAX_POINTS) chartData.shift();
                    drawChart();
                    document.getElementById('status-indicator').style.background = '#4CAF50';
                })
                .catch(err => {
                    document.getElementById('status-indicator').style.background = '#ff3d3d';
                });
        }
 
        function drawChart() {
            const p = 40, w = canvas.width - 2*p, h = canvas.height - 2*p;
            ctx.clearRect(0, 0, canvas.width, canvas.height);
            if (chartData.length < 2) return;
 
            const min = Math.min(...chartData), max = Math.max(...chartData), r = (max - min) || 1;
 
            ctx.strokeStyle = '#e0e0e0'; ctx.lineWidth = 1; ctx.textAlign = 'right'; ctx.fillStyle = '#666';
            for (let i = 0; i <= 5; i++) {
                const y = p + (h / 5) * i;
                ctx.beginPath(); ctx.moveTo(p, y); ctx.lineTo(p + w, y); ctx.stroke();
                const val = max - (r / 5) * i;
                ctx.fillText(val.toFixed(2), p - 10, y + 4);
            }
 
            ctx.strokeStyle = '#667eea'; ctx.lineWidth = 3; ctx.beginPath();
            chartData.forEach((v, i) => {
                const x = p + (w / (MAX_POINTS - 1)) * i;
                const y = p + h - ((v - min) / r) * h;
                if (i === 0) ctx.moveTo(x, y); else ctx.lineTo(x, y);
            });
            ctx.stroke();
 
            ctx.fillStyle = '#667eea';
            chartData.forEach((v, i) => {
                const x = p + (w / (MAX_POINTS - 1)) * i;
                const y = p + h - ((v - min) / r) * h;
                ctx.beginPath(); ctx.arc(x, y, 4, 0, Math.PI * 2); ctx.fill();
            });
        }
 
        function clearChart() { if (confirm('Clear chart?')) { chartData = []; drawChart(); } }
        setInterval(updateData, 2000);
        updateData();
    </script>
    </body>
    </html>
  )rawliteral";
  server.send(200, "text/html", html);
}
 
// ── Web: JSON data ───────────────────────────────────────────
void handleData() {
  String json = "{";
  json += "\"current\":" + String(currentVoltage, 3) + ",";
  json += "\"batt\":"    + String(getBatteryPercentage(), 0) + ",";
  json += "\"min\":"     + String(minVoltage, 3) + ",";
  json += "\"max\":"     + String(maxVoltage, 3) + ",";
  json += "\"avg\":"     + String(avgVoltage, 3) + ",";
  json += "\"total\":"   + String(totalReadings) + ",";
  json += "\"uptime\":"  + String(millis() / 1000);
  json += "}";
  server.send(200, "application/json", json);
}
 
// ── Web: CSV download ────────────────────────────────────────
void handleDownload() {
  String csv = "Reading,Time_seconds,Voltage_mV\n";
 
  int count    = min(totalReadings, MAX_READINGS);
  int startIdx = totalReadings > MAX_READINGS ? readingIndex : 0;
 
  for (int i = 0; i < count; i++) {
    int idx = (startIdx + i) % MAX_READINGS;
    csv += String(totalReadings - count + i + 1) + ",";
    csv += String(timestamps[idx]) + ",";
    csv += String(readings[idx], 3) + "\n";
  }
 
  server.sendHeader("Content-Disposition", "attachment; filename=millivolt_log.csv");
  server.send(200, "text/csv", csv);
}
 
// ============================================================
// setup() – εκτελείται κάθε φορά που ξυπνά ο ESP32
// ============================================================
void setup() {
  Serial.begin(115200);
  Serial.println("\n=== Millivoltmeter Data Logger (v3 – Deep Sleep) ===");
  Serial.printf("Wake-up | Total readings so far: %d\n", totalReadings);
 
  // ── Battery ADC ──────────────────────────────────────────
  pinMode(batteryPin, INPUT);
  analogReadResolution(12);
  analogSetAttenuation(ADC_11db);
 
  // ── I2C & ADC ────────────────────────────────────────────
  Wire.begin(21, 22);
  MCP342x::generalCallReset();
  delay(100);
 
  // ── Static IP & WiFi σύνδεση ─────────────────────────────
  WiFi.config(STATIC_IP, GATEWAY, SUBNET, DNS1);
  WiFi.begin(ssid, password);
 
  Serial.print("Connecting to WiFi (Static IP)");
  unsigned long wifiStart = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - wifiStart < WIFI_TIMEOUT_MS) {
    delay(200);
    Serial.print(".");
  }
 
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWiFi connected!");
    Serial.print("IP: "); Serial.println(WiFi.localIP());
  } else {
    // Fallback: Access Point mode
    Serial.println("\nWiFi failed – starting Access Point.");
    WiFi.softAP("MillivoltLogger", "logger123");
    Serial.print("AP IP: "); Serial.println(WiFi.softAPIP());
  }
 
  // ── Μέτρηση αισθητήρα (εφάπαξ ανά κύκλο αφύπνισης) ─────
  takeReading();
 
  // ── Web Server ────────────────────────────────────────────
  server.on("/",         handleRoot);
  server.on("/data",     handleData);
  server.on("/download", handleDownload);
  server.begin();
  Serial.println("Web server started. Active for 10 seconds...");
 
  // ── Ρύθμιση timer για Deep Sleep (10 λεπτά) ─────────────
  esp_sleep_enable_timer_wakeup(SLEEP_DURATION_US);
 
  // Αποθηκεύουμε τον χρόνο αφύπνισης για το non-blocking countdown
  wakeTime = millis();
}
 
// ============================================================
// loop() – τρέχει κατά το active window των 10 δευτερολέπτων
// ============================================================
void loop() {
  // Εξυπηρέτηση αιτημάτων web (non-blocking)
  server.handleClient();
 
  // Έλεγχος αν έληξε το active window
  if (millis() - wakeTime >= ACTIVE_WINDOW_MS) {
    Serial.println("\n--- Active window expired. Entering Deep Sleep for 10 minutes. ---");
    Serial.flush();
 
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
 
    esp_deep_sleep_start();
    // Μετά από αυτό το σημείο ο ESP32 κοιμάται.
    // Η εκτέλεση συνεχίζεται από την αρχή του setup() μετά από 10 λεπτά.
  }
}