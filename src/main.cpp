// ── Libraries ────────────────────────────────────────────────────────────────
#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <TFT_eSPI.h>

// ── Pin assignments ───────────────────────────────────────────────────────────
#define LDR_PIN 36    // LDR analog input (ADC1_CH0)
#define TFT_BL_PIN 4  // TFT backlight control
#define RELAY_PIN 13  // Pull-up relay, active low (LOW=ON, HIGH=OFF)
#define BTN_PIN 35    // Built-in right button, active low

// ── Pastel colour palette (RGB565) ───────────────────────────────────────────
#define COL_HDR_BG    0x3AD0   // slate blue    #3D5A80
#define COL_IP_OK     0xCD5D   // soft lavender #C8A8E8
#define COL_IP_FAIL   0xE410   // soft coral    #E08080
#define COL_TEMP      0xFD0F   // light salmon  #FFA07A
#define COL_HUMID     0xAED5   // soft mint     #A8D8A8
#define COL_LIGHT     0x867D   // sky blue      #87CEEB
#define COL_BORDER    0xB596   // soft gray     #B0B0B0
#define COL_RELAY_ON  0x8410   // medium gray   #808080

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
#define MQTT_SERVER "<emqx.serv.ip.addr>"
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

// ── Relay state ───────────────────────────────────────────────────────────────
bool relayOn = false;
bool relayChanged = false;   // flag: publish immediately on next mqttLoop

// ── Screen sleep ──────────────────────────────────────────────────────────────
bool screenOn = true;
unsigned long lastActivityMs = 0;
#define SLEEP_MS 300000UL  // 5 minutes

// ── Display: fixed header bar ─────────────────────────────────────────────────
void drawHeader()
{
  tft.fillRect(0, 0, 135, 34, COL_HDR_BG);
  tft.setTextColor(COL_BORDER, COL_HDR_BG);
  tft.setTextSize(2);
  tft.setTextDatum(MC_DATUM);
  tft.drawString("BED ROOM", 67, 17);
  tft.setTextDatum(TL_DATUM);
  tft.drawRoundRect(0, 0, 135, 34, 8, COL_BORDER);
}

// ── Display: IP address row (green = connected, red = offline) ───────────────
void drawIPRow()
{
  tft.fillRect(0, 34, 135, 34, TFT_BLACK);  // same height as header
  tft.setTextSize(2);
  tft.setTextDatum(MC_DATUM);
  tft.drawRoundRect(0, 34, 135, 34, 8, COL_BORDER);
  if (WiFi.status() == WL_CONNECTED)
  {
    IPAddress ip = WiFi.localIP();
    char buf[16];
    snprintf(buf, sizeof(buf), "IP: ~.%d", ip[3]);
    tft.setTextColor(COL_IP_OK, TFT_BLACK);
    tft.drawString(buf, 67, 51);
  }
  else
  {
    tft.setTextColor(COL_IP_FAIL, TFT_BLACK);
    tft.drawString("No WiFi", 67, 51);
  }
  tft.setTextDatum(TL_DATUM);
}

// ── Display: sensor row — label left 4 px, value right 4 px ──────────────────
void drawSensorRow(int y, uint16_t color, const char *label, const char *value)
{
  tft.fillRect(0, y, 135, 22, TFT_BLACK);
  tft.setTextColor(color, TFT_BLACK);
  tft.setTextSize(2);
  tft.setTextDatum(TL_DATUM);
  tft.drawString(label, 6, y + 3);          // left-aligned, 6 px from left
  tft.setTextDatum(TR_DATUM);
  tft.drawString(value, 129, y + 3);        // right-aligned, 6 px from right
  tft.setTextDatum(TL_DATUM);
}

// ── Display: relay toggle graphic ────────────────────────────────────────────
// Rect: width=99, height=34, radius=8, 2 px margin at bottom of slot.
// OFF → gray border only, gray "OFF"
// ON  → TFT_LIGHTGREY fill inset 4 px, gray border, black "ON"
void drawRelayToggle()
{
  const int RX = 0, RY = 196, RW = 135, RH = 34, RR = 8;

  tft.fillRect(0, RY, 135, 36, TFT_BLACK);             // clear slot (rect + 2 px bottom)
  if (relayOn)
    tft.fillRoundRect(RX + 4, RY + 4, RW - 8, RH - 8, RR / 2, COL_RELAY_ON);
  tft.drawRoundRect(RX, RY, RW, RH, RR, COL_BORDER);

  tft.setTextSize(2);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(relayOn ? TFT_BLACK : COL_BORDER,
                   relayOn ? COL_RELAY_ON : TFT_BLACK);
  tft.drawString(relayOn ? "ON" : "OFF", RX + RW / 2, RY + RH / 2);
  tft.setTextDatum(TL_DATUM);
}

// ── Display: refresh all sensor rows + relay status ───────────────────────────
// Layout — space-around over y=68..240 (172 px):
//   end gaps=8 px, between=18 px, item heights: T/H/L=22, Relay slot=36
void drawUI()
{
  char t_val[12], h_val[12], l_val[12];
  snprintf(t_val, sizeof(t_val), "%.2fC",  temperature);
  snprintf(h_val, sizeof(h_val), "%.2f%%", humidity);
  snprintf(l_val, sizeof(l_val), "%.2f%%", lightPct);

  drawIPRow();
  drawSensorRow(79,  COL_TEMP,  "T =", t_val);
  drawSensorRow(119, COL_HUMID, "H =", h_val);
  drawSensorRow(159, COL_LIGHT, "L =", l_val);
  drawRelayToggle();
}

