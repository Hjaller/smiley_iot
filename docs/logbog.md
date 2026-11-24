# Logbog – Smiley feedback-panel

**Fag:** IoT II (FAG 17682)
**Gruppe:** Hjalte
**Repo:** https://github.com/Hjaller/smiley_iot

Hvert gruppemedlem skriver mindst én indførsel **hver dag** og committer den samme dag.
Nyeste dag øverst.

---

## Skabelon (kopiér til ny dag)

```markdown
## ÅÅÅÅ-MM-DD

### Navn
**Lavet i dag**
-

**Problemer / udfordringer**
-

**Beslutninger (og hvorfor)**
-

**Næste skridt**
-
```

---

## 2026-11-24

### Hjalte
**Lavet i dag**
- Læst projektopgaven og siden om den lokale MQTT-server (wilsons).
- Bygget selve IoT-enheden på breadboard med ESP32 DevKit V1:
  - 4 knapper (grøn, blå, gul, rød) på GPIO 32, 33, 25, 26 – koblet fra GPIO til 3V3 med pull-down-modstand til GND, så et tryk giver HIGH.
  - 4 LED'er på GPIO 23, 22, 21, 19 – hver med 220 Ω formodstand til GND.
  - Knapperne er valgt på RTC-GPIO'er, så de kan vække ESP32 fra deep sleep.
- Testet hardwaren med et simpelt testprogram: hver LED lyser, mens knappen ud for den holdes nede – alle 4 virker.
- Skrevet første udgave af firmwaren (med hjælp fra Claude Code / AI):
  - Debounce (50 ms) og 7 sekunders lås, hvor LED'en ud for knappen lyser.
  - WiFi, NTP-tid med dansk tidszone (inkl. sommertid).
  - MQTT med JSON-beskeder (knap, værdi, tidsstempel) til `elever/<bruger>/smiley/tryk` og status til `.../smiley/status`.
  - Deep sleep: vækkes af knaptryk (ext1) og af en timer for rutine-vågning.
  - Kø i RTC-hukommelsen, så tryk ikke går tabt, hvis netværket er nede.
- Sat TLS op: CA-certifikat (`ca.crt`) lagt ind i `include/ca_cert.h`, port 8883, brugernavn/kode i `include/secrets.h` (står i `.gitignore`).
- Projektet kompilerer både med og uden TLS.
- Uploadet og testet firmwaren på boardet (uden TLS, port 1883).

**Problemer / udfordringer**
- Vi har endnu ikke fået MQTT-brugernavn og -kode af læreren, så TLS-delen er ikke testet mod brokeren.

**Beslutninger (og hvorfor)**
- TLS er **midlertidigt slået fra** (`#define BRUG_TLS 0`, port 1883, uden login), så vi kan teste resten, mens vi venter på login. Skal sættes tilbage til `1` – opgaven kræver TLS.
- Grøn = 4 (mest tilfreds) … rød = 1, så højere tal betyder større tilfredshed.
- Tidsstemplet er tidspunktet for **trykket**, ikke for afsendelsen, og rettes efter NTP, når nettet er oppe.
- Startværdier: 30 s inaktiv før deep sleep, rutine-vågning hver 6. time. Det er gæt, som skal bestemmes ved forsøg.

**Næste skridt**
- Få login af læreren, slå TLS til (`BRUG_TLS 1`) og teste igen på boardet.
- Planlægge forsøg med deep sleep (tid fra opvågning til afsendelse, strømforbrug).
- Give Søren adgang til git repo / gøre git repo public

