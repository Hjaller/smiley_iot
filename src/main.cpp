#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <esp_sleep.h>
#include <esp_sntp.h>
#include <esp_timer.h>
#include <sys/time.h>
#include <time.h>

#include "secrets.h"   // WIFI_SSID, WIFI_PASSWORD, MQTT_USER, MQTT_PASSWORD (not in git)
#include "ca_cert.h"   // CA_CERT - the broker's CA certificate

// Smiley feedback panel
//
// Flow:
//   1. A button press wakes the ESP32 from deep sleep (ext1). The LED lights up
//      immediately and the press is stored with a timestamp in RTC memory.
//   2. WiFi -> NTP -> MQTT over TLS (port 8883, CA check, username/password).
//   3. The press is sent as JSON. The panel is locked for 7 s while the LED is lit.
//   4. After IDLE_MS without presses it goes back to deep sleep.
//   5. Routine wake-up every ROUTINE_SLEEP_S: NTP sync + status message.
//
// Presses that cannot be sent (e.g. no network) stay in the RTC queue
// and are sent on the next wake-up.

// ---------------------------------------------------------------------------
// Settings
// ---------------------------------------------------------------------------
// *** SECURITY ***
//   1 = TLS encryption + username/password, port 8883  (REQUIRED by the assignment)
//   0 = TEMPORARY: unencrypted, no login, port 1883
#ifndef USE_TLS
#define USE_TLS 1
#endif

#if USE_TLS
const uint16_t MQTT_PORT = 8883;
#else
#warning "USE_TLS=0: MQTT is UNENCRYPTED and without login - remember to switch back to 1!"
const uint16_t MQTT_PORT = 1883;
#endif

const char* MQTT_HOST = "wilsons.local";   // host name (not IP) - used for the certificate check

const char* TIMEZONE     = "CET-1CEST,M3.5.0,M10.5.0/3";   // Denmark incl. daylight saving time
const char* NTP_SERVER_1 = "pool.ntp.org";
const char* NTP_SERVER_2 = "time.google.com";

const uint32_t DEBOUNCE_MS     = 50;
const uint32_t LOCK_MS         = 7000;     // lock-out after a press (required: 7 s)
const uint32_t IDLE_MS         = 10000;    // awake time without activity before deep sleep -> determined by experiments
const uint64_t ROUTINE_SLEEP_S = 6ULL * 3600;  // routine wake-up (NTP + status)        -> determined by experiments
const uint32_t NET_TIMEOUT_MS  = 20000;    // give up on the network and sleep (presses stay in the queue)
const uint32_t NTP_WAIT_MS     = 17000;    // max wait (from WiFi up) for NTP before a press is sent with RTC time.
                                           // Awake >= LOCK_MS + IDLE_MS = 17 s after a press anyway, so waiting is ~free.
                                           // RTC drift ~1.3 % -> up to ~5 min error after 6 h sleep without NTP.
const uint32_t MQTT_RETRY_MS   = 3000;
const uint32_t TLS_TIMEOUT_MS  = 8000;     // max time for TCP connect (default 30 s). DNS lookup is NOT covered (~15 s if it fails)

//                          green blue yellow red
const int   BUTTON_PIN[4] = {  32,  33,   25,  26 };   // all are RTC GPIOs -> can wake from deep sleep
const int   LED_PIN[4]    = {  23,  22,   21,  19 };
const char* NAME[4]       = { "green", "blue", "yellow", "red" };
const int   VALUE[4]      = {    4,      3,      2,       1  };  // 4 = most satisfied

#define LOG(fmt, ...) Serial.printf("[%7lu ms] " fmt "\n", (unsigned long)millis(), ##__VA_ARGS__)