// ── MQTT: per-topic publish timers (10 s ± 2 s each, independent) ────────────
static unsigned long lastTempPub = 0, nextTempMs = 0;
static unsigned long lastHumidPub = 0, nextHumidMs = 0;
static unsigned long lastLightPub = 0, nextLightMs = 0;
static unsigned long lastRelayPub = 0, nextRelayMs = 0;

// ── MQTT: format value and publish to topic ───────────────────────────────────
void mqttPublish(const char *topic, float value)
{
  char buf[12];
  snprintf(buf, sizeof(buf), "%.2f", value);
  mqtt.publish(topic, buf);
  Serial.printf("[MQTT] %s => %s\n", topic, buf);
}

// ── MQTT: incoming message handler ───────────────────────────────────────────
void mqttCallback(char *topic, byte *payload, unsigned int len)
{
  Serial.printf("[MQTT] rx  %s => %.*s\n", topic, (int)len, (char *)payload);
  if (strncmp(topic, "home/bed/control/relay", 22) != 0) return;
  if (len == 2 && strncasecmp((char *)payload, "on",  2) == 0) relayOn = true;
  if (len == 3 && strncasecmp((char *)payload, "off", 3) == 0) relayOn = false;
  relayChanged = true;
  digitalWrite(RELAY_PIN, relayOn ? LOW : HIGH);
  Serial.printf("[RELAY] %s (MQTT)\n", relayOn ? "ON" : "OFF");
  if (!screenOn)
  {
    screenOn = true;
    digitalWrite(TFT_BL_PIN, HIGH);
    Serial.println("[SCREEN] ON  (MQTT wake)");
    drawHeader();
  }
  lastActivityMs = millis();
  drawUI();
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
      if (mqtt.connect("bed-sensors"))
      {
        Serial.println("[MQTT] connected => subscribed home/bed/control/relay");
        mqtt.subscribe("home/bed/control/relay");
      }
      else
      {
        Serial.printf("[MQTT] connect failed, rc=%d\n", mqtt.state());
      }
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
    nextTempMs = random(28000, 32001);
    mqttPublish("home/bed/dht/temp", temperature);
  }
  if (now - lastHumidPub >= nextHumidMs)
  {
    lastHumidPub = now;
    nextHumidMs = random(28000, 32001);
    mqttPublish("home/bed/dht/humid", humidity);
  }
  if (now - lastLightPub >= nextLightMs)
  {
    lastLightPub = now;
    nextLightMs = random(28000, 32001);
    mqttPublish("home/bed/ldr/light", lightPct);
  }
  // immediate publish on state change, periodic heartbeat every ~2 min
  const char *relayMsg = relayOn ? "on" : "off";
  if (relayChanged)
  {
    relayChanged = false;
    lastRelayPub = now;
    nextRelayMs = random(118000, 122001);
    mqtt.publish("home/bed/button/relay", relayMsg);
    Serial.printf("[MQTT] home/bed/button/relay => %s (changed)\n", relayMsg);
  }
  else if (now - lastRelayPub >= nextRelayMs)
  {
    lastRelayPub = now;
    nextRelayMs = random(118000, 122001);
    mqtt.publish("home/bed/button/relay", relayMsg);
    Serial.printf("[MQTT] home/bed/button/relay => %s (periodic)\n", relayMsg);
  }
}

// ── Setup ─────────────────────────────────────────────────────────────────────
void setup()
{
  Serial.begin(115200);
  randomSeed(analogRead(0)); // seed from floating ADC pin for true randomness

  // relay off at boot (active low, so drive HIGH)
  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, HIGH);

  // built-in button — GPIO35 has external pull-up on TTGO T-Display
  pinMode(BTN_PIN, INPUT);

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

  // point MQTT client at broker and register callback
  mqtt.setServer(MQTT_SERVER, MQTT_PORT);
  mqtt.setCallback(mqttCallback);

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

  // GPIO35 button: debounce + wake screen or toggle relay on press
  {
    static uint8_t prevBtn   = HIGH;
    static uint8_t stableBtn = HIGH;
    static unsigned long debounceTime = 0;
    uint8_t raw = digitalRead(BTN_PIN);
    if (raw != prevBtn) { debounceTime = millis(); prevBtn = raw; }
    if ((millis() - debounceTime) > 50 && prevBtn != stableBtn)
    {
      stableBtn = prevBtn;
      if (stableBtn == LOW)                       // button pressed
      {
        lastActivityMs = millis();
        if (!screenOn)                            // wake from sleep
        {
          screenOn = true;
          digitalWrite(TFT_BL_PIN, HIGH);
          Serial.println("[SCREEN] ON  (wake)");
          drawHeader();
          drawUI();
        }
        else                                      // normal: toggle relay
        {
          relayOn = !relayOn;
          relayChanged = true;
          digitalWrite(RELAY_PIN, relayOn ? LOW : HIGH);
          Serial.printf("[RELAY] %s\n", relayOn ? "ON" : "OFF");
          drawUI();
        }
      }
    }
  }

  // screen sleep after SLEEP_MS of inactivity
  if (screenOn && (millis() - lastActivityMs > SLEEP_MS))
  {
    screenOn = false;
    digitalWrite(TFT_BL_PIN, LOW);
    Serial.println("[SCREEN] OFF (sleep)");
  }

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

    Serial.printf("[SCREEN] T=%.2f°C  H=%.2f%%  L=%.2f%%  Relay=%s  WiFi=%s\n",
                  temperature, humidity, lightPct,
                  relayOn ? "ON" : "OFF",
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
