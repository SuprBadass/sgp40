// Authors:
// * SuprBadass
// * Claude

#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <ArduinoJson.hpp>
#include <functional>

#include "inc/mbedtls/aes.h"

#include "SparkFun_SGP40_Arduino_Library.h" // Click here to get the library: http://librarymanager/All#SparkFun_SGP40
#include <Wire.h>
#include <EEPROM.h> // ESP32 core emulates EEPROM in flash; API matches classic AVR EEPROM

#include <Adafruit_NeoPixel.h>

// The SparkFun SGP40 class keeps its own VocAlgorithmParams as a private
// member, so getVOCindex() can't be combined with VocAlgorithm_get_states()/
// set_states() from a sketch. Instead we run the Sensirion algorithm
// ourselves: pull raw ticks with measureRaw() and feed them into our own
// VocAlgorithmParams via VocAlgorithm_process(). sensirion_voc_algorithm.h
// ships inside the SparkFun library, so it's already on the include path.
extern "C" {
#include "sensirion_voc_algorithm.h"
}

#include "secrets.h" // gitignored - see secrets.h.example for the template

// ----- MQTT Communication -----
#define MQTT_PORT 1883
#define HA_DISCOVERY_PREFIX "homeassistant"
#define MQTT_PUBLISH_PERIOD_MS 5000UL
#define MQTT_RECONNECT_BACKOFF_MS 5000UL

// Neopixel
#define NEOPIXEL_PIN 0
#define POWER_PIN 2

SGP40 mySensor; //create an object of the SGP40 class
VocAlgorithmParams vocParams;
WiFiClient wifiClient;

// 1 NeoPixel built into the board
Adafruit_NeoPixel strip(1, NEOPIXEL_PIN, NEO_GRB + NEO_KHZ800);

// ----- MQTT Communication -----
PubSubClient  mqtt(wifiClient);
char          deviceId[32];                 // "vocsens_aabbccddeeff"
char          telemetryTopic[64];
char          statusTopic[64];
unsigned long lastPublishMs          = 0;
unsigned long lastReconnectAttemptMs = 0;

const char* ssid = WIFI_SSID;

// Password encryption
const char key_hex[] = WIFI_PASSWORD_HEX_KEY;
const unsigned char password[] = WIFI_PASSWORD_CIPHERTEXT;

// Sensirion docs on the algorithm here:
// https://sensirion.com/media/documents/25AB572C/62B463AA/Sensirion_Engineering_Guidelines_SEN5x.pdf
const uint32_t SAMPLE_INTERVAL_MS = 1000UL;                // Sensirion algorithm expects ~1 Hz samples
const uint32_t SAVE_INTERVAL_MS = 2UL * 60UL * 60UL * 1000UL; // 2 hours
const uint32_t DELTA_WINDOW_MS = 5UL * 60UL * 1000UL; // trailing window the VOC index delta is computed over
const int DELTA_HISTORY_LEN = DELTA_WINDOW_MS / SAMPLE_INTERVAL_MS; // samples needed to span the window at 1 Hz
const int EEPROM_SIZE = 16; // rounds up to a flash sector internally, plenty for our 12-byte struct
const int EEPROM_ADDR = 0;
const uint32_t STATE_MAGIC = 0x53475034; // "SGP4" - bump this if VocState's layout ever changes


void setup_wifi(const char *ssid, const char *password) {
  delay(10);
  // We start by connecting to a WiFi network
  Serial.println();
  Serial.print("Connecting to ");
  Serial.println(ssid);

  WiFi.begin(ssid, password);

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }

  Serial.println("");
  Serial.println("WiFi connected");
  Serial.println("IP address: ");
  Serial.println(WiFi.localIP());
}