// ---------------------------------------------------------------------------
// RTC memory - survives deep sleep (cleared on power-on/reset)
// ---------------------------------------------------------------------------
struct Press {
  uint8_t  button;
  uint32_t session;        // bootCount when the press happened
  uint32_t millisAtPress;  // for NTP correction of the timestamp in the same session
  int64_t  epochMs;        // time of the press (0 = unknown)
  bool     ntpCorrected;   // epochMs has been corrected by NTP (before deep sleep)
};

const int QUEUE_MAX = 32;
RTC_DATA_ATTR Press    queue[QUEUE_MAX];
RTC_DATA_ATTR int      queueCount    = 0;
RTC_DATA_ATTR uint32_t bootCount     = 0;
RTC_DATA_ATTR uint32_t prevAwakeMs   = 0;      // for the power consumption analysis
RTC_DATA_ATTR int64_t  sleepStartMs  = 0;      // clock time when going to deep sleep (drift measurement)
RTC_DATA_ATTR bool     sleepStartNtp = false;  // was the clock NTP-synced when going to sleep?

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
#if USE_TLS
WiFiClientSecure netClient;   // TLS
#else
WiFiClient       netClient;   // unencrypted (temporary)
#endif
PubSubClient     mqtt(netClient);

char clientId[48];
char topicPress[96];
char topicStatus[96];

volatile bool ntpSynced = false;
bool ntpStarted         = false;
bool loginRejected      = false;   // rc=4/5: give up MQTT for the rest of this wake-up
uint32_t wifiConnectedMs = 0;
uint32_t lastMqttAttempt = 0;

bool routineWake   = false;
bool statusPending = false;
const char* statusEvent = "";

// Buttons and lock run in esp_timer callbacks (the esp_timer task), NOT in loop().
// DNS + TLS can block loop() for up to ~15 s (test 15), but buttons are still read
// every 5 ms and the LED turns off after exactly 7 s.
const uint32_t BUTTON_POLL_MS = 5;
esp_timer_handle_t lockTimer, buttonTimer;
volatile bool locked       = false;
volatile int  activeLed    = -1;
volatile uint32_t lastActivity = 0;
portMUX_TYPE queueMux = portMUX_INITIALIZER_UNLOCKED;   // queue is shared by esp_timer task and loop()

int64_t  rtcAtStartMs  = 0;                   // clock time at program start (before NTP)
uint32_t millisAtStart = 0;
bool     driftLogged   = false;

volatile int raw[4], stable[4];
uint32_t lastChange[4];

