
// === Libraries ===
#include <WiFi.h>
#include <ArduinoJson.h>
#include <DHT.h>

#include <Firebase_ESP_Client.h>          // Firebase ESP Client
#include "addons/TokenHelper.h"           // Helper for OAuth tokens (required)
#include "addons/RTDBHelper.h"           // Realtime DB helper (optional)

// === Wi-Fi ===
const char* WIFI_SSID     = "yourwifi";
const char* WIFI_PASSWORD = "yourpassword";

// === Firebase ===
// Replace with your Firebase project values
#define FIREBASE_API_KEY     "AIzaSyCVv0rBh_jwfpnlWo3ZelaQmDxbFZugc94"
#define FIREBASE_DATABASE_URL "https://heritech-museum-default-rtdb.asia-southeast1.firebasedatabase.app/"  // include trailing slash

FirebaseData fbdo;
FirebaseAuth auth;
FirebaseConfig config;

// === Pins & Sensors ===
#define LIGHT_PIN   32
#define UV_PIN      33
#define GAS_PIN     35   // input-only (ADC)
#define VIB_PIN     25
#define BUZZER_PIN  26
#define LED_PIN     27
#define DHT11_PIN   23
#define DHTTYPE     DHT11

DHT dht(DHT11_PIN, DHTTYPE);

// === Timers / config ===
const uint32_t PUBLISH_INTERVAL_MS = 10000;

// UV smoothing
#define UV_SAMPLES_PER_READ 16
#define UV_MICROS_BETWEEN   200
#define UV_EMA_CUTOFF_SEC   1.5f
#define UV_VREF             3.3f

// Gas calibration
#define GAS_RAW_MIN_CAL 830
#define GAS_RAW_MAX_CAL 4095

// Severity enums
enum { SEV_OK=0, SEV_WARN=1, SEV_CRIT=2 };

// Temperature thresholds (°C)
#define TEMP_WARN_HI   36.0f
#define TEMP_WARN_LO   24.0f
#define TEMP_CRIT_HI   39.0f
#define TEMP_CRIT_LO   15.0f
#define TEMP_HYS        0.6f

// Humidity (%RH)
#define HUM_WARN_HI    80.0f
#define HUM_WARN_LO    35.0f
#define HUM_CRIT_HI    90.0f
#define HUM_CRIT_LO    25.0f
#define HUM_HYS         2.0f

// Gas index thresholds
#define GAS_WARN_IDX   103
#define GAS_CRIT_IDX   150
#define GAS_HYS         15

// UV thresholds (Volt)
#define UV_WARN_V      1.40f
#define UV_CRIT_V      2.00f
#define UV_HYS_V       0.05f

// Direct sun heuristics
#define SUN_LIGHT_WARN_COUNTS   4000
#define SUN_LIGHT_CLEAR_COUNTS  3800
#define SUN_UV_WARN_MWCM2       1.20f
#define SUN_UV_CLEAR_MWCM2      1.00f

// Vibration
#define VIB_DEBOUNCE_MS         50
#define VIB_COUNT_THRESHOLD     10
#define VIB_RESET_IDLE_MS     2000
#define VIB_LATCH_MS          5000

// LED/Buzzer patterns
#define LED_INTERVAL_WARN   200
#define LED_INTERVAL_CRIT    50
#define BUZZER_BEEP_WARN    250
#define BUZZER_BEEP_CRIT     50

#define BUZZER_ACTIVE_LOW   1

// === State ===
uint32_t tPublish=0;
uint32_t tLedBlink=0;
bool     ledState=false;

bool isAlarmActive=false;
bool muteBuzzer=false;
int  severityLevel=SEV_OK;

// Hysteresis states
bool tempWarn=false, tempCrit=false;
bool humWarn=false,  humCrit=false;
bool gasWarn=false,  gasCrit=false;
bool uvWarn=false,   uvCrit=false;

uint32_t vibLastEdgeMs=0;
uint32_t vibLastPulseMs=0;
uint16_t vibPulseCount=0;
uint32_t vibLatchedUntil=0;

float uvEmaVolt = NAN;
uint32_t uvEmaLastMs = 0;

bool sunWarn = false;

