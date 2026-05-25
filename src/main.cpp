// ── Libraries ────────────────────────────────────────────────────────────────
#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <TFT_eSPI.h>

// ── Pin assignments ───────────────────────────────────────────────────────────
#define LDR_PIN 36   // LDR analog input (ADC1_CH0)
#define TFT_BL_PIN 4 // TFT backlight control

// ── Sensor mode: 1 = simulated data, 0 = real DHT22 + LDR ───────────────────
#define SIMULATE_SENSORS 1

#if !SIMULATE_SENSORS
#include <DHT.h>
#define DHT_PIN 12
#define DHT_TYPE DHT22
DHT dht(DHT_PIN, DHT_TYPE);
#endif

// ── Network credentials ───────────────────────────────────────────────────────
#define WIFI_SSID "<ssid>"
#define WIFI_PASS "<pass>"

// ── MQTT broker ───────────────────────────────────────────────────────────────
#define MQTT_SERVER "<host-ip>"
#define MQTT_PORT 1883

// ── Sensor refresh interval ───────────────────────────────────────────────────
#define UPDATE_MS 2000

// ── Global objects ────────────────────────────────────────────────────────────
WiFiClient wifiClient;
PubSubClient mqtt(wifiClient);
TFT_eSPI tft = TFT_eSPI();

// ── Sensor state (mid-range defaults) ────────────────────────────────────────
float temperature = 25.0f;
float humidity = 70.0f;
float lightPct = 50.0f;

// ── Display: fixed header bar ─────────────────────────────────────────────────
void drawHeader()
{
  tft.fillRect(0, 0, 135, 34, TFT_NAVY);
  tft.setTextColor(TFT_WHITE, TFT_NAVY);
  tft.setTextSize(2);
  tft.setTextDatum(MC_DATUM);
  tft.drawString("BED ROOM", 67, 17);
  tft.setTextDatum(TL_DATUM);
}

// ── Display: IP address row (green = connected, red = offline) ───────────────
void drawIPRow()
{
  tft.fillRect(0, 34, 135, 24, TFT_BLACK);
  tft.setTextSize(2);
  tft.setTextDatum(MC_DATUM);
  if (WiFi.status() == WL_CONNECTED)
  {
    IPAddress ip = WiFi.localIP();
    char buf[12];
    snprintf(buf, sizeof(buf), ".%d.%d", ip[2], ip[3]); // last two octets
    tft.setTextColor(TFT_GREEN, TFT_BLACK);
    tft.drawString(buf, 67, 46);
  }
  else
  {
    tft.setTextColor(TFT_RED, TFT_BLACK);
    tft.drawString("No WiFi", 67, 46);
  }
  tft.setTextDatum(TL_DATUM);
}

// ── Display: generic centered text row (optional second line) ─────────────────
void drawRow(int y, uint16_t color, const char *line1, const char *line2 = nullptr)
{
  int h = line2 ? 40 : 22;
  tft.fillRect(0, y, 135, h, TFT_BLACK);
  tft.setTextColor(color, TFT_BLACK);
  tft.setTextSize(2);
  tft.setTextDatum(MC_DATUM);
  tft.drawString(line1, 67, y + 11);
  if (line2)
    tft.drawString(line2, 67, y + 30);
  tft.setTextDatum(TL_DATUM);
}

// ── Display: refresh all sensor rows ─────────────────────────────────────────
void drawUI()
{
  char t_buf[16], h_buf[16], l_buf[16];
  snprintf(t_buf, sizeof(t_buf), "T: %.2fC", temperature);
  snprintf(h_buf, sizeof(h_buf), "H: %.2f%%", humidity);
  snprintf(l_buf, sizeof(l_buf), "L: %.2f%%", lightPct);

  drawIPRow();
  drawRow(87, TFT_ORANGE, t_buf);
  drawRow(138, TFT_CYAN, h_buf);
  drawRow(189, TFT_YELLOW, l_buf);
}

// ── MQTT: per-topic publish timers (10 s ± 2 s each, independent) ────────────
static unsigned long lastTempPub = 0, nextTempMs = 10000;
static unsigned long lastHumidPub = 0, nextHumidMs = 10000;
static unsigned long lastLightPub = 0, nextLightMs = 10000;

