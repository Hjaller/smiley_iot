#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <esp_sleep.h>
#include <esp_sntp.h>
#include <sys/time.h>
#include <time.h>

#include "secrets.h"   // WIFI_SSID, WIFI_PASSWORD, MQTT_USER, MQTT_PASSWORD (ikke i git)
#include "ca_cert.h"   // CA_CERT - brokerens CA-certifikat

// Smiley feedback-panel
//
// Forløb:
//   1. Knaptryk vækker ESP32 fra deep sleep (ext1). LED'en lyser straks,
//      og trykket gemmes med tidsstempel i RTC-hukommelsen.
//   2. WiFi -> NTP -> MQTT over TLS (port 8883, CA-tjek, brugernavn/kode).
//   3. Trykket sendes som JSON. Panelet er låst i 7 s, mens LED'en lyser.
//   4. Efter INAKTIV_MS uden tryk går den i deep sleep igen.
//   5. Rutine-vågning hver RUTINE_SOVETID_S: NTP-synk + statusbesked.
//
// Tryk, der ikke kan sendes (fx intet net), bliver liggende i RTC-køen
// og sendes ved næste vågning.

// ---------------------------------------------------------------------------
// Indstillinger
// ---------------------------------------------------------------------------
// *** SIKKERHED ***
//   1 = TLS-kryptering + brugernavn/kode, port 8883  (KRAV i opgaven)
//   0 = MIDLERTIDIGT: ukrypteret, uden login, port 1883
#ifndef BRUG_TLS
#define BRUG_TLS 0
#endif

#if BRUG_TLS
const uint16_t MQTT_PORT = 8883;
#else
#warning "BRUG_TLS=0: MQTT er UKRYPTERET og uden login - husk at skifte tilbage til 1!"
const uint16_t MQTT_PORT = 1883;
#endif

const char* MQTT_VAERT = "wilsons.local";   // værtsnavn (ikke IP) - bruges til certifikat-tjek

const char* TIDSZONE     = "CET-1CEST,M3.5.0,M10.5.0/3";   // Danmark inkl. sommertid
const char* NTP_SERVER_1 = "pool.ntp.org";
const char* NTP_SERVER_2 = "time.google.com";

const uint32_t DEBOUNCE_MS        = 50;
const uint32_t LAAS_MS            = 7000;     // spærretid efter tryk (krav: 7 s)
const uint32_t INAKTIV_MS         = 30000;    // vågen-tid uden aktivitet før deep sleep  -> bestem ved forsøg
const uint64_t RUTINE_SOVETID_S   = 6ULL * 3600;  // rutine-vågning (NTP + status)       -> bestem ved forsøg
const uint32_t NET_TIMEOUT_MS     = 20000;    // opgiv net og sov (tryk bliver i køen)
const uint32_t NTP_VENT_MS        = 4000;     // max ventetid på NTP før tryk sendes med RTC-tid
const uint32_t MQTT_GENFORSOEG_MS = 3000;

//                         grøn  blå  gul  rød
const int   KNAP_PIN[4] = {  32,  33,  25,  26 };   // alle er RTC-GPIO -> kan vække fra deep sleep
const int   LED_PIN[4]  = {  23,  22,  21,  19 };
const char* NAVN[4]     = { "groen", "blaa", "gul", "roed" };
const int   VAERDI[4]   = {   4,     3,     2,     1   };  // 4 = mest tilfreds

#define LOG(fmt, ...) Serial.printf("[%7lu ms] " fmt "\n", (unsigned long)millis(), ##__VA_ARGS__)

// ---------------------------------------------------------------------------
// RTC-hukommelse - overlever deep sleep (nulstilles ved strøm-til/reset)
// ---------------------------------------------------------------------------
struct Tryk {
  uint8_t  knap;
  uint32_t session;        // bootNr da trykket skete
  uint32_t millisVedTryk;  // til NTP-korrektion af tidsstemplet i samme session
  int64_t  epochMs;        // tid ved tryk (0 = ukendt)
};

const int KOE_MAX = 32;
RTC_DATA_ATTR Tryk     koe[KOE_MAX];
RTC_DATA_ATTR int      koeAntal        = 0;
RTC_DATA_ATTR uint32_t bootNr          = 0;
RTC_DATA_ATTR uint32_t forrigeVaagenMs = 0;   // til strømforbrugs-analysen