// === Helpers ===
static inline void buzzerOn()  { digitalWrite(BUZZER_PIN, BUZZER_ACTIVE_LOW ? LOW : HIGH); }
static inline void buzzerOff() { digitalWrite(BUZZER_PIN, BUZZER_ACTIVE_LOW ? HIGH: LOW ); }

static inline int16_t median5(int16_t a, int16_t b, int16_t c, int16_t d, int16_t e) {
  int16_t arr[5] = { a,b,c,d,e };
  for (int i=0;i<4;i++) for (int j=i+1;j<5;j++) if (arr[j]<arr[i]) { int16_t t=arr[i]; arr[i]=arr[j]; arr[j]=t; }
  return arr[2];
}

uint16_t readUvRawSmoothedOnce() {
  int16_t g1[5], g2[5], g3[5];
  for (int i=0;i<5;i++){ g1[i]=analogRead(UV_PIN); delayMicroseconds(UV_MICROS_BETWEEN); }
  for (int i=0;i<5;i++){ g2[i]=analogRead(UV_PIN); delayMicroseconds(UV_MICROS_BETWEEN); }
  for (int i=0;i<5;i++){ g3[i]=analogRead(UV_PIN); delayMicroseconds(UV_MICROS_BETWEEN); }
  int16_t m1=median5(g1[0],g1[1],g1[2],g1[3],g1[4]);
  int16_t m2=median5(g2[0],g2[1],g2[2],g2[3],g2[4]);
  int16_t m3=median5(g3[0],g3[1],g3[2],g3[3],g3[4]);
  int16_t s16 = analogRead(UV_PIN);
  return (uint16_t)((m1 + m2 + m3 + s16)/4);
}

float uvFilterEMA(float volt) {
  uint32_t now = millis();
  float dt = (uvEmaLastMs==0) ? 0 : (now - uvEmaLastMs)/1000.0f;
  uvEmaLastMs = now;
  float tau = UV_EMA_CUTOFF_SEC;
  float alpha = (dt<=0) ? 1.0f : (1.0f - expf(-dt/tau));
  if (isnan(uvEmaVolt)) uvEmaVolt = volt;
  uvEmaVolt += alpha * (volt - uvEmaVolt);
  return uvEmaVolt;
}

static inline float uvVoltToIrradiance_mWcm2(float vout) {
  float current_uA = vout / 4.3f;
  float mWcm2 = current_uA / 0.112f;
  if (!isfinite(mWcm2) || mWcm2 < 0) mWcm2 = 0;
  return mWcm2;
}

static inline int max3(int a, int b, int c){ return max(a, max(b,c)); }