int aes_decrypt_cbc(const unsigned char *cipherText, const char *key_hex, unsigned char *outputBuffer, size_t outputBufferSize)
{
  if (outputBuffer == NULL) {
    return -1;
  }

  unsigned char key[32];
  for (int i = 0; i < 32; i++) {
      sscanf(&key_hex[i * 2], "%02hhx", &key[i]);
  }

  // // 16 bytes always — copy this before passing, it gets mutated!
  unsigned char iv[16] = {0x45, 0x73, 0xe3, 0xf0, 0x46, 0x70, 0x67, 0xdf, 0x6f, 0x61, 0x7d, 0x66, 0x22, 0x1f, 0x65, 0x2b};
  unsigned char iv_copy[16];
  memcpy(iv_copy, iv, 16);

  mbedtls_aes_context ctx;
  mbedtls_aes_init(&ctx);
  // Key size arg is in BITS: 128, 192, or 256
  mbedtls_aes_setkey_dec(&ctx, (const unsigned char*)key, 256);

  unsigned char decrypted[32] = {};
  mbedtls_aes_crypt_cbc(&ctx, MBEDTLS_AES_DECRYPT, 32, iv_copy, (const unsigned char *)cipherText, decrypted);
  mbedtls_aes_free(&ctx);

  // Last byte tells us how many padding bytes there are
  int pad_len = decrypted[31];
  decrypted[32 - pad_len] = '\0';  // null-terminate at the real end

  memcpy(outputBuffer, decrypted, outputBufferSize);
  // outputBuffer = decrypted;
  // Serial.print("Credential: ");
  // Serial.println((char *)decrypted);

  return 0;
}

