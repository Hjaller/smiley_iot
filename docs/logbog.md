# Logbog – Smiley feedback-panel

**Fag:** IoT II (FAG 17682) · **Gruppe:** Hjalte · **Repo:** https://github.com/Hjaller/smiley_iot

Kort overblik over hver dag. Detaljer, målinger og tests står i [portfolio.md](portfolio.md) (kort udgave: [portfolio_kort.md](portfolio_kort.md)).
Hvert gruppemedlem skriver mindst én indførsel hver dag og committer den samme dag. Nyeste dag øverst.

---

## 2026-11-25 – Hjalte

**Lavet**
- Fået MQTT-login af læreren og slået TLS til. Det virker mod brokeren.
- Målt tider med TLS: handshaken tager ca. 0,7 s, men tiden fra tryk til afsendelse er uændret (ca. 4 s).
- Målt, at ESP32's ur går ca. 1,3 % for hurtigt under deep sleep.
- Beregnet strømforbrug ud fra databladet og valgt inaktiv-tid (10 s) og rutine-vågning (hver 6. time).
- Kørt tests af rutine-vågning, TLS, forkert kode, langsomt net, holdt knap og den nye inaktiv-tid. Alle bestået efter rettelser.
- Oversat koden og JSON-felterne til engelsk.
- Taget foto af opstillingen og screenshot af weblog'en til portfolioen.

**Ændret**
- Knapper og lås kører nu uafhængigt af netværket, så tryk ikke går tabt, når forbindelsen hænger.
- Forkert login prøves kun én gang pr. vågning.
- Ventetid på NTP hævet fra 4 til 17 s, og tryk sendes ikke længere uden tidsstempel.
- Inaktiv-tid før deep sleep sat ned fra 30 til 10 s.

**Problemer**
- NTP er ofte langsom (op til ca. 17 s).
- Test 10 (intet WiFi) kunne ikke laves, da vi ikke kan komme uden for WiFi-rækkevidde.

**Mangler**
- Commit og give læreren adgang til repoet.

---

## 2026-11-24 – Hjalte

**Lavet**
- Læst opgaven og siden om den lokale MQTT-server.
- Bygget enheden på breadboard: ESP32, 4 knapper og 4 LED'er. Testet, at alt virker.
- Skrevet firmwaren (med hjælp fra AI): debounce, 7 s lås, WiFi, NTP, MQTT med JSON og deep sleep.
- Testet på boardet uden TLS: tryk når frem til brokeren, og deep sleep virker.
- Oprettet logbog og portfolio.

**Ændret**
- TLS midlertidigt slået fra, fordi vi endnu ikke havde login.

**Mangler**
- Login fra læreren, så TLS kan testes.

---

<details>
<summary>Skabelon til ny dag</summary>

```markdown
## ÅÅÅÅ-MM-DD – Navn

**Lavet**
-

**Ændret**
-

**Problemer**
-

**Mangler**
-
```
</details>