// Severity evaluation (with hysteresis & vibration)
int evaluateSeverity(float temp, float hum, int gasIdx, float uvVolt,
                     int lightCounts, float uv_mWcm2,
                     bool vibRaw, uint32_t nowMs) {
  // Temperature
  if (!tempCrit && (temp >= TEMP_CRIT_HI || temp <= TEMP_CRIT_LO)) tempCrit = true;
  if ( tempCrit && (temp <= TEMP_CRIT_HI - TEMP_HYS && temp >= TEMP_CRIT_LO + TEMP_HYS)) tempCrit = false;
  if (!tempWarn && (temp >= TEMP_WARN_HI || temp <= TEMP_WARN_LO)) tempWarn = true;
  if ( tempWarn && (temp <= TEMP_WARN_HI - TEMP_HYS && temp >= TEMP_WARN_LO + TEMP_HYS)) tempWarn = false;
  int sevTemp = tempCrit ? SEV_CRIT : (tempWarn ? SEV_WARN : SEV_OK);

  // Humidity
  if (!humCrit && (hum >= HUM_CRIT_HI || hum <= HUM_CRIT_LO)) humCrit = true;
  if ( humCrit && (hum <= HUM_CRIT_HI - HUM_HYS && hum >= HUM_CRIT_LO + HUM_HYS)) humCrit = false;
  if (!humWarn && (hum >= HUM_WARN_HI || hum <= HUM_WARN_LO)) humWarn = true;
  if ( humWarn && (hum <= HUM_WARN_HI - HUM_HYS && hum >= HUM_WARN_LO + HUM_HYS)) humWarn = false;
  int sevHum = humCrit ? SEV_CRIT : (humWarn ? SEV_WARN : SEV_OK);

  // Gas
  if (!gasCrit && gasIdx >= GAS_CRIT_IDX) gasCrit = true;
  if ( gasCrit && gasIdx <= GAS_CRIT_IDX - GAS_HYS) gasCrit = false;
  if (!gasWarn && gasIdx >= GAS_WARN_IDX) gasWarn = true;
  if ( gasWarn && gasIdx <= GAS_WARN_IDX - GAS_HYS) gasWarn = false;
  int sevGas = gasCrit ? SEV_CRIT : (gasWarn ? SEV_WARN : SEV_OK);

  // UV
  if (!uvCrit && uvVolt >= UV_CRIT_V) uvCrit = true;
  if ( uvCrit && uvVolt <= UV_CRIT_V - UV_HYS_V) uvCrit = false;
  if (!uvWarn && uvVolt >= UV_WARN_V) uvWarn = true;
  if ( uvWarn && uvVolt <= UV_WARN_V - UV_HYS_V) uvWarn = false;
  int sevUV = uvCrit ? SEV_CRIT : (uvWarn ? SEV_WARN : SEV_OK);

  // Direct Sun
  if (!sunWarn && (lightCounts >= SUN_LIGHT_WARN_COUNTS)) {
    sunWarn = true;
  } else if (sunWarn && (lightCounts <= SUN_LIGHT_CLEAR_COUNTS)) {
    sunWarn = false;
  }
  int sevSun = sunWarn ? SEV_WARN : SEV_OK;

  // Vibration: debounce + count + latch
  static bool vibStable = false;
  bool vibLevelRaw = (vibRaw == LOW);

  if (vibLevelRaw != vibStable) {
    if (nowMs - vibLastEdgeMs >= VIB_DEBOUNCE_MS) {
      vibStable = vibLevelRaw;
      vibLastEdgeMs = nowMs;
      if (vibStable) {
        if (nowMs - vibLastPulseMs > VIB_RESET_IDLE_MS) {
          vibPulseCount = 0;
        }
        vibPulseCount++;
        vibLastPulseMs = nowMs;
        if (vibPulseCount >= VIB_COUNT_THRESHOLD) {
          vibPulseCount = 0;
          vibLatchedUntil = nowMs + VIB_LATCH_MS;
        }
      }
    }
  }

  bool vibActive = (nowMs < vibLatchedUntil);
  int sevVib = vibActive ? SEV_WARN : SEV_OK;

  return max3(max3(sevTemp, sevHum, sevGas), max(sevUV, sevSun), sevVib);
}

// === Setup / Loop Helpers ===
void connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("WiFi: ");
  while (WiFi.status() != WL_CONNECTED) { delay(400); Serial.print("."); }
  Serial.print(" connected, IP="); Serial.println(WiFi.localIP());
}

// (No MQTT callbacks now)
// Setup Firebase connection
void setupFirebase() {
  config.api_key = FIREBASE_API_KEY;
  config.database_url = FIREBASE_DATABASE_URL;

  // Try to sign up anonymously (helps if no auth configured)
  // If you use a service account or other auth, follow different flow
  if (Firebase.signUp(&config, &auth, "", "")) {
    Serial.println("Firebase: signUp OK");
  } else {
    Serial.printf("Firebase: signUp failed: %s\n", config.signer.signupError.message.c_str());
    // continue anyway, if rules allow unauthenticated writes
  }

  Firebase.begin(&config, &auth);
  Firebase.reconnectWiFi(true);
}