// ----- MQTT Communication -----
static void buildIds() {
  uint8_t mac[6];
  WiFi.macAddress(mac);
  snprintf(deviceId, sizeof(deviceId),
           "vocsens_%02x%02x%02x%02x%02x%02x",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  snprintf(telemetryTopic, sizeof(telemetryTopic), "%s/telemetry", deviceId);
  snprintf(statusTopic,    sizeof(statusTopic),    "%s/status",    deviceId);
}

// ----- MQTT Communication -----
// Publish one discovery config. `fill` layers on entity-specific fields.
static void publishDiscovery(const char* component,
                             const char* objectId,
                             std::function<void(JsonDocument&)> fill) {
  StaticJsonDocument<768> cfg;

  // Shared wiring
  cfg["state_topic"]           = telemetryTopic;
  cfg["availability_topic"]    = statusTopic;
  cfg["payload_available"]     = "online";
  cfg["payload_not_available"] = "offline";

  char uid[72];
  snprintf(uid, sizeof(uid), "%s_%s", deviceId, objectId);
  cfg["unique_id"] = uid;

  // Device block — groups all entities under one HA device card.
  JsonObject dev = cfg.createNestedObject("device");
  JsonArray  ids = dev.createNestedArray("identifiers");
  ids.add(deviceId);
  dev["name"]         = "VOC Sensor";
  dev["manufacturer"] = "DIY";
  dev["model"]        = "ESP32 Air Quality Sensor";

  fill(cfg);

  char topic[128];
  snprintf(topic, sizeof(topic), "%s/%s/%s/%s/config",
           HA_DISCOVERY_PREFIX, component, deviceId, objectId);

  char payload[768];
  const size_t n = serializeJson(cfg, payload, sizeof(payload));
  mqtt.publish(topic, (const uint8_t*)payload, n, /*retained=*/true);
}

// ----- MQTT Communication -----
static void publishAllDiscovery() {
  publishDiscovery("sensor", "voc_index", [](JsonDocument& c){
    c["name"]                        = "VOC Index";
    c["value_template"]              = "{{ value_json.voc_index }}";
    c["unit_of_measurement"]         = "ppm";                               // The sensor doesnt actually use ppm (instead, a unitless VOC index) but this is the only option
    c["device_class"]                = "volatile_organic_compounds_parts";
    c["state_class"]                 = "measurement";
    c["suggested_display_precision"] = 2;
  });

  publishDiscovery("sensor", "voc_index_delta_5m", [](JsonDocument& c){
    c["name"]           = "VOC Index Delta (5m)";
    c["value_template"] = "{{ value_json.voc_index_delta_5m }}";
    c["state_class"]    = "measurement";
    c["icon"]           = "mdi:delta";
  });
}

// ----- MQTT Communication -----
static bool mqttReconnect() {
  if (mqtt.connected())                   return true;
  if (WiFi.status() != WL_CONNECTED)      return false;

  const unsigned long now = millis();
  if (now - lastReconnectAttemptMs < MQTT_RECONNECT_BACKOFF_MS) return false;
  lastReconnectAttemptMs = now;

  // LWT: broker auto-publishes "offline" to statusTopic on ungraceful disconnect.
  // Retained so HA sees the last known state even after an HA restart.
  const bool ok = mqtt.connect(
      deviceId, MQTT_USER, MQTT_PASS,
      statusTopic, /*willQos=*/1, /*willRetain=*/true, "offline");

  if (!ok) {
    Serial.print(F("MQTT connect failed, rc="));
    Serial.println(mqtt.state());
    return false;
  }

  Serial.println(F("MQTT connected"));
  mqtt.publish(statusTopic, "online", /*retained=*/true);
  publishAllDiscovery();
  return true;
}

// ----- MQTT Communication -----
static void publishTelemetry(int vocIndex, int32_t vocDelta) {

  StaticJsonDocument<384> doc;
  doc["voc_index"]          = vocIndex;
  doc["voc_index_delta_5m"] = vocDelta;

  char payload[384];
  const size_t n = serializeJson(doc, payload, sizeof(payload));
  mqtt.publish(telemetryTopic, (const uint8_t*)payload, n);
}

// ----- MQTT Communication -----
void mqttSetup() {
  // Don't block here — mqttLoop() will simply no-op until WiFi is up.
  buildIds();
  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setBufferSize(1024);   // default 256 is too small for discovery payloads
  mqtt.setKeepAlive(30);
}

// ----- MQTT Communication -----
// Non-blocking. Call every loop() iteration; it rate-limits itself.
void mqttLoop(int vocIndex, int32_t vocDelta) {

  if (!mqttReconnect()) return;
  mqtt.loop();

  const unsigned long now = millis();
  if (now - lastPublishMs >= MQTT_PUBLISH_PERIOD_MS) {
    lastPublishMs = now;
    publishTelemetry(vocIndex, vocDelta);
  }
}

struct VocState
{
  uint32_t magic;
  int32_t state0;
  int32_t state1;
};

unsigned long lastSampleMs = 0;
unsigned long lastSaveMs = 0;

int32_t vocHistory[DELTA_HISTORY_LEN]; // ring buffer of the last DELTA_WINDOW_MS worth of VOC index samples
int     vocHistoryHead   = 0;          // next slot to write
int     vocHistoryCount  = 0;          // samples written so far, caps at DELTA_HISTORY_LEN
int32_t vocDelta         = 0;          // published delta; 0 until warmed up and the window is full

bool loadVocState()
{
  bool ret = 0;
  VocState saved;
  EEPROM.get(EEPROM_ADDR, saved);

  if (saved.magic == STATE_MAGIC)
  {
    VocAlgorithm_set_states(&vocParams, saved.state0, saved.state1);
    Serial.println(F("Restored VOC algorithm state from EEPROM"));
    neopixel(100, 200, 127, 0);

    ret = 1;
  }
  else
  {
    Serial.println(F("No saved VOC state found in EEPROM, starting fresh"));
    ret = 0;
  }

  return ret;
}

void saveVocState()
{
  VocState toSave;
  toSave.magic = STATE_MAGIC;
  VocAlgorithm_get_states(&vocParams, &toSave.state0, &toSave.state1);

  EEPROM.put(EEPROM_ADDR, toSave);
  EEPROM.commit(); // required on ESP32 - writes are buffered in RAM until this is called

  Serial.print(F("Saved VOC algorithm state (state0="));
  Serial.print(toSave.state0);
  Serial.print(F(", state1="));
  Serial.print(toSave.state1);
  Serial.println(F(")"));
}

// Updates vocDelta with the change in VOC index over the trailing DELTA_WINDOW_MS
// window (a rolling window: recomputed every sample against the reading from
// ~DELTA_WINDOW_MS ago, not just once per window - so a spike can't hide between
// two snapshots and shows up with ~1 sample of latency). Reports 0 until two
// things are both true: uptime has passed SAVE_INTERVAL_MS at least once (a
// one-time latch - not re-checked periodically - giving the algorithm's baseline
// time to stabilize after boot), and the ring buffer holds a full window of
// history.
void updateVocDelta(int32_t vocIndex, unsigned long now)
{
  static bool vocDeltaWarmedUp = false; // latches true once uptime has passed SAVE_INTERVAL_MS - never unlatches

  if (!vocDeltaWarmedUp && now >= SAVE_INTERVAL_MS)
  {
    vocDeltaWarmedUp = true;
  }

  int32_t vocIndex5MinAgo = vocHistory[vocHistoryHead]; // oldest sample, about to be overwritten
  bool windowFull = (vocHistoryCount == DELTA_HISTORY_LEN);

  vocHistory[vocHistoryHead] = vocIndex;
  vocHistoryHead++;
  if (vocHistoryHead >= DELTA_HISTORY_LEN) vocHistoryHead = 0;
  if (vocHistoryCount < DELTA_HISTORY_LEN) vocHistoryCount++;

  vocDelta = (vocDeltaWarmedUp && windowFull) ? (vocIndex - vocIndex5MinAgo) : 0;
}

void neopixel(uint8_t brightness, uint8_t red, uint8_t green, uint8_t blue)
{
  strip.setPixelColor(0, strip.Color(red, green, blue));
  strip.setBrightness(brightness);
  strip.show();
}

void setup()
{
  Serial.begin(115200);
  Serial.println(F("SGP40 Program - EEPROM state persistence"));

  Wire.begin();

  if (EEPROM.begin(EEPROM_SIZE) == false)
  {
    Serial.println(F("Failed to initialize EEPROM. Freezing..."));
    while (1)
      ;
  }

  //mySensor.enableDebugging(); // Uncomment this line to print useful debug messages to Serial

  //Initialize sensor
  if (mySensor.begin() == false)
  {
    Serial.println(F("SGP40 not detected. Check connections. Freezing..."));
    while (1)
      ; // Do nothing more
  }

  VocAlgorithm_init(&vocParams);
  bool loadSuccess = loadVocState(); // overwrites the fresh init state if we have a saved one

  lastSampleMs = millis();
  lastSaveMs = millis();

  // Enable power to the NeoPixel / STEMMA QT port
  pinMode(POWER_PIN, OUTPUT);
  digitalWrite(POWER_PIN, HIGH);
  strip.begin();

  if(loadSuccess == 0) {
    neopixel(100, 255, 0, 0);
  }
}

void loop()
{
  unsigned char decrypted[32] = {};

  delay(1000);
  aes_decrypt_cbc(password, key_hex, decrypted, sizeof(decrypted));
  // Serial.print("Credential: ");
  // Serial.println((char *)decrypted);
  setup_wifi(ssid, (const char *)decrypted);

  mqttSetup();

  while(1) {
    unsigned long now = millis();

    // Sample at ~1 Hz, as the Sensirion algorithm expects
    if (now - lastSampleMs >= SAMPLE_INTERVAL_MS)
    {
      lastSampleMs = now;

      uint16_t srawTicks;
      int32_t vocIndex;
      if (mySensor.measureRaw(&srawTicks) == SGP40_SUCCESS) //default RH (50%) and T (25C)
      {
        VocAlgorithm_process(&vocParams, srawTicks, &vocIndex);
        updateVocDelta(vocIndex, now);
        Serial.print(F("VOC Index is: "));
        Serial.println(vocIndex);
        Serial.print(F("VOC Index Delta (5m) is: "));
        Serial.println(vocDelta);

        mqttLoop(vocIndex, vocDelta);
      }
      else
      {
        Serial.println(F("measureRaw() failed"));
      }
    }

    // Persist algorithm state every 4 hours so it survives a reboot/power loss
    if (now - lastSaveMs >= SAVE_INTERVAL_MS)
    {
      lastSaveMs = now;
      saveVocState();
      neopixel(100, 0, 255, 0);
    }
  }
}