// ---------------------------------------------------------------------------
// Time
// ---------------------------------------------------------------------------
int64_t nowMs() {
  struct timeval tv;
  gettimeofday(&tv, nullptr);
  return (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

bool timeValid() { return time(nullptr) > 1700000000; }   // after Nov 2023 = set

// ISO 8601 with milliseconds and time zone, e.g. 2026-11-24T10:15:03.123+01:00
void formatTime(int64_t ms, char* buf, size_t len) {
  time_t s = ms / 1000;
  struct tm t;
  localtime_r(&s, &t);
  char date[24], zone[8];
  strftime(date, sizeof date, "%Y-%m-%dT%H:%M:%S", &t);
  strftime(zone, sizeof zone, "%z", &t);                 // +0100
  snprintf(buf, len, "%s.%03d%.3s:%s", date, (int)(ms % 1000), zone, zone + 3);
}

void ntpCallback(struct timeval*) { ntpSynced = true; }

// First NTP sync in a wake-up: how much did the RTC clock drift during deep sleep?
void logDrift() {
  if (!ntpSynced || driftLogged) return;
  driftLogged = true;
  int64_t  now   = nowMs();
  uint32_t since = millis() - millisAtStart;
  if (rtcAtStartMs == 0) {
    LOG("NTP sync - clock was not set before");
    return;
  }
  int64_t drift = rtcAtStartMs + since - now;     // + = RTC clock ran fast
  if (sleepStartMs > 0) {
    double sleepS = (double)(now - since - sleepStartMs) / 1000.0;
    LOG("NTP sync - RTC clock was off by %+lld ms after %.1f s deep sleep (%.2f %%)%s",
        (long long)drift, sleepS, sleepS > 0 ? 100.0 * drift / (sleepS * 1000.0) : 0.0,
        sleepStartNtp ? "" : " [uncertain: clock was not NTP-synced when going to sleep]");
  } else {
    LOG("NTP sync - RTC clock was off by %+lld ms", (long long)drift);
  }
}

// ---------------------------------------------------------------------------
// Buttons, debounce and lock
// ---------------------------------------------------------------------------
void lockExpired(void*) {                     // esp_timer callback after LOCK_MS
  if (activeLed >= 0) digitalWrite(LED_PIN[activeLed], LOW);
  activeLed = -1;
  lastActivity = millis();
  locked = false;
  LOG("Lock released - ready for next press");
}

void registerPress(int i) {
  locked = true;
  activeLed = i;
  lastActivity = millis();
  routineWake = false;   // now it is a "real" wake-up
  for (int j = 0; j < 4; j++) digitalWrite(LED_PIN[j], j == i ? HIGH : LOW);
  esp_timer_start_once(lockTimer, (uint64_t)LOCK_MS * 1000);

  Press p = { (uint8_t)i, bootCount, millis(), timeValid() ? nowMs() : 0, false };
  bool dropped = false;
  portENTER_CRITICAL(&queueMux);
  if (queueCount == QUEUE_MAX) {      // queue full: drop the oldest
    memmove(&queue[0], &queue[1], sizeof(Press) * (QUEUE_MAX - 1));
    queueCount--;
    dropped = true;
  }
  queue[queueCount++] = p;
  portEXIT_CRITICAL(&queueMux);
  if (dropped) LOG("Queue full - oldest press dropped");
  LOG("Press: %s (value %d) - locked for %lu s", NAME[i], VALUE[i], LOCK_MS / 1000);
}

void readButtons() {
  uint32_t now = millis();
  for (int i = 0; i < 4; i++) {
    int r = digitalRead(BUTTON_PIN[i]);      // HIGH = pressed (pull-down)
    if (r != raw[i]) {                       // signal is changing -> start over
      raw[i] = r;
      lastChange[i] = now;
    } else if (r != stable[i] && now - lastChange[i] >= DEBOUNCE_MS) {
      stable[i] = r;                         // stable for DEBOUNCE_MS -> accept
      if (r == HIGH) {
        if (!locked) registerPress(i);
        else LOG("%s ignored (locked)", NAME[i]);
      } else {
        lastActivity = now;                  // release counts as activity: sleep IDLE_MS after it (test 17)
      }
    }
  }
}

void pollButtons(void*) { readButtons(); }    // esp_timer callback every BUTTON_POLL_MS

// ---------------------------------------------------------------------------
// MQTT
// ---------------------------------------------------------------------------
void mqttError() {
  int rc = mqtt.state();
  if (rc == MQTT_CONNECT_BAD_CREDENTIALS || rc == MQTT_CONNECT_UNAUTHORIZED) {
    loginRejected = true;                      // the connection worked, but the login was rejected
    LOG("MQTT login rejected (rc=%d) - check MQTT_USER/MQTT_PASSWORD. Retrying on next wake-up", rc);
    return;
  }
#if USE_TLS
  if (rc == MQTT_CONNECT_FAILED) {             // TCP/TLS failed - show the reason
    char err[128] = "";
    netClient.lastError(err, sizeof err);
    LOG("MQTT error rc=%d (connection/TLS): %s", rc, err);
    return;
  }
#endif
  LOG("MQTT error rc=%d", rc);
}

bool readyToSend(const Press& p) {
  // Presses from earlier wake-ups already have their best timestamp.
  if (p.session != bootCount || ntpSynced) return true;
  // No clock at all (after power-on/reset): wait for NTP instead of sending "time": null (test 17).
  // If NTP never comes, the press stays in the queue until NET_TIMEOUT_MS and is sent on the next wake-up.
  if (p.epochMs == 0) return false;
  // RTC time exists (after deep sleep): wait briefly for NTP so the timestamp can be corrected.
  return millis() - wifiConnectedMs >= NTP_WAIT_MS;
}

bool sendPress(const Press& p) {
  int64_t ms = p.epochMs;
  bool corrected = p.ntpCorrected;
  if (p.session == bootCount && ntpSynced) {   // calculate back from NTP time
    ms = nowMs() - (int64_t)(millis() - p.millisAtPress);
    corrected = true;
  }

  char payload[256];
  if (ms > 0) {
    char ts[40];
    formatTime(ms, ts, sizeof ts);
    snprintf(payload, sizeof payload,
             "{\"device\":\"%s\",\"button\":\"%s\",\"value\":%d,\"time\":\"%s\",\"epoch_ms\":%lld,\"ntp_synced\":%s}",
             clientId, NAME[p.button], VALUE[p.button], ts, (long long)ms, corrected ? "true" : "false");
  } else {
    snprintf(payload, sizeof payload,
             "{\"device\":\"%s\",\"button\":\"%s\",\"value\":%d,\"time\":null,\"epoch_ms\":null,\"ntp_synced\":false}",
             clientId, NAME[p.button], VALUE[p.button]);
  }
  bool ok = mqtt.publish(topicPress, payload);
  if (ok) lastActivity = millis();             // stay awake IDLE_MS after sending, so a delayed packet gets through
  LOG("%s %s -> %s", ok ? "Sent" : "ERROR", topicPress, payload);
  return ok;
}

void flushQueue() {
  for (;;) {
    Press p;
    portENTER_CRITICAL(&queueMux);             // copy the oldest press - new ones may arrive meanwhile
    bool any = queueCount > 0;
    if (any) p = queue[0];
    portEXIT_CRITICAL(&queueMux);
    if (!any || !readyToSend(p)) return;
    if (!sendPress(p)) return;                 // try again later
    portENTER_CRITICAL(&queueMux);
    if (queueCount > 0 && memcmp(&queue[0], &p, sizeof p) == 0) {   // unless "queue full" dropped it
      memmove(&queue[0], &queue[1], sizeof(Press) * (queueCount - 1));
      queueCount--;
    }
    portEXIT_CRITICAL(&queueMux);
  }
}

void sendStatus() {
  if (!(ntpSynced || millis() - wifiConnectedMs >= NTP_WAIT_MS)) return;
  char ts[40] = "";
  if (timeValid()) formatTime(nowMs(), ts, sizeof ts);
  char payload[256];
  snprintf(payload, sizeof payload,
           "{\"device\":\"%s\",\"event\":\"%s\",\"time\":\"%s\",\"boot\":%lu,\"rssi\":%d,"
           "\"queue\":%d,\"prev_awake_ms\":%lu,\"ntp_synced\":%s,\"tls\":%s}",
           clientId, statusEvent, ts, (unsigned long)bootCount, WiFi.RSSI(),
           queueCount, (unsigned long)prevAwakeMs, ntpSynced ? "true" : "false",
           USE_TLS ? "true" : "false");
  if (mqtt.publish(topicStatus, payload)) {
    statusPending = false;
    LOG("Status sent -> %s", payload);
  }
}

void handleNetwork() {
  if (WiFi.status() != WL_CONNECTED) {
    wifiConnectedMs = 0;
    return;
  }
  if (wifiConnectedMs == 0) {
    wifiConnectedMs = millis();
    LOG("WiFi connected - IP %s, RSSI %d dBm", WiFi.localIP().toString().c_str(), WiFi.RSSI());
    if (!ntpStarted) {                        // SNTP re-syncs automatically every 3 h (CONFIG_LWIP_SNTP_UPDATE_DELAY)
      configTzTime(TIMEZONE, NTP_SERVER_1, NTP_SERVER_2);
      ntpStarted = true;
    }
  }

  if (!mqtt.connected()) {
    if (loginRejected) return;                // a wrong login does not get better by retrying
    if (lastMqttAttempt != 0 && millis() - lastMqttAttempt < MQTT_RETRY_MS) return;
    lastMqttAttempt = millis();
#if USE_TLS
    LOG("Connecting to MQTT %s:%u (TLS + login) as %s ...", MQTT_HOST, MQTT_PORT, clientId);
    uint32_t t0 = millis();
    // Open the TLS connection ourselves with a timeout (TCP + handshake, CA + host name are checked).
    // Blocks loop() while it runs - that is OK because buttons and lock run in esp_timer.
    // PubSubClient reuses it and only sends the MQTT login.
    if (!netClient.connect(MQTT_HOST, MQTT_PORT, (int32_t)TLS_TIMEOUT_MS)) {
      char err[128] = "";
      netClient.lastError(err, sizeof err);
      LOG("TLS connection failed after %lu ms: %s", (unsigned long)(millis() - t0), err);
      lastMqttAttempt = millis();              // retry gap counts from the END of a slow attempt
      return;
    }
    uint32_t t1 = millis();
    LOG("TLS up after %lu ms (CA verified)", (unsigned long)(t1 - t0));
    if (!mqtt.connect(clientId, MQTT_USER, MQTT_PASSWORD)) {
      mqttError();
      lastMqttAttempt = millis();
      return;
    }
    LOG("MQTT connected - login %lu ms, total %lu ms",
        (unsigned long)(millis() - t1), (unsigned long)(millis() - t0));
#else
    LOG("Connecting to MQTT %s:%u (UNENCRYPTED, no login) as %s ...", MQTT_HOST, MQTT_PORT, clientId);
    if (!mqtt.connect(clientId)) {
      mqttError();
      lastMqttAttempt = millis();
      return;
    }
    LOG("MQTT connected (UNENCRYPTED)");
#endif
  }

  mqtt.loop();
  flushQueue();
  if (statusPending) sendStatus();
}

// ---------------------------------------------------------------------------
// Deep sleep
// ---------------------------------------------------------------------------
uint64_t buttonMask() {
  uint64_t m = 0;
  for (int i = 0; i < 4; i++) m |= 1ULL << BUTTON_PIN[i];
  return m;
}

void goToSleep() {
  esp_timer_stop(buttonTimer);                 // no new presses from here
  if (locked) {                                // a press arrived just before the timer stopped
    esp_timer_start_periodic(buttonTimer, BUTTON_POLL_MS * 1000);
    return;
  }
  logDrift();                                  // NTP may have arrived in this same loop() pass
  prevAwakeMs = millis();
  // Correct timestamps of unsent presses from this wake-up before millis() is reset
  if (ntpSynced) {
    for (int i = 0; i < queueCount; i++) {
      if (queue[i].session == bootCount) {
        queue[i].epochMs = nowMs() - (int64_t)(millis() - queue[i].millisAtPress);
        queue[i].ntpCorrected = true;
      }
    }
  }
  if (mqtt.connected()) {
    mqtt.loop();
    mqtt.disconnect();
  }
  netClient.stop();
  delay(50);                                   // let TCP send the last data
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);

  esp_sleep_enable_ext1_wakeup(buttonMask(), ESP_EXT1_WAKEUP_ANY_HIGH);   // any button HIGH
  esp_sleep_enable_timer_wakeup(ROUTINE_SLEEP_S * 1000000ULL);
  sleepStartMs  = timeValid() ? nowMs() : 0;   // for drift measurement at the next NTP sync
  sleepStartNtp = ntpSynced;
  LOG("Deep sleep - awake %lu ms, %d presses in queue. Wakes on press or in %llu s",
      (unsigned long)prevAwakeMs, queueCount, (unsigned long long)ROUTINE_SLEEP_S);
  Serial.flush();
  esp_deep_sleep_start();
}

void checkSleep() {
  if (locked) return;
  for (int i = 0; i < 4; i++)
    if (raw[i] == HIGH || stable[i] == HIGH) return;   // a held button would wake it immediately

  uint32_t idle = millis() - lastActivity;
  if (idle < (routineWake ? 0 : IDLE_MS)) return;

  bool netDone = queueCount == 0 && !statusPending && (!routineWake || ntpSynced);
  if (!netDone) {
    if (idle < NET_TIMEOUT_MS && !loginRejected) return;
    LOG("Network not done after %lu ms - %d presses kept for next wake-up",
        (unsigned long)idle, queueCount);
  }
  goToSleep();
}

// ---------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  bootCount++;
  setenv("TZ", TIMEZONE, 1);                   // TZ is lost in deep sleep - set it again
  tzset();
  millisAtStart = millis();
  rtcAtStartMs  = timeValid() ? nowMs() : 0;   // RTC clock time, before NTP corrects it

  const esp_timer_create_args_t lockArgs = { .callback = lockExpired, .name = "lock" };
  esp_timer_create(&lockArgs, &lockTimer);     // must exist before the first registerPress()
  const esp_timer_create_args_t pollArgs = { .callback = pollButtons, .name = "buttons" };
  esp_timer_create(&pollArgs, &buttonTimer);

  for (int i = 0; i < 4; i++) {
    pinMode(BUTTON_PIN[i], INPUT);
    pinMode(LED_PIN[i], OUTPUT);
    raw[i] = digitalRead(BUTTON_PIN[i]);
    stable[i] = raw[i];
    lastChange[i] = millis();
  }
  lastActivity = millis();

  switch (esp_sleep_get_wakeup_cause()) {
    case ESP_SLEEP_WAKEUP_EXT1: {
      uint64_t m = esp_sleep_get_ext1_wakeup_status();
      LOG("Woken by button (boot %lu)", (unsigned long)bootCount);
      for (int i = 0; i < 4; i++) {
        if (m & (1ULL << BUTTON_PIN[i])) {
          raw[i] = HIGH;                       // avoid double registration
          stable[i] = HIGH;
          registerPress(i);
          break;
        }
      }
      break;
    }
    case ESP_SLEEP_WAKEUP_TIMER:
      LOG("Routine wake-up (boot %lu)", (unsigned long)bootCount);
      routineWake = true;
      statusPending = true;
      statusEvent = "routine";
      break;
    default:
      LOG("Startup (power-on / reset)");
      for (int i = 0; i < 4; i++) {            // LED test: green, blue, yellow, red
        digitalWrite(LED_PIN[i], HIGH);
        delay(400);
        digitalWrite(LED_PIN[i], LOW);
      }
      statusPending = true;
      statusEvent = "startup";
      lastActivity = millis();
      break;
  }

  snprintf(clientId, sizeof clientId, "%s-%06llx", MQTT_USER,
           (unsigned long long)(ESP.getEfuseMac() >> 24));
  // "elever/<user>/" is required by the broker's ACL - students may only publish there
  snprintf(topicPress,  sizeof topicPress,  "elever/%s/smiley/press",  MQTT_USER);
  snprintf(topicStatus, sizeof topicStatus, "elever/%s/smiley/status", MQTT_USER);

#if USE_TLS
  netClient.setCACert(CA_CERT);                // NEVER setInsecure()
#endif
  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setBufferSize(512);
  mqtt.setSocketTimeout(5);                    // max wait for a reply from the broker (default 15 s)
  sntp_set_time_sync_notification_cb(ntpCallback);

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);        // non-blocking
  esp_timer_start_periodic(buttonTimer, BUTTON_POLL_MS * 1000);   // buttons from now on
}

void loop() {                                  // buttons + lock run in esp_timer (see pollButtons)
  logDrift();
  handleNetwork();
  checkSleep();
  delay(5);
}