void setup() {
  Serial.begin(115200);
  dht.begin();

  pinMode(VIB_PIN, INPUT_PULLUP);
  pinMode(LED_PIN, OUTPUT);  digitalWrite(LED_PIN, LOW);
  pinMode(BUZZER_PIN, OUTPUT); buzzerOff();

  analogSetPinAttenuation(LIGHT_PIN, ADC_11db);
  analogSetPinAttenuation(UV_PIN,    ADC_11db);
  analogSetPinAttenuation(GAS_PIN,   ADC_11db);

  connectWiFi();

  // Configure Firebase client
  setupFirebase();
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) connectWiFi();

  const uint32_t now = millis();

  // ---- Read sensors ----
  float hum = dht.readHumidity();
  float temp = dht.readTemperature();
  bool dhtOK = !(isnan(hum) || isnan(temp));

  int rawGas = analogRead(GAS_PIN);
  int gasMapped = map(rawGas, GAS_RAW_MIN_CAL, GAS_RAW_MAX_CAL, 0, 1023);
  gasMapped = constrain(gasMapped, 0, 1023);

  int lightRaw = analogRead(LIGHT_PIN);
  int lightVal = 4095 - lightRaw; // brighter → higher

  uint32_t acc=0;
  for (int i=0;i<UV_SAMPLES_PER_READ;i++) acc += readUvRawSmoothedOnce();
  uint16_t uvRaw = acc / UV_SAMPLES_PER_READ;

  float uvVoltageInstant = uvRaw * (UV_VREF / 4095.0f);
  float uvVoltage = uvFilterEMA(uvVoltageInstant);
  float uv_mWcm2 = uvVoltToIrradiance_mWcm2(uvVoltage);

  bool vibration = (digitalRead(VIB_PIN) == LOW);

  // ---- Evaluate severity & alarm ----
  float tempUse = dhtOK ? temp : (TEMP_WARN_HI - TEMP_HYS);
  float humUse  = dhtOK ? hum  : (HUM_WARN_HI - HUM_HYS);

  severityLevel = evaluateSeverity(tempUse, humUse, gasMapped, uvVoltage,
                                   lightVal, uv_mWcm2,
                                   vibration, now);
  isAlarmActive = (severityLevel != SEV_OK);

  // ---- LED pattern ----
  uint32_t ledInterval = (severityLevel==SEV_CRIT) ? LED_INTERVAL_CRIT :
                         (severityLevel==SEV_WARN) ? LED_INTERVAL_WARN  : 0;

  if (severityLevel == SEV_OK) {
    digitalWrite(LED_PIN, LOW);
    ledState = false;
  } else {
    if (now - tLedBlink >= ledInterval) {
      tLedBlink = now;
      ledState = !ledState;
      digitalWrite(LED_PIN, ledState);
    }
  }

  // ---- Buzzer pattern ----
  if (isAlarmActive && !muteBuzzer) {
    if (severityLevel == SEV_CRIT) {
      if ((now % 100) < BUZZER_BEEP_CRIT) buzzerOn(); else buzzerOff();
    } else if (severityLevel == SEV_WARN) {
      if ((now % 500) < BUZZER_BEEP_WARN) buzzerOn(); else buzzerOff();
    }
  } else {
    buzzerOff();
  }

  // ---- Publish to Firebase ----
  if (now - tPublish >= PUBLISH_INTERVAL_MS) {
    tPublish = now;

    // Build FirebaseJson payload
    FirebaseJson json;

    if (dhtOK) {
      json.set("nhietDo", temp);
      json.set("doAm", hum);
    } else {
    json.set("nhietDo", "null");
    json.set("doAm", "null");
    }

    json.set("anhSang", lightVal);
    json.set("khiGas", gasMapped);
    json.set("khiGasRaw", rawGas);

    json.set("tiaUV", uvVoltage);
    json.set("uv_mWcm2", uv_mWcm2);
    json.set("uvRaw", uvRaw);

    json.set("sunDirect", (int)sunWarn);   // 0 or 1
    json.set("rungChan", (vibration ? 1 : 0));
    json.set("severity", severityLevel);
    json.set("canhBao", isAlarmActive);
    json.set("rssi", (int)WiFi.RSSI());

    // Write to path: /museum_data/<MAC>  (safe key)
    String mac = WiFi.macAddress();
    mac.replace(":", "_"); // Firebase path keys: avoid colon
    String path = "/museum_data/" + mac + "/last";

    // Attempt write
    if (Firebase.RTDB.setJSON(&fbdo, path.c_str(), &json)) {
      // optionally write timestamp or increment counter
      // Serial print returned JSON or success
      Serial.print("Firebase write OK: ");
      Serial.println(path);
    } else {
      Serial.print("Firebase write failed: ");
      Serial.println(fbdo.errorReason());
    }

    // Optionally also push historical entry with timestamp
    // get server timestamp (client side)
    String histPath = "/museum_data/" + mac + "/history";
    // create object with timestamp key = millis() or better use push
    if (Firebase.RTDB.pushJSON(&fbdo, histPath.c_str(), &json)) {
      // success
    } else {
      // failed (ok to ignore in low memory)
    }
  }
}