// ── MQTT: format value and publish to topic ───────────────────────────────────
void mqttPublish(const char *topic, float value)
{
  char buf[12];
  snprintf(buf, sizeof(buf), "%.2f", value);
  mqtt.publish(topic, buf);
  Serial.printf("[MQTT] %s => %s\n", topic, buf);
}

// ── MQTT: reconnect if needed, then check each publish timer ─────────────────
void mqttLoop()
{
  // reconnect at most once every 5 s
  if (!mqtt.connected() && WiFi.status() == WL_CONNECTED)
  {
    static unsigned long lastRetry = 0;
    if (millis() - lastRetry > 5000)
    {
      lastRetry = millis();
      mqtt.connect("bed-sensors");
    }
  }
  mqtt.loop();

  if (!mqtt.connected())
    return;

  // publish each value on its own randomised interval
  unsigned long now = millis();
  if (now - lastTempPub >= nextTempMs)
  {
    lastTempPub = now;
    nextTempMs = random(8000, 12001);
    mqttPublish("/home/bed/dht/temp", temperature);
  }
  if (now - lastHumidPub >= nextHumidMs)
  {
    lastHumidPub = now;
    nextHumidMs = random(8000, 12001);
    mqttPublish("/home/bed/dht/humid", humidity);
  }
  if (now - lastLightPub >= nextLightMs)
  {
    lastLightPub = now;
    nextLightMs = random(8000, 12001);
    mqttPublish("/home/bed/ldr/light", lightPct);
  }
}

// ── Setup ─────────────────────────────────────────────────────────────────────
void setup()
{
  Serial.begin(115200);
  randomSeed(analogRead(0)); // seed from floating ADC pin for true randomness

  // turn on TFT backlight
  pinMode(TFT_BL_PIN, OUTPUT);
  digitalWrite(TFT_BL_PIN, HIGH);

  // initialise display and show boot message
  tft.init();
  tft.setRotation(0);
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextSize(2);
  tft.setTextDatum(MC_DATUM);
  tft.drawString("Starting...", 67, 120);
  tft.setTextDatum(TL_DATUM);

#if !SIMULATE_SENSORS
  dht.begin();
#endif

  // point MQTT client at broker
  mqtt.setServer(MQTT_SERVER, MQTT_PORT);

  // start WiFi (non-blocking)
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  // brief pause then draw static header
  delay(2000);
  tft.fillScreen(TFT_BLACK);
  drawHeader();
}

// ── Main loop ─────────────────────────────────────────────────────────────────
void loop()
{
  static unsigned long lastUpdate = 0;

  // read sensors and refresh display every UPDATE_MS
  if (millis() - lastUpdate >= UPDATE_MS)
  {
    lastUpdate = millis();

#if SIMULATE_SENSORS
    // random walk within realistic ranges
    temperature += (float)random(-49, 50) / 100.0f; // ±0.49 °C
    temperature = constrain(temperature, 20.0f, 30.0f);

    humidity += (float)random(-199, 200) / 100.0f; // ±1.99 %RH
    humidity = constrain(humidity, 60.0f, 80.0f);

    lightPct += (float)random(-500, 501) / 100.0f; // ±5 %
    lightPct = constrain(lightPct, 0.0f, 100.0f);
#else
    // read real DHT22
    float t = dht.readTemperature();
    float h = dht.readHumidity();
    if (!isnan(t))
      temperature = t;
    if (!isnan(h))
      humidity = h;

    // read LDR: higher ADC = brighter light
    lightPct = analogRead(LDR_PIN) / 4095.0f * 100.0f;
#endif

    drawUI();

    Serial.printf("[%lus] T=%.2f°C  H=%.2f%%  L=%.2f%%  WiFi=%s\n",
                  millis() / 1000, temperature, humidity, lightPct,
                  WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString().c_str() : "no");
  }

  // handle MQTT publish timers
  mqttLoop();

  // reconnect WiFi if dropped
  if (WiFi.status() != WL_CONNECTED)
  {
    static unsigned long lastReconnect = 0;
    if (millis() - lastReconnect > 10000)
    {
      lastReconnect = millis();
      WiFi.disconnect();
      WiFi.begin(WIFI_SSID, WIFI_PASS);
    }
  }
}