// ---------------------------------------------------------------------------
// Tilstand
// ---------------------------------------------------------------------------
#if BRUG_TLS
WiFiClientSecure netKlient;   // TLS
#else
WiFiClient       netKlient;   // ukrypteret (midlertidigt)
#endif
PubSubClient     mqtt(netKlient);

char klientId[48];
char topicTryk[96];
char topicStatus[96];

volatile bool ntpSynk = false;
bool ntpStartet       = false;
uint32_t wifiForbundetMs  = 0;
uint32_t sidsteMqttForsoeg = 0;

bool rutineVaagning = false;
bool statusAfventer = false;
const char* statusHaendelse = "";

bool     laast = false;
int      aktivLed = -1;
uint32_t laasStart = 0;
uint32_t sidsteAktivitet = 0;

int      raa[4], stabil[4];
uint32_t sidsteSkift[4];

// ---------------------------------------------------------------------------
// Tid
// ---------------------------------------------------------------------------
int64_t nuMs() {
  struct timeval tv;
  gettimeofday(&tv, nullptr);
  return (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

bool tidGyldig() { return time(nullptr) > 1700000000; }   // efter nov. 2023 = sat

// ISO 8601 med millisekunder og tidszone, fx 2026-11-24T10:15:03.123+01:00
void formaterTid(int64_t ms, char* buf, size_t len) {
  time_t s = ms / 1000;
  struct tm t;
  localtime_r(&s, &t);
  char dato[24], zone[8];
  strftime(dato, sizeof dato, "%Y-%m-%dT%H:%M:%S", &t);
  strftime(zone, sizeof zone, "%z", &t);                 // +0100
  snprintf(buf, len, "%s.%03d%.3s:%s", dato, (int)(ms % 1000), zone, zone + 3);
}

void ntpCallback(struct timeval*) { ntpSynk = true; }

// ---------------------------------------------------------------------------
// Knapper, debounce og lås
// ---------------------------------------------------------------------------
void registrerTryk(int i) {
  laast = true;
  laasStart = millis();
  aktivLed = i;
  sidsteAktivitet = millis();
  rutineVaagning = false;   // nu er det en "rigtig" vågning
  for (int j = 0; j < 4; j++) digitalWrite(LED_PIN[j], j == i ? HIGH : LOW);

  if (koeAntal == KOE_MAX) {          // fuld kø: kassér ældste
    memmove(&koe[0], &koe[1], sizeof(Tryk) * (KOE_MAX - 1));
    koeAntal--;
    LOG("Koe fuld - aeldste tryk kasseret");
  }
  koe[koeAntal++] = { (uint8_t)i, bootNr, millis(), tidGyldig() ? nuMs() : 0 };
  LOG("Tryk: %s (vaerdi %d) - laast i %lu s", NAVN[i], VAERDI[i], LAAS_MS / 1000);
}

void laesKnapper() {
  uint32_t nu = millis();
  for (int i = 0; i < 4; i++) {
    int r = digitalRead(KNAP_PIN[i]);        // HIGH = trykket (pull-down)
    if (r != raa[i]) {                       // signalet ændrer sig -> start forfra
      raa[i] = r;
      sidsteSkift[i] = nu;
    } else if (r != stabil[i] && nu - sidsteSkift[i] >= DEBOUNCE_MS) {
      stabil[i] = r;                         // stabilt i DEBOUNCE_MS -> accepter
      if (r == HIGH) {
        if (!laast) registrerTryk(i);
        else LOG("%s ignoreret (laast)", NAVN[i]);
      }
    }
  }
}

void opdaterLaas() {
  if (laast && millis() - laasStart >= LAAS_MS) {
    laast = false;
    digitalWrite(LED_PIN[aktivLed], LOW);
    aktivLed = -1;
    sidsteAktivitet = millis();
    LOG("Laas ophaevet - klar til nyt tryk");
  }
}

// ---------------------------------------------------------------------------
// MQTT
// ---------------------------------------------------------------------------
bool klarTilAtSende(uint32_t session) {
  // Tryk fra tidligere vågninger har allerede deres bedste tidsstempel.
  // Tryk fra denne vågning venter kort på NTP, så tidsstemplet kan korrigeres.
  return session != bootNr || ntpSynk || millis() - wifiForbundetMs >= NTP_VENT_MS;
}

bool sendTryk(const Tryk& t) {
  int64_t ms = t.epochMs;
  bool korrigeret = false;
  if (t.session == bootNr && ntpSynk) {        // regn tilbage fra NTP-tid
    ms = nuMs() - (int64_t)(millis() - t.millisVedTryk);
    korrigeret = true;
  }

  char payload[256];
  if (ms > 0) {
    char tid[40];
    formaterTid(ms, tid, sizeof tid);
    snprintf(payload, sizeof payload,
             "{\"enhed\":\"%s\",\"knap\":\"%s\",\"vaerdi\":%d,\"tid\":\"%s\",\"epoch_ms\":%lld,\"ntp_synk\":%s}",
             klientId, NAVN[t.knap], VAERDI[t.knap], tid, (long long)ms, korrigeret ? "true" : "false");
  } else {
    snprintf(payload, sizeof payload,
             "{\"enhed\":\"%s\",\"knap\":\"%s\",\"vaerdi\":%d,\"tid\":null,\"epoch_ms\":null,\"ntp_synk\":false}",
             klientId, NAVN[t.knap], VAERDI[t.knap]);
  }
  bool ok = mqtt.publish(topicTryk, payload);
  LOG("%s %s -> %s", ok ? "Sendt" : "FEJL", topicTryk, payload);
  return ok;
}

void toemKoe() {
  while (koeAntal > 0 && klarTilAtSende(koe[0].session)) {
    if (!sendTryk(koe[0])) return;             // prøv igen senere
    memmove(&koe[0], &koe[1], sizeof(Tryk) * (koeAntal - 1));
    koeAntal--;
  }
}

void sendStatus() {
  if (!(ntpSynk || millis() - wifiForbundetMs >= NTP_VENT_MS)) return;
  char tid[40] = "";
  if (tidGyldig()) formaterTid(nuMs(), tid, sizeof tid);
  char payload[256];
  snprintf(payload, sizeof payload,
           "{\"enhed\":\"%s\",\"haendelse\":\"%s\",\"tid\":\"%s\",\"boot\":%lu,\"rssi\":%d,"
           "\"koe\":%d,\"forrige_vaagen_ms\":%lu,\"ntp_synk\":%s,\"tls\":%s}",
           klientId, statusHaendelse, tid, (unsigned long)bootNr, WiFi.RSSI(),
           koeAntal, (unsigned long)forrigeVaagenMs, ntpSynk ? "true" : "false",
           BRUG_TLS ? "true" : "false");
  if (mqtt.publish(topicStatus, payload)) {
    statusAfventer = false;
    LOG("Status sendt -> %s", payload);
  }
}

void haandterNetvaerk() {
  if (WiFi.status() != WL_CONNECTED) {
    wifiForbundetMs = 0;
    return;
  }
  if (wifiForbundetMs == 0) {
    wifiForbundetMs = millis();
    LOG("WiFi forbundet - IP %s, RSSI %d dBm", WiFi.localIP().toString().c_str(), WiFi.RSSI());
    if (!ntpStartet) {                        // SNTP synker igen automatisk hver time
      configTzTime(TIDSZONE, NTP_SERVER_1, NTP_SERVER_2);
      ntpStartet = true;
    }
  }

  if (!mqtt.connected()) {
    if (sidsteMqttForsoeg != 0 && millis() - sidsteMqttForsoeg < MQTT_GENFORSOEG_MS) return;
    sidsteMqttForsoeg = millis();
#if BRUG_TLS
    LOG("Forbinder til MQTT %s:%u (TLS + login) som %s ...", MQTT_VAERT, MQTT_PORT, klientId);
    if (!mqtt.connect(klientId, MQTT_USER, MQTT_PASSWORD)) {
      char fejl[128] = "";
      netKlient.lastError(fejl, sizeof fejl);
      LOG("MQTT fejl rc=%d, TLS: %s", mqtt.state(), fejl);
      return;
    }
    LOG("MQTT forbundet (TLS verificeret mod CA)");
#else
    LOG("Forbinder til MQTT %s:%u (UKRYPTERET, uden login) som %s ...", MQTT_VAERT, MQTT_PORT, klientId);
    if (!mqtt.connect(klientId)) {
      LOG("MQTT fejl rc=%d", mqtt.state());
      return;
    }
    LOG("MQTT forbundet (UKRYPTERET)");
#endif
  }

  mqtt.loop();
  toemKoe();
  if (statusAfventer) sendStatus();
}

// ---------------------------------------------------------------------------
// Deep sleep
// ---------------------------------------------------------------------------
uint64_t knapMaske() {
  uint64_t m = 0;
  for (int i = 0; i < 4; i++) m |= 1ULL << KNAP_PIN[i];
  return m;
}

void gaaISoevn() {
  forrigeVaagenMs = millis();
  if (mqtt.connected()) {
    mqtt.loop();
    mqtt.disconnect();
  }
  netKlient.stop();
  delay(50);                                   // lad TCP nå at sende det sidste
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);

  esp_sleep_enable_ext1_wakeup(knapMaske(), ESP_EXT1_WAKEUP_ANY_HIGH);   // vilkårlig knap HIGH
  esp_sleep_enable_timer_wakeup(RUTINE_SOVETID_S * 1000000ULL);
  LOG("Deep sleep - vaagen %lu ms, %d tryk i koe. Vaagner ved tryk eller om %llu s",
      (unsigned long)forrigeVaagenMs, koeAntal, (unsigned long long)RUTINE_SOVETID_S);
  Serial.flush();
  esp_deep_sleep_start();
}

void tjekSoevn() {
  if (laast) return;
  for (int i = 0; i < 4; i++)
    if (raa[i] == HIGH || stabil[i] == HIGH) return;   // holdt knap ville vække straks

  uint32_t inaktiv = millis() - sidsteAktivitet;
  if (inaktiv < (rutineVaagning ? 0 : INAKTIV_MS)) return;

  bool netFaerdig = koeAntal == 0 && !statusAfventer && (!rutineVaagning || ntpSynk);
  if (!netFaerdig) {
    if (inaktiv < NET_TIMEOUT_MS) return;
    LOG("Net ikke faerdigt efter %lu ms - %d tryk gemmes til naeste vaagning",
        (unsigned long)inaktiv, koeAntal);
  }
  gaaISoevn();
}

// ---------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  bootNr++;
  setenv("TZ", TIDSZONE, 1);                   // TZ går tabt i deep sleep - sæt igen
  tzset();

  for (int i = 0; i < 4; i++) {
    pinMode(KNAP_PIN[i], INPUT);
    pinMode(LED_PIN[i], OUTPUT);
    raa[i] = stabil[i] = digitalRead(KNAP_PIN[i]);
    sidsteSkift[i] = millis();
  }
  sidsteAktivitet = millis();

  switch (esp_sleep_get_wakeup_cause()) {
    case ESP_SLEEP_WAKEUP_EXT1: {
      uint64_t m = esp_sleep_get_ext1_wakeup_status();
      LOG("Vaagnet af knap (boot %lu)", (unsigned long)bootNr);
      for (int i = 0; i < 4; i++) {
        if (m & (1ULL << KNAP_PIN[i])) {
          raa[i] = stabil[i] = HIGH;           // undgå dobbelt-registrering
          registrerTryk(i);
          break;
        }
      }
      break;
    }
    case ESP_SLEEP_WAKEUP_TIMER:
      LOG("Rutine-vaagning (boot %lu)", (unsigned long)bootNr);
      rutineVaagning = true;
      statusAfventer = true;
      statusHaendelse = "rutine";
      break;
    default:
      LOG("Opstart (stroem til / reset)");
      for (int i = 0; i < 4; i++) {            // LED-test: grøn, blå, gul, rød
        digitalWrite(LED_PIN[i], HIGH);
        delay(400);
        digitalWrite(LED_PIN[i], LOW);
      }
      statusAfventer = true;
      statusHaendelse = "opstart";
      sidsteAktivitet = millis();
      break;
  }

  snprintf(klientId, sizeof klientId, "%s-%06llx", MQTT_USER,
           (unsigned long long)(ESP.getEfuseMac() >> 24));
  snprintf(topicTryk,   sizeof topicTryk,   "elever/%s/smiley/tryk",   MQTT_USER);
  snprintf(topicStatus, sizeof topicStatus, "elever/%s/smiley/status", MQTT_USER);

#if BRUG_TLS
  netKlient.setCACert(CA_CERT);                // ALDRIG setInsecure()
#endif
  mqtt.setServer(MQTT_VAERT, MQTT_PORT);
  mqtt.setBufferSize(512);
  sntp_set_time_sync_notification_cb(ntpCallback);

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);        // ikke-blokerende - knapper virker imens
}

void loop() {
  laesKnapper();
  opdaterLaas();
  haandterNetvaerk();
  tjekSoevn();
  delay(5);
}
