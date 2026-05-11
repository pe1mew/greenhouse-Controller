# Handleiding Kascontroller — voor de beheerder

**Versie:** 1.5 — concept
**Datum:** 2026-05-11
**Firmware:** 1.17.20

---

> **Voor wie is deze handleiding?**
> Dit document is geschreven voor de **technisch beheerder / installateur** die verantwoordelijk is voor de installatie, configuratie, onderhoud en het oplossen van storingen aan de kascontroller. De boer/kasgebruiker bedient het systeem dagelijks; daarvoor is een aparte boer-handleiding (`manual/handleiding.md`).

> **Veiligheid**
> Binnen in de kast van de kascontroller bevinden zich onderdelen onder **netspanning (230 V)**. Werk uitsluitend aan de bedrading of aan motoren met de **voeding afgekoppeld**. Een aansluiting op het lichtnet vereist kennis van elektrische installaties. Let bovendien op draaiende motoren: handmatige werkzaamheden aan ramen mogen alleen wanneer de motoren stilstaan en bij voorkeur met de motorbox-schakelaars in handbediening (zie [§15](#15-handmatige-overname-via-de-motorbox)).

> **Verwijzing naar boer-handleiding**
> Voor algemene uitleg over dagelijks gebruik, schermen op de LCD en alarm-meldingen verwijst dit document regelmatig naar de [boer-handleiding](handleiding.md). Onderwerpen die specifiek voor de beheerder zijn worden hier volledig behandeld.

---

## Inhoudsopgave

1. [Over deze handleiding](#1-over-deze-handleiding)
2. [Wat doet de kascontroller?](#2-wat-doet-de-kascontroller)
3. [De kas en het systeem](#3-de-kas-en-het-systeem)
4. [Hoe regelt de controller het klimaat?](#4-hoe-regelt-de-controller-het-klimaat)
5. [De controller (fysiek)](#5-de-controller-fysiek)
6. [De webinterface (via WiFi)](#6-de-webinterface-via-WiFi)
7. [De twee gebruikersrollen](#7-de-twee-gebruikersrollen)
8. [Gebruik zonder inloggen — informatiemenu](#8-gebruik-zonder-inloggen--informatiemenu)
9. [Inloggen als beheerder](#9-inloggen-als-beheerder)
10. [Klimaat instellen](#10-klimaat-instellen)
11. [WiFi en webinterface — installatie en beheer](#11-WiFi-en-webinterface--installatie-en-beheer)
12. [Alarmen en bedrijfsmodi — diagnose en herstel](#12-alarmen-en-bedrijfsmodi--diagnose-en-herstel)
13. [Inschakelen na stroomuitval](#13-inschakelen-na-stroomuitval)
14. [Onderhoud — wat de beheerder doet](#14-onderhoud--wat-de-beheerder-doet)
15. [Handmatige overname via de motorbox](#15-handmatige-overname-via-de-motorbox)
16. [Probleemoplossing — Beheerder-niveau](#16-probleemoplossing--Beheerder-niveau)
17. [Verklarende woordenlijst](#17-verklarende-woordenlijst)
18. [Reset-procedure (BOOT-knop)](#18-reset-procedure-boot-knop-op-microprocessorboard)
19. [Bijlagen](#19-bijlagen)
20. [Versie en wijzigingshistorie](#20-versie-en-wijzigingshistorie)

---

## 1. Over deze handleiding

### Doelpubliek
Technisch beheerder / installateur van de kascontroller. Een persoon met:
- Kennis van **elektrische installaties** (230 V, aarding, zekeringen)
- Basis **netwerkkennis** (WiFi, IP-adressen, DHCP)
- Begrip van **Modbus RTU / RS485** op hoofdlijnen (bus-topologie, afsluitweerstanden) — zie [Bijlage E](#bijlage-e--modbus-rtu-een-digitale-sensor-interface) voor een korte uitleg
- Bereidheid om incidenteel een microprocessor-board fysiek te bedienen (jumpers, BOOT-knop, USB-flash)

### Wat staat er in deze handleiding?
- Eerste-installatie en commissioning
- Alle beheersfuncties op het LCD/toetsenbord én in de webinterface
- Configuratie van WiFi, locatie, NTP, motor-tijden, sensor-instellingen, hysteresis, glijdend gemiddelde
- PIN-management voor zowel Farmer als Beheerder
- Diagnose en oplossing van alarmen, sensor-fouten en motor-storingen
- Firmware-updates (OTA), SD-kaart beheer, downloaden van logs
- Reset-procedures en herstel naar fabrieksinstellingen

### Wat staat er niet in?
- **Firmware-broncode-niveau** detail → zie `design/technicalSoftwareDesignSpecification.md`
- **Hardware-ontwerp / schema's / printontwerp** → zie `hardware/` map
- **Teelt-advies** (klimaatzones per gewas) → vraag de teler / leverancier van de planten

### Bij escalatie
- Voor hardware-storing: leverancier kascontroller, zie [§19 Bijlage A](#bijlage-a--contactgegevens-leverancier-en-escalatie)
- Voor firmware-bug: GitHub-issue / leverancier
- Voor planten / teelt-vragen: niet door beheerder of leverancier

---

## 2. Wat doet de kascontroller?

De kascontroller automatiseert het **klimaat in één kas** door drie motorgestuurde ramen op de juiste momenten te openen en te sluiten. Hij meet:
- Temperatuur (T) en relatieve luchtvochtigheid (RH) binnen
- Windsnelheid en -richting buiten

en stuurt op basis daarvan de Hotraco RRK-3 motorbox aan om de drie ramen te bedienen.

### Werkingsprincipe in één alinea

De controller leest **elke poll-cyclus** (default 30 sec.) de sensoren uit via Modbus RTU. De ruwe metingen worden door een **glijdend gemiddelde** gehaald om piekmetingen te dempen. Het gemiddelde wordt vergeleken met de actieve **dag- of nacht-setpoints** voor Temperatuur en Relatieve Luchtvochtigheid (dag/nacht omgeschakeld op basis van zonsopkomst en zonsondergang, berekend uit geografische locatie en datum). Bij overschrijding van een setpoint inclusief **hysteresis** wordt een ventilatie-stap omhoog (raam open) of omlaag (raam dicht) genomen. De drie ramen worden in drie stappen gebruikt: eerst M1, dan M1+M2, dan alle drie.

### Wat doet de controller wél
- Klimaatregeling door **ventilatie** op basis van Temperatuur en Relatieve luchtvoctiheid binnen ingestelde grenzen
- Automatische dag/nacht-omschakeling
- hysteresis en glijdend gemiddelde tegen oscillaties
- Drie-traps ventilatie-strategie (M1 → M1+M2 → M1+M2+M3)
- Veiligheidsmechanismen: wind-override (sterke wind), motor-alarm, sensor-fault detectie (problemen met het uitlezen van de sensors)
- Automatische CLOSE_ALL kalibratie bij opstart
- Permanente opslag in het geheugen (NVS) van alle setpoints en configuratie instellingen
- Logging van de activiteiten op de kascontroller in het geheugen en op SD-kaart

### Wat doet de controller niet
- Geen verwarming of koeling aansturen
- Geen klimaatschermen aansturen
- Geen besproeiing of CO₂-dosering aansturen
- Ramen die gedeeltelijk penof dicht gestuurd worden. Er is een positie-feedback van motoren. De controller werkt op tijd-gestuurde commando's via de RRK-3

---

## 3. De kas en het systeem

`[FOTO: bovenaanzicht / plattegrond van de kas met M1, M2 en M3 aangegeven]`

### Kas-afmetingen
- Lengte (oost-west): ongeveer 40 m
- Breedte (noord-zuid): ongeveer 16 m
- Dak: gevelvormig (puntdak), nok in oost-west richting

### De drie motorgestuurde ramen

| ID | Naam | Locatie | Oppervlak | Travel-time default |
|---|---|---|---|---|
| **M1** | Dakbeluchting Zuid | Zuidelijke dakhelft | ca. 8 m² | 21 sec. |
| **M2** | Dakbeluchting Noord | Noordelijke dakhelft | ca. 8 m² | 21 sec. |
| **M3** | Zijwandbeluchting Noord | Noordelijke zijwand | ca. 80 m² | 171 sec. |

> **Travel-time** is de tijd die de motor nodig heeft om volledig te openen of te sluiten. Deze wordt door de controller gebruikt als time-out voor de open/sluit-pulsen. 

### Sensoren

| Type | Model | Lokatie |
|---|---|---|---|---|
| Temperatuur + Relatieve luchtvochtigheid (RH) | **FG6485A** | Binnen, een op representatieve plek | 
| Wind snelheid + richting | **SenseCAP S200** | Buiten, vrij van afscherming  door grote objecten in de omgeving | 

De sensoren zijn **digitaal** en communiceren met de kascontroller via het **Modbus RTU** protocol over een gedeelde **RS485-bus**. Elke sensor heeft een eigen bus-adres en levert meetwaarden direct als getal — geen analoge spanning of stroom. Voor een volledige uitleg van wat Modbus RTU is, hoe het werkt en waarom het wordt gebruikt in plaats van analoge sensoren, zie [Bijlage E — Modbus RTU](#bijlage-e--modbus-rtu-een-digitale-sensor-interface).

> **Modbus-bus in deze installatie**: Alle sensoren delen één RS485-bus naar de kascontroller (UART1, MAX485 transceiver, DE/RE direction control). Afsluitweerstand 120 Ω op het verste eind van de bus. Bekabeling: twisted-pair voor A/B + aparte aarding/afscherming en 24 V voor de sensorvoeding.

### Hotraco RRK-3 motor-relaisbox

- **Locatie**: in de kas gemonteerd, rechts bij binnenkomst, op dezelfde plek als de kascontroller
- **Functie**: drie kanalen (één per raam), elk met OPEN- en CLOSE-relais (potentiaalvrije contacten 24 V)
- **Eindschakelaars**: per kanaal aanwezig — stoppen de motor automatisch wanneer een raam volledig open of dicht is
- **Alarm-uitgang**: potentiaal vrij contact. Active-low: gesloten contact = alarm
- **Handbediening**: drie schakelaars per kanaal (Auto / Hand / Uit) — zie [§15](#15-handmatige-overname-via-de-motorbox)
- **Documentatie**: leverancier-handleiding van Hotraco RRK-3 — naast deze handleiding bewaren

### Kascontroller hardware

- **Microprocessor**: WEMOS LOLIN S3 (ESP32-S3, 8 MB flash, 512 KB SRAM)
- **LCD**: 16×2 tekens LCD met RGB of witte verlichting
- **Toetsenbord**: 4×4 matrix (membraan)
- **RGB-LED**: WS2812B op GPIO38 (zichtbaar door doorzichtige kap)
- **Heartbeat-LED**: groene LED (1 Hz)
- **Modbus-interface**: MAX485 transceiver
- **Motor-relais sturing**: 6 relais contacten naar RRK-3 (3× OPEN + 3× CLOSE)
- **Motor-alarm-input**: input, gesloten contact is alarm. (intern pull-up, active-low)
- **RTC**: Real Time Clock met CR2032 backup-batterij
- **SD-kaart**: Grote SD kaartlezer
- **Voeding**: Meanwell 24 V gelijkspanning
- **IP-rating kast**: IP67

`[FOTO: open kast met bedrading en bordjes met etiketten, GPIO-pinout zichtbaar]`

### Schematisch overzicht

```
                           Load
   +---------------+        |      +-----------------+        +-----------+
   | Temperatuur/Luchtvochtigheid-sensor   |--------|      | KASCONTROLLER   |--------| RRK-3     |--motor M1
   | FG6485A       |   RS485|      | (ESP32-S3       |        | (relais-  |--motor M2
   | (Modbus)      |        |      |  + LCD + WiFi)  |        |  box)     |--motor M3
   +---------------+        |------|                 |        +-----------+
                            |      |                 |              | alarm-uitgang
   +---------------+        |      |                 |<-------------+
   | Wind-sensor   |--------|      |                 |        
   | SenseCAP S200 |   RS485|      |                 |         
   | (Modbus)      |        |      +-----------------+
   +---------------+      Load
```

---

## 4. Hoe regelt de controller het klimaat?

### Setpoints (boer-bewerkbaar via Climate-tab of LCD-menu)

| Beschrijving | Default | Bereik | Eenheid |
|---|---|---|---|
| Maximum dagtemperatuur | 28 | 15–45 | °C |
| Maximum nachttemperatuur | 20 | 10–35 | °C |
| Maximum dagvochtigheid | 75 | 40–98 | % |
| Maximum nachtvochtigheid | — | 40–98 | % |
| Minimum dagvochtigheid | 50 | 20–90 | % |
| Minimum nachtvochtigheid | — | 20–90 | % |

### Beheerder-only parameters

| Beschrijving | Default | Bereik | Eenheid |
|---|---|---|---|
| hysteresis temperatuur | 5 | 2–15 | °C |
| hysteresis vochtigheid | 12 | 2–20 | % |
| Gemiddelde-venster T | 6 | 1–30 | min. |
| Gemiddelde-venster RH | 10 | 1–30 | min. |
| Vochtregeling aan/uit | 1 | 0/1 | — |
| Conflict-prioriteit | 0 | 0–2 | — |

**Effect van elke parameter**:

- **Hysteresis**: Dit is de bandbreedte rondom een setpoint waarbinnen niet wordt geschakeld. Voorbeeld bij `hyst_t = 5`: bij `t_max_dag = 28 °C` opent een raam wanneer T ≥ 28 °C en sluit het pas weer wanneer T ≤ 23 °C. Voorkomt continu in/uit-schakelen rond een setpoint.
- **Glijdend gemiddelde**: meetwaarden worden over de venstertijd in minutengemiddeld voordat ze met een setpoint worden vergeleken. Een groter venster maakt de regeling rustiger en minder gevoelig voor pieken (een korte zonnestraal op de sensor); een kleiner venster reageert sneller. De live waarden op het LCD display toont de **ruwe** (laatste) meetwaarde zodat de gebruiker altijd het actuele resultaat ziet.
- **Vochtregeling aan/uit**: vochtregeling uit; alleen temperatuur wordt geregeld. Zinvol bij teelten waarbij luchtvochtigheid niet relevant is of wanneer de luchtvochtiheid sensor defect is.
- **Conflict-prioriteit**:
  - `0` — Temperature first (Temperatuur regeling krijgt voorrang)
  - `1` — Humidity first (luchtvochtigheid krijgt voorrang)
  - `2` — Auto (de regeling kijkt naar relatieve afwijking, de regeling eldt voor de meetwaarde met de grootset afwijking)

### Dwelltime (wachttijd) per motor (Beheerder-only, Motors-tab)

Minimum tijd dat een raam in een stand moet blijven voordat de controller hem opnieuw mag schakelen. Deze instelling dempt langzame oscillatie, met name bij het raam in de zijwand (M3) met een groot oppervlak en lang klimaat-respons.

| Motor | Dwell-open default | Dwell-close default | Bereik |
|---|---|---|---|
| M1 (dak zuid) | 300 s (5 min) | 300 s (5 min) | 0–1500 s |
| M2 (dak noord) | 300 s (5 min) | 300 s (5 min) | 0–1500 s |
| M3 (zijwand noord) | **1500 s (25 min)** | **600 s (10 min)** | 0–1500 s |

> De M3 default-waarden zijn op kas gekalibreerd voor eem lange responstijd. Andere kassen kunnen andere waarden vergen.

### Stapsgewijs ventileren

De controller telt een interne ventilatie-stap-teller (0–3) per regelas (Temperatuur en Luchtvochtigheid):

| Stap | Open ramen |
|---|---|
| 0 | Geen — alle ramen dicht |
| 1 | M1 |
| 2 | M1 + M2 |
| 3 | M1 + M2 + M3 |

Bij overschrijding van een setpoint stijgt de stap; binnen de hysteresis daalt hij. De Temperatuur-stap en luchtvochtigheid-stap worden via de conflict-prioriteit gecombineerd.

### Dag/nacht-omschakeling

- **Berekening** vindt automatisch op basis van **datum + geografische locatie**: zonsopkomst en zonsondergang
- **Geografische locatie** (`latitude`, `longitude`) wordt **automatisch bepaald op basis van de internet-aansluiting** (geo-lookup via [ip-api.com](ip-api.com) eerste verbinding) en wordt daarna in het permanente geheugen bewaard. De beheerder kan deze handmatig overschrijven via System-tab → Location
- **Tijdzone** (`tz_str`) — POSIX-formaat, default voor Nederland: `CET-1CEST,M3.5.0,M10.5.0/3`*Wordt automatisch bepaald bij locatiebepaling.* De beheerder kan deze handmatig overschrijven via System-tab → NTP timezone → POSIX TZ string
- **Tijd** synchroniseert via NTP zodra WiFi-client is verbonden; bij geen verbinding gebruikt controller de interne klok

### Reboot-vereiste parameters

Niet alle parameters zijn live actief; sommige vereisen een power-cycle (Schakel het apparaat uit en weer in):

| Parameter | Reboot vereist? |
|---|---|
| Climate setpoints, hyst, avg_win, rh_ctrl_en, cr_priority | Nee — direct actief |
| Wind v_max, dir_excl_low/high, Wind protection | Nee — direct actief |
| Motor travel- + dwell-times | Nee — geldt voor volgende beweging |
| WiFi SSID/PSK client | Nee — connect/reconnect binnen ~30 s |
| WiFi AP enable / AP password | Nee — direct actief |
| **Sensor poll interval (`poll_interval_s`)** | **Ja — power-cycle vereist** |
| Timezone (`tz_str`) | Nee — direct actief |
| Locatie lat/lon | Nee — volgende dag/nacht-evaluatie; 1 keer per 24 uur |
| Session timeout | Nee — direct actief |
| LED brightness | Nee — direct actief |
| PIN's | Nee — direct actief |

---

## 5. De controller (fysiek)

`[FOTO: vooraanzicht kast met LCD, toetsenbord, LEDs]`

### De kast
- Schroefverbinding aan de wand met M5/M6 schroeven (placeholder)
- Kabelinvoer onderzijde, met drukwartels voor sensoren, motor-bedrading, voeding
- IP-rating: IP67

### 5.1 LCD-display

Voor algemene uitleg over schermen, auto-rotatie en mode-regel: zie de [boer-handleiding §5.1](handleiding.md#51-lcd-display-16--2-tekens).

**Aanvullend voor de beheerder**:

- **Achtergrondkleur LCD-display** geeft de veiligheidsstatus van de controller weer:
  - **Blauw** — normale werking (geen alarmen actief)
  - **Rood** — kritiek: **motor-alarm** OF **wind-alarm** OF **sensor-fault Temperatuur/Luchtvochtigheid**

### 5.2 Toetsenbord
Zie [boer-handleiding §5.2](handleiding.md#52-toetsenbord-4--4) voor het volledige overzicht van toetsfuncties per scherm. Geen Beheerder-specifieke uitbreidingen.

### 5.3 LED-indicatoren

#### RGB-LED kleuren
Zie [boer-handleiding §5.3](handleiding.md#53-led-indicatoren). 

> Nachtmodus voorkomt een felle LED in een verduisterde kas of woonruimte naast de kas.

#### Heartbeat-LED
groene LED Knippert 1× per seconde. **Knippert niet:**
- Controleer groene voedling LED; bij uit geen spanning.
- Probeer power-cycle of RESET-knop
- Bij blijvende uitval: Neem contact op met de leverancier (zie [§14](#14-onderhoud--wat-de-beheerder-doet))

---

## 6. De webinterface (via WiFi)

De webinterface is de primaire beheerdersinterface. Veel beheers-functies zijn alleen hier beschikbaar (niet via de bediening op de kast).

### Bereiken

Dezelfde route als voor de boer: lees IP-adres af van het LCD, het WiFi-scherm, open browser op laptop op hetzelfde WiFi-netwerk, typ het IP-adres in dat wordt getoond p het LCD-display.

> Voor eerste-installatie zonder bestaand WiFi-netwerk: gebruik de AP-modus — zie [§11 WiFi installatie](#11-WiFi-en-webinterface--installatie-en-beheer).

### Hoofdtabs en role-vereisten

| Tab | Rol | Functies |
|---|---|---|
| **Status** | Iedereen, *geen login* | Live Temparatuur, Luchtvochtigheid, wind snelheid en richting, raamposities, mode, alarmen, klok, WiFi, SD-kaart informatie |
| **Climate** | Boer + Beheerder | Setpoints; Beheerder ziet ook hyst, avg_win, rh_ctrl_en |
| **Wind** | Boer + Beheerder | wind_prot_en; Beheerder ziet ook v_max, dir_excl_low/high |
| **Motors** | **Alleen Beheerder** | M1/M2/M3 travel + dwell open/close |
| **System** | **Alleen Beheerder** | Sessie-timeout, AP-config, WiFi-client, NTP/timezone, locatie coordinaten, Over the Air Update (OTA) |
| **Log** | **Alleen Beheerder** | SD-kaart mount/unmount, log-bestanden downloaden |
| **Access** | **Alleen Beheerder** | PIN-management voor Farmer + Beheerder |

`[SCHERMAFBEELDING: webinterface met alle 7 tabs zichtbaar voor Beheerder]`

### Status-tab — Klok-tegel

Op de Status-tab staat in de linker tegelrij de **Klok-tegel**. Deze toont drie regels:

- **Tijd** — actuele lokale datum en tijd op de controller (format `YYYY-MM-DD HH:MM:SS`). De tijdzone wordt automatisch ingesteld na NTP-sync via geolocation, of handmatig via System-tab → NTP timezone.
- **NTP-badge** — `NTP synced` (groen) wanneer de klok deze sessie via NTP gesynchroniseerd is, `NTP pending` (rood) zolang dat nog niet gelukt is en de controller op de interne klok RTC met batterij-backup draait.
- **Uptime** — bedrijfsduur sinds de laatste start, ververst om de ~2 seconden. Het formaat past zich aan aan de beschikbare ruimte wat verschillende presentatie oplevert:
  - `5s` … `59s`
  - `1m 23s`
  - `2h 15m`
  - `1d 4h 23m`

  Bij een herstart van de controller springt deze waarde `0s` en begint opnieuw — handig om te zien of de controller stabiel draait.

### Status-tab — versie-controle van firmware en web-assets

Vanaf firmware 1.17.20 wordt een verschil tussen de **firmware-versie** (in de ESP32-image gecompileerd) en de **web-assets-versie** (uit `/manifest.json` op de actieve LittleFS-partitie) automatisch herkend en gemeld als alarm. Tijdens normaal bedrijf zijn beide versies gelijk; een afwijking wijst op een onvolledig uitgevoerde OTA-update.

**Hoe te zien op het dashboard**

Een mismatch verschijnt als een rode **MISMATCH**-badge in de **Alarms**-tegel op de Status-tab, naast eventuele andere alarmen (WIND, MOTOR ALARM, sensor-faults). Bij gelijke versies (of wanneer de assets-versie onbekend is — `?` op een schone serieel-geflasht systeem zonder OTA-pakket) blijft de badge weg en toont Alarms zoals gebruikelijk **OK** of de actieve mode-vlaggen.

**Onafhankelijke controle (handig bij twijfel of bij verdacht gedrag van de browser-cache)**

| Bron | Wat toont het | Hoe op te roepen |
|---|---|---|
| `http://<controller-ip>/manifest.json` | JSON met `asset_version` — leest het bestand direct van de actieve LittleFS-partitie | Browser of `curl`. Direct ground-truth. |
| Pagina-bron op de hoofdpagina | Regel 2: `<!-- web-assets X.Y.Z -->`, gestempeld door `bin/build_release.ps1` in de ZIP | Browser → *Pagina-bron weergeven* (Ctrl+U) |
| Webgui footer | Firmware-versie als `vX.Y.Z` | Onderaan de pagina, altijd zichtbaar |

Bij een **MISMATCH** voer als eerste een harde refresh uit van de webpagina (`Ctrl+Shift+R`); blijft de melding staan, dan is de OTA op de controller zelf incompleet. Voer de OTA-update opnieuw uit met **beide** pakketten — eerst `greenhouse-controller-X.Y.Z.bin`, daarna `web-assets-X.Y.Z.zip` — en wacht op de automatische herstart. Het mismatch-mechanisme werkt op basis van het `manifest.json`-bestand dat door `build_release.ps1` mee in de assets-ZIP wordt verpakt, dus de versie-vergelijking is betrouwbaar zonder dat de firmware moet weten welke ZIP er is geüpload.

> **Historische opmerking** — In firmware 1.17.4–1.17.9a stond op deze plek een tijdelijke **OTA diagnostic (temp)**-tegel met aparte Firmware/Assets-regels en een eigen MISMATCH-badge. Die tegel is sinds 1.17.20 verwijderd nadat de onderliggende oorzaak (een collisie tussen beide LittleFS-partities op dezelfde VFS-mountpoint) is opgelost; de versie-controle zelf is behouden en geïntegreerd in de Alarms-tegel. Zie de changelog onder `1.17.9` en `1.17.20` voor de technische details.

---

## 7. De twee gebruikersrollen

### Boer (kasgebruiker)
- 4-cijferige PIN
- Fabrieksstandaard: `1234` — moet bij eerste gebruik worden gewijzigd door de boer of door de beheerder namens de boer
- **Mag** klimaat-setpoints en windbeveiliging instellen, eigen PIN wijzigen (web)
- Kan niet bij motor-, WiFi-, systeem-, toegang- of log-instellingen

### Beheerer (Technisch beheerder van installatie)
- 8-cijferige PIN
- **Geen default in deze handleiding genoemd** — wordt door installateur ingesteld bij oplevering, of via fysieke reset op fabrieksstandaard teruggezet (zie [§18](#18-reset-procedure-boot-knop-op-microprocessorboard))
- Volledige toegang tot alle beheers-functies

### Lockout
- 5 opeenvolgende foute PIN-pogingen → 5 minuten lockout voor die rol
- Lockout-teller en -duur per rol apart

### PIN-opslag
- PIN's worden opgeslagen als **salted SHA-256 hash** in permanent gehuegen
- De salt is 16 bytes random getal, en wordt gegenereerd bij eerste boot
- Plaintext-PIN wordt nooit opgeslagen of gelogd

### PIN-management voor de Beheerder (webinterface, Access-tab)

`[SCHERMAFBEELDING: Access-tab met PIN-change formulieren voor Farmer en Beheerder]`

#### Eigen Beheerder-PIN wijzigen
1. Inloggen als Beheerder (8 cijfers)
2. Tab **Access**
3. In het Beheerder-PIN formulier: huidige PIN + nieuwe PIN (2× ter bevestiging)
4. Klik **Apply**
5. Bij succes: bevestigingsmelding; je sessie blijft actief

#### Boer-PIN wijzigen / resetten
1. Inloggen als Beheerder
2. Tab **Access**
3. In het Beheerder-PIN formulier: nieuwe PIN (2× ter bevestiging)
4. Klik **Apply**
5. Communiceer de nieuwe PIN aan de Boer

> **Boer is zijn PIN vergeten** → Beheerder reset Boer-PIN via deze procedure.
> **Beheerder is zijn PIN vergeten** → fysieke reset niveau 1 op de IO0-knop (zie [§18](#18-reset-procedure-boot-knop-op-microprocessorboard)).

### Sessie-timeout instellen
- Default: 5 min
- Bereik: 1–1440 min (1 dag)
- Webinterface: System-tab → veld → `session timeout (min)`
- Geldt voor zowel Boer- als Beheerder-sessies

---

## 8. Gebruik zonder inloggen — informatiemenu

Geen beheer-specifieke verschillen. Zie [boer-handleiding §8](handleiding.md#8-gebruik-zonder-inloggen--informatiemenu) voor de volledige uitleg.

Alle beheer-functies vereisen een ingelogde sessie als Beheerder.

---

## 9. Inloggen als beheerder

### Op de kas controller (LCD)

Inloggen via het hoofdmenu — **dezelfde route als boer maar met Beheerder-PIN**:

1. Druk vanaf elk statusscherm een willekeurige toets behalve `D` om het hoofdmenu te openen
2. Druk `3` voor Access-menu
3. Druk `2` voor **Beheerder** (in plaats van `1` voor Boer)
4. PIN-invoerscherm verschijnt: `PIN (8 dig) *=Bk` — voer **8 cijfers** in
5. Druk `#` om te bevestigen
6. Bij succes: `Access granted` / `Welcome!`; `Sess: Beheerder` op het modus-scherm

### Snelweg via #-toets

Op vier statusschermen kan je direct vanuit de auto-rotatie naar een instellingen-menu springen, waarbij PIN-invoer eerst wordt gevraagd als je nog niet ingelogd bent. Vanaf firmware 1.16.39 staat er **geen zichtbare hint** meer op rij 2 — `#` werkt op elk statusscherm dat een instellingen-menu heeft, en wordt genegeerd op de overige schermen (Mode/Sess en Raamposities).

- **T/RH-status (scherm 1)** — `#` → vraagt **Boer-PIN** (4 cijfers) → daarna direct in Climate-menu (Day/Night setpoints + Conflict-prioriteit)
- **Wind-status (scherm 2)** — `#` → vraagt **Boer-PIN** → daarna direct in Wind-menu (`Wnd-max`, `Wnd-prot`)
- **WiFi-status (scherm 4)** — `#` → vraagt **Beheerder-PIN** (8 cijfers) → daarna direct in System-menu (waar je AP kunt aan/uit zetten)
- **Time-status (scherm 5)** — `#` → vraagt **Beheerder-PIN** → daarna direct in datum/tijd-invoer

> **Let op**: ben je al ingelogd als boer, dan zal `#` op de WiFi- of Time-status alsnog om de Beheerder-PIN vragen (deze schermen zijn admin-only). Andersom werkt voor een Beheerder elk van de vier sneltoetsen direct zonder extra PIN-invoer.

### In de webinterface

1. Open de webinterface
2. Tab **Access** (of Login-knop)
3. Klik **Beheerder**
4. Voer 8-cijferige Beheerder-PIN in
5. Klik **Login**

### Uitloggen
- LCD: hoofdmenu → `3:Access` → `3:Logout`
- Web: knop **Logout** in Access-tab
- Automatisch na sessie-timeout (default 5 min)

### Verschillen met Boer-login
- 8 cijfers in plaats van 4
- Eigen lockout-teller (Beheerder lockout blokkeert Boer-login niet en omgekeerd)
- Toegang tot tabs Motors, System, Log, Access

---

## 10. Klimaat instellen

### 10.1 Op de kas controller (LCD en numeriek toetsenbord)

De LCD-route voor de door de **boer-bewerkbare** instellingen (T-max dag/ngt, RH-max/min dag/ngt, T vs RH conflict priority) staat in de [boer-handleiding §10.1](handleiding.md#101-op-de-kas-controller).

**Beschikbaar via LCD voor de Beheerder** (allemaal ook voor de Boer toegankelijk):
- Climate-menu → 1 dag / 2 nacht → setpoints
- Climate-menu → 3 CR (Conflict-prioriteit)
- Wind-menu → Wnd-max, Wnd-prot

**NIET via LCD beschikbaar** (alleen via webinterface):
- hysteresis Temperatuur en Luchtvochtigheid
- Glijdend gemiddelde Temperatuur en Luchtvochtigheid
- Vochtregeling aan en uitschakelen
- Wind-uitsluitings-zone
- Alle motor-tijden (M1/M2/M3 travel + dwell)

### 10.2 In de webinterface (tab Climate)

`[SCHERMAFBEELDING: tab Climate, ingelogd als Beheerder, met Beheerder-only sectie zichtbaar]`

#### Boer-bewerkbare velden (Farmer + Beheerder)

Per setpoint: schuifregelaar + nummerveld + **Apply**-knop.

| Label | Default | Bereik |
|---|---|---|
| Temperatuur max dag (°C) | 28 | 15–45 |
| Temperatuur max nacht (°C) | 20 | 10–35 |
| Luchtvochtigheid (RH) max dag (%) | 75 | 40–98 |
| Luchtvochtigheid (RH) max nacht (%) |  — | 40–98 |
| Luchtvochtigheid (RH) min dag (%) |  50 | 20–90 |
| Luchtvochtigheid (RH) min nacht (%) | — | 20–90 |
| Luchtvochtigheid (RH) besturing | Aan | Uit/Aan |
| Temperatuur/Luchtvochtigheid conflict prioriteit | 0 | 0–2 |

#### Beheerder-only velden (Climate-tab onderzijde)

| Label | Default | Bereik | Eenheid |
|---|---|---|---|
| Temperatuur hysteresis | 5 | 2–15 | °C |
| Luchtvochtigheid hysteresis | 12 | 2–20 | % |
| Temperatuur gemiddelde window | 6 | 1–30 | min. |
| Luchtvochtigheid gemiddeld window | 10 | 1–30 | min. |

> **Apply per veld**: klik **Apply** na elke wijziging. Anders gaat de wijziging verloren bij navigatie.

### 10.3 Wind-tab

`[SCHERMAFBEELDING: tab Wind, ingelogd als Beheerder]`

#### Boer + Beheerder
|  Label |  Default | Bereik |
|---|---|---|---|---|
| Wind protection | Aan | Uit/Aan |

#### Beheerder-only

**### uitleg over de wind instellingen toevoegen**

| Label |  Default | Bereik | Eenheid |
|---|---|---|---|
| Wind speed max | 6 | 1–30 | m/s |
| Excl. zone low | — | 0–359 | ° |
|  Excl. zone high | — | 0–359 | ° |

> **Wind-uitsluitings-zone**: windrichting waarbij de wind extra gevaarlijk is (bijvoorbeeld omdat ramen rechtstreeks in deze richting staan). Wind binnen deze hoek triggert wind-override ongeacht windsnelheid.

> Windsnelheid kan worden uitgedrukt in **Beaufort (Bft)** of in **meter per seconde (m/s)**.  
De **Beaufortschaal** geeft aan hoe sterk de wind is op basis van het effect dat de wind heeft, van **0 Beaufort (windstil)** tot **12 Beaufort (orkaan)**.  
**m/s** is een natuurkundige eenheid die direct aangeeft hoeveel meter de lucht per seconde aflegt.

Hieronder staat een conversietabel van **Beaufort naar m/s**:

| Beaufort | Omschrijving         | Windsnelheid (m/s) |
|---------:|----------------------|--------------------|
| 0        | Windstil             | 0,0 – 0,2          |
| 1        | Zwakke wind          | 0,3 – 1,5          |
| 2        | Zwakke bries         | 1,6 – 3,3          |
| 3        | Matige bries         | 3,4 – 5,4          |
| 4        | Matige wind          | 5,5 – 7,9          |
| 5        | Vrij krachtige wind  | 8,0 – 10,7         |
| **_6_**  | **_Krachtige wind_** | **_10,8 – 13,8_**  |
| 7        | Harde wind           | 13,9 – 17,1        |
| 8        | Stormachtige wind    | 17,2 – 20,7        |
| 9        | Storm                | 20,8 – 24,4        |
| 10       | Zware storm          | 24,5 – 28,4        |
| 11       | Zeer zware storm     | 28,5 – 32,6        |
| 12       | Orkaan               | ≥ 32,7             |


### 10.4 Motors-tab (alleen Beheerder)

`[SCHERMAFBEELDING: tab Motors met M1, M2, M3 instellingen]`

| Veld | Default M1 | Default M2 | Default M3 | Bereik |
|---|---|---|---|---|
| Travel time |  21 s | 21 s | 171 s | 5–300 s |
| Dwell open | 300 s | 300 s | 1500 s | 0–1500 s |
| Dwell close | 300 s | 300 s | 600 s | 0–1500 s |

> **Travel-time afstemming**: meet de werkelijke open- of sluit-tijd van een raam met een stopwatch. Stel die waarde in als *travel-time.* Dde firmware voegt zelf een veiligheidsmarge toe van 5 sec. De controller gebruikt deze waarde als time-out voor het OPEN/CLOSE-relais.
>
> **Dwell-tijden aanpassen**: bij oscillatie (raam gaat steeds open/dicht in een korte cyclus) → dwell-tijd verhogen. Bij trage reactie op klimaat-veranderingen → dwell verlagen. Begin met de standaar instellingen; pas deze waarden alleen aan na minimaal 1 dag observeren.

### 10.5 Adviezen voor instelling per teelttype

`[TABEL: richtwaarden per teelt — door teler / leverancier planten in te vullen]`

Algemene vuistregels:
- **Nacht-Temperatuur** iets lager dan dag-Temperatuur (planten besparen energie)
- **Luchtvochtigheid boven 85%** voor langere tijd; verhoogd schimmelrisico
- **Luchtvochtigheid onder 50%** kan groei remmen
- Begin met **default-waarden** en stel pas bij na een week observeren

---

## 11. WiFi en webinterface — installatie en beheer

### 11.1 Eerste keer WiFi configureren (na fabrieksreset of nieuwe installatie)

Wanneer de kascontroller voor het eerst wordt aangesloten of na een reset niveau 2/3, is er nog geen WiFi-verbinding. Volg de volgende procedure:

1. **Schakel het AP in op de controller**:
   - Druk vanaf elk statusscherm `D` totdat je op het **WiFi-status scherm** (scherm 4) bent
   - Druk `#`
   - Voer Beheerder-PIN in (8 cijfers), druk `#`
   - Het System-menu opent met `1=WiFi AP`
   - Druk `1` om AP te activeren — bevestiging `WiFi AP / enabling...`
   - LCD toont nu `WiFi: AP active` met SSID `Greenhouse-XXXX`

2. **Verbind je laptop/telefoon met het AP**:
   - Zoek WiFi-netwerk `Greenhouse-XXXX` (waar XXXX de laatste twee bytes van het MAC-adres zijn)
   - AP-wachtwoord: standaard `0123456789` — **wijzig dit zo snel mogelijk** (zie §11.4)
   - Verbind met het WiFi netwerk.

3. **Open in je browser de webinterface op het AP**:
   - Browse naar → `http://192.168.4.1`
   - Login als Beheerder

4. **Configureer client-mode WiFi**:
   - Tab **System** → sectie **WiFi client**
   - Stel in het veld **SSID** de naam van het WiFi netwerk in
   - Stel in het veld ** Password** het WiFi-wachtwoord in
   - Klik **Connect**
   - De controller probeert binnen ~30 sec. verbinding te maken; Het LCD toont `WiFi: connecting...` daarna `WiFi: connected` en het IP-adres van de controller

5. **Verbind je apparaat opnieuw met het kas-/thuisnetwerk** en open de webinterface op het nieuwe IP-adres. Het AP kan nu uitgezet worden:
   - Op de controller: System-menu → `1` om AP weer uit te schakelen, of wacht tot dat het AP automatisch wordt uitgeschakeld na 30 min.

### 11.2 AP-timeout

| Veld | Beschrijving | Default | Bereik |
|---|---|---|---|
| AP timeout (min, 0=never) | Min. tot AP automatisch uitschakelt | 30 | 0 = nooit, 1–∞ min. |

Voorkomt dat een vergeten AP-modus permanent open blijft staan.

### 11.3 AP-wachtwoord wijzigen

> **Sterk aangeraden bij installatie**: wijzig het AP-wachtwoord van fabriekswaarde `0123456789` naar iets unieks per kas.

1. Inloggen als Beheerder
2. Tab **System** → sectie **WiFi AP**
3. Veld `AP password` (max 63 tekens, min 8 tekens voor WPA2)
4. Klik **Apply**

### 11.4 WiFi client SSID/PSK wijzigen

Volg dezelfde procedure als bij eerste configuratie (§11.1 stap 4). Tijdens uitvoer is de controller kort niet bereikbaar tot de WiFi verbinding is hersteld (~30 sec.).

### 11.5 Statisch IP

De firmware ondersteunt **geen** statische IP adressen en werkt alleen met een **DHCP-reservering** op de internet-router (op MAC-adres) als je een vast IP wilt.

### 11.6 mDNS / hostname

De controller registreert zichzelf niet op de lokale DNS server; gebruik altijd het IP-adres. Maak indien gewenst een eigen DNS-record in je router.

### 11.7 NTP en tijdzone

| Instelling | Beschrijving | Default | Voorbeeld |
|---|---|---|---|
| `POSIX TZ string` | POSIX TZ-string | (Nederlandse tijdzone) | `CET-1CEST,M3.5.0,M10.5.0/3` |

- **NTP-synchronisatie**: is automatisch zodra WiFi-client verbonden is. Default server is: [pool.ntp.org](pool.ntp.org) en is niet configureerbaar
- **Tijdzone-wijziging**: webinterface System-tab → veld `POSIX TZ string`. Een aanpassing is direct actief, geen reboot nodig
- **daglight Saving**: Wordt automatisch bepaald; meegenomen in POSIX TZ-string (zie default Nederland)
- **Tijd handmatig instellen** Wanneer je geen internet toegang hebt: Op de contrller: LCD time-status (scherm 5) → `#` → Beheerder-PIN → datum invoer DDMMYY → `#` → tijd invoer HHMM → `#`

### 11.8 Geografische locatie

Deze wordt gebruikt voor het bepalen van zonsopkomst/zonsondergang en automatische dag/nacht-omschakeling.

| Beschrijving | Default | Eenheid |
|---|---|---|
| Latitude (graden + fractie) | 52.0 | ° (Nederland) |
| Longitude (graden + fractie) | 5.0 | ° (Nederland) |

**Automatische detectie**: bij eerste WiFi-verbinding doet de controller een geo-lookup via [ip-api.com](ip-api.com) en vult latitiude/longitude automatischwaarna dit in het permanente geheugen wordt bewaard.
> Wanneer de Wifi verbinding via een mobiele internet verbinding loopt kan de geo-lookup onverwachte resultaten opleveren

**Handmatig aanpassen locatie**: in de webinterface: System-tab → sectie **Location** → velden `latitude`, `longitude`. Notatie is Decimal-graden met teken (positief = N/E, negatief = S/W). *Bijvoorbeeld: 52.218, 5.939* 

### 11.9 Sessie-timeout

| Invul veld | Beschrijving | Default | Bereik |
|---|---|---|---|
| `Session timeout (min)` | Idle-timeout in minuten | 5 | 1–1440 |

Geldt voor zowel LCD als webinterface. Een te lange waarde (bv. 60 min) is een veiligheidsrisico — een vergeten ingelogde sessie kan worden misbruikt.

---

### 11.10 Status-rapportage naar extern webdashboard (tab **Web**) — nieuw in 1.17

De kascontroller kan zijn actuele toestand periodiek naar een **externe web-server** sturen. Op die web-server draait een dashboard dat dezelfde gegevens toont als de eigen webinterface — zo kan iemand op afstand toch de werking van de kas volgen. Daarnaast wordt het laatst-gesloten logbestand van de SD-kaart één keer per dag (en/of bij elke logrotatie) naar dezelfde server geüpload.

De feature staat **standaard uit**. Inschakelen gebeurt volledig in de webinterface, tab **Web** (alleen zichtbaar voor de Beheerder).

#### Werkingsoverzicht

```
┌──────────────────┐                  ┌────────────────────┐
│  Kascontroller   │  POST status     │ uw-server.nl/.../  │
│   (firmware)     │ ───────────────► │     api.php        │
│                  │  (om de 60–300 s)│                    │
│                  │                  │   slaat laatste    │
│                  │  POST logbestand │   status op disk   │
│                  │ ───────────────► │   serveert via     │
│                  │  (1×/dag of rota-│   view.php aan     │
│                  │   tie SD-log)    │   het dashboard    │
└──────────────────┘                  └────────────────────┘
```

Iedere POST draagt een **shared secret** in de HTTP-header `sourceidentifier`. De web-server vergelijkt die met zijn eigen **shared secret**; bij verschil wordt de informatie zonder terugmelding verworpen. Het **shared secret** staat dus letterlijk op twéé plaatsen — kascontroller en web-server — en moet bij een aanpassing aan beide kanten worden aangepast.

#### Velden op tab Web

| Veld | Beschrijving | Default | Bereik / regels |
|---|---|---|---|
| `URL` | Eindpunt van het PHP-script | leeg | Moet beginnen met `http://` of `https://`, mag géén `?` of `#` bevatten, móét eindigen op `api.php`. Maximaal 128 tekens. Leeg laten = functie uit. |
| `Shared secret` | Token in `sourceidentifier`-header | leeg | Minimaal 16 tekens. Leeg laten bij `Apply` = bestaande token blijft staan. Wordt nooit teruggetoond bij heropenen van het tabblad. |
| `Interval (s)` | Tijd tussen POST's | 120 | 60–300 |
| `Enabled` | Hoofdschakelaar | uit | aan / uit. Bij `uit` worden geen POST's verstuurd, ook niet als URL en token correct zijn ingevuld. |
| `Climate / Wind / Windows / Mode / Sun / System` (6 vinkjes) | Welke tegels worden meegestuurd | alle 6 aan | Een uitgevinkt vinkje laat het bijhorende JSON-object weg uit de POST → de tegel verschijnt automatisch niet op het publieke dashboard. |
| `Daily upload time` | Lokale tijd waarop log-upload geprobeerd wordt | 03:15 | uu : mm, 24-uur klok |
| `Upload on rotation` | Ook uploaden zodra T9 een logbestand sluit | aan | aan / uit |

#### Actuele informatie over de werking van deze functie

Onderaan tab *Web* staan drie regels die elke 5 seconden ververst worden (zolang u op dit tabblad staat). Ze tonen wat T14 zelf intern weet, ze zijn niet handmatig in te vullen:

| Regel | Inhoud | Voorbeeld |
|---|---|---|
| `Last post` | Datum/tijd en uitkomst van laatste status-POST | `OK 2026-05-10 14:30:22` of `FAIL 2026-05-10 14:30:22` |
| `Last log upload` | Idem voor laatste log-upload | `OK 2026-05-10 03:15:08` of leeg als nooit geprobeerd |
| `Last uploaded file` | Bestandsnaam van het laatst succesvol geüploade logbestand | `20260507143022.csv` |

De auto-refresh raakt alleen deze drie regels aan — uw invoer in `URL`, `Shared secret`, intervalkeuze of vinkjes wordt nooit overschreven terwijl u typt. Pas op het moment dat u op **Apply** klikt worden de waarden eerst gevalideerd, daarna naar NVS geschreven en daarna teruggelezen, zodat de formuliervelden exact tonen wat er in NVS staat.

#### Eerste keer instellen — stap voor stap

1. Log in als Beheerder en open tab **Web**.
2. Vul de URL van het PHP-eindpunt in (bv. `https://uw-server.nl/hbwv/api.php`).
3. Vraag aan de beheerder van de web-server de waarde van het shared secret. Plak die in `Shared secret`. Laat het secret-veld leeg als u het later eens wilt wijzigen zonder het opnieuw te hoeven invullen.
4. Stel het update `Interval (s)` in op een geschikte waarde — `120` is een goede default (niet te druk op het netwerk, dashboard blijft binnen 5 minuten "vers").
5. Vink desgewenst tegels uit die u **niet** publiek wilt tonen. Standaard staan alle zes aan.
6. Eventueel `Daily upload time` aanpassen naar een rustig moment in uw netwerk (bv. nacht).
7. Vink `Enabled` aan.
8. Klik **Apply**.

Binnen één intervalperiode hoort de regel `Last post` op `OK …` te springen. Blijft hij op `FAIL …` of leeg staan? Zie [§11.10 troubleshooting](#1110-troubleshooting-status-rapportage) hieronder.

#### Functie tijdelijk uitschakelen

Twee opties:
- Vink `Enabled` uit en klik **Apply**. URL en token blijven bewaard.
- Maak het URL-veld leeg en klik **Apply**. De feature is dan ook uit, ongeacht de stand van `Enabled`.

Bij OTA-firmware-update worden status-POST's automatisch overgeslagen totdat de update klaar is — u hoeft niks handmatig uit te zetten.

#### HTTPS

Endpoints met `https://` worden ondersteund. **De controller controleert het certificaat NIET** (De verbinding is versleuteld maar niet geauthenticeerd). Dat is een bewuste keuze: anders moest de firmware een actuele CA-bundel meedragen en periodiek updaten. De gedeelde token in de header is de eigenlijke authenticatie. Wijzig de token meteen als u vermoedt dat hij is gelekt.

#### Veelgemaakte fouten

| Symptoom | Oorzaak | Oplossing |
|---|---|---|
| Bij **Apply** verschijnt rood `URL must end with "api.php"` | URL eindigt op een directorypad zoals `/api/` | Voeg `api.php` toe; De HTTP-Client volgt geen 301-redirects, dus de server-side `DirectoryIndex` kan niet vertrouwd worden |
| Rood `URL must not contain ? or #` | Query-parameters in de URL | Verwijder ze — de firmware voegt zelf `?action=log` toe voor de log-upload |
| Rood `secret too short` | Minder dan 16 tekens | Vraag de beheerder van de website om een langer token |
| `Last post` blijft `FAIL` | Server bereikbaar maar weigert ('wrong secret') | Controleer dat `Shared secret` byte-exact gelijk is aan shared secret op de web-server. Spaties/tabs aan einde tellen mee! |
| `Last post` blijft leeg | WiFi-client verbinding niet actief, of klok niet via NTP gesynchroniseerd | Zie [§11.1](#111-eerste-keer-WiFi-configureren-na-fabrieksreset-of-nieuwe-installatie) (WiFi) of [§11.7](#117-ntp-en-tijdzone) (NTP). T14 wacht op beide vóórdat hij verstuurt. |
| Publiek dashboard toont een tegel met verkeerde inhoud | Mismatch in veldnamen tussen kascontroller-firmware en het PHP-dashboard | Beide moeten van dezelfde release-generatie zijn. Firmware 1.17.1 hoort bij `pe1mew.nl/hbwv` van mei 2026 of nieuwer. |

#### Logbestand-upload — wat gaat er precies heen?

De controller upload het **meest recent gesloten** CSV-logbestand op de SD-kaart (dus niet het bestand waar T9 op het moment van uploaden nog in schrijft). De bestandsnaam is van de vorm `YYYYMMDDHHMMSS.csv` (lokale tijd van aanmaak) en is maximaal 512 KB groot — daarboven heeft T9 het al gerouteerd naar een nieuwer bestand.

Twee triggers, beide aan te zetten of uit te zetten:
- **On rotation:** zodra T9 een logbestand sluit (omdat het 512 KB heeft bereikt), wordt het vrijwel direct geüpload.
- **Daily:** elke dag rond `Daily upload time` lokaal wordt het laatst-gesloten bestand opnieuw beoordeeld; staat het al onder `Last uploaded file`, dan wordt het geslagen — anders wordt het geüpload.

Door deze dubbele aanpak met dedup-op-bestandsnaam wordt hetzelfde bestand nooit twee keer geüpload, ook als de rotatie en de dagelijkse check op verschillende dagen vallen.

---

## 12. Alarmen en bedrijfsmodi — diagnose en herstel

Voor algemene uitleg van bedrijfsmodi: zie [boer-handleiding §12](handleiding.md#12-alarmen-en-bedrijfsmodi--wat-betekenen-ze-wat-te-doen).

Onderstaande secties verdiepen de diagnose vanuit beheerder-perspectief.

### 12.1 Windbeveiliging — fijn afstemmen

#### `v_max` instellen op kas-locatie

- Default 6 m/s is voor de meeste Nederlandse kassen veilig
- Bij blootstelling aan harde rukwinden: **verlagen naar 4–5 m/s**
- Bij geluwde locatie: **verhogen tot 8–10 m/s**
- Combineer met een **groter `avg_win_w`** (gemiddeld windvenster) om kortdurende rukwinden te dempen

#### Uitsluitings-zone

Stel in als ramen rechtstreeks blootgesteld zijn aan een specifieke windrichting (bijv. zuidwesten).

Voorbeeld: ramen blootgesteld aan zuidwesten (180–270°):
- `dir_excl_low = 180`
- `dir_excl_high = 270`

Wanneer Noord binnen de uitsluitings-zone ligt (door 0°): bijv. uitsluiting NO-N-NW = 315–45°:
- `Dir excl. low = 315`
- `Dir excl. high = 45`

Zone uitschakelen: `Dir excl. low = Dir excl. high` of negatief.

#### Geen hysteresis — hoe omgaan met flapperen

**### Dit moet uitgezocht worden hoe het is geimplementeerd en instelbaar is**

Bij wind rond `v_max` kan de override snel in/uit-flikkeren. **Verhoog `avg_win_w`** (Beheerder-only, namespace `system`, default 1 min., Bereik 1–30 min.). Een venster van 5–10 min. dempt flikkering goed.

### 12.2 Motor-alarm — diagnose

Voor algemene uitleg: zie [boer-handleiding §12.6](handleiding.md#126-motor-alarm-in-detail).

**Diagnose-stappen door de beheerder**:

1. **Lees Hotraco RRK-3 status-LED's af** (op de RRK-3 zelf, niet op de kascontroller). Welke LED brandt rood / knippert?
2. **Visuele inspectie** van de drie ramen: zit er één vast? Iets in de weg?
3. **Eindschakelaar-controle**: is de eindschakelaar van het verdachte raam mechanisch in orde? Kabel los?
4. **Motor-zekering** in de RRK-3 (per kanaal) — controleer
5. **Stroommeting** op de motor-bedrading bij OPEN- of CLOSE-actie (clamp-meter) — overbelasting wijst op vastloper of mechanisch probleem
6. **Reset RRK-3** alleen na bevestigde diagnose — anders triggert het alarm direct opnieuw

> Procedure RRK-3 reset: zie Hotraco-handleiding (handmatige procedure op de RRK-3 zelf, geen actie op de kascontroller).

### 12.3 Sensor-fault — diagnose

Bij `** SENSOR FAULT` op LCD (Temperatuur/Luchtvochtigheid-sensor) of `--` op windscherm (wind-sensor):

> Achtergrond over hoe Modbus RTU en RS485 werken — handig bij het volgen van onderstaande diagnose: zie [Bijlage E](#bijlage-e--modbus-rtu-een-digitale-sensor-interface).

#### Diagnose-checklist
1. **Voeding op de sensor**: 9–36 V DC? (FG6485A en SenseCAP S200 hebben beide een groot voedingsBereik)
2. **RS485 bedrading**:
   - A en B niet verwisseld? Standaardconventie: A = -, B = +
   - Afscherming aan één kant geaard (niet beide kanten, dit voorkomt een aardlus)
   - Maximaal ~1200 m bij 9600 baud (in praktijk veel korter); gebruik ktwisted-pair
3. **Afsluitweerstanden**: 120 Ω op beide einden van de RS485-bus. Bij ster-topologie of T-aftakkingen kan reflectie ontstaan
4. **Modbus-adres** klopt met sensor-config? Modbus adres van de FG6485A moet `1` zijn, de SenseCAP S200 moet ades `44` zijn (controleer de  leverancier-documentatie om dit zo in te stellen.) *De modbus adressen zijn niet instelbaar in de controller.* 
5. **EMI-bron** in de buurt: dimmers, lasapparatuur, frequentieregelaars kunnen RS485 verstoren
6. **Sensor-condensvorming** (Temperatuur/Luchtvochtigheid): een koud-warm overgang kan vocht in behuizing laten condenseren → wachten of vervangen
7. **Sensor-afscherming** (wind): De windmeter is bedekt door blad of vuil → reinigen

#### Logbestand-analyse

- Webinterface tab **Log** → SD-bestanden of NVS-ringbuffer downloaden (CSV-formaat)
- Zoek naar `ALARM`-events met sensor-fault-flag
- Tijdstempels (UTC in het bestand) correleren met externe gebeurtenissen (storm, stroomdip)

> **Logbestand omzetten naar leesbare tekst**: in de git-repo zit een Python-script (`log/logparser.py`) waarmee je een ruw CSV-logbestand kunt omzetten naar een geformatteerde tekst-versie met begrijpelijke beschrijvingen per event. Voor het complete log-formaat (alle velden, event-types en parameter-ID's) en het gebruik van het script: zie [Bijlage F — Logbestand-formaat en `logparser` script](#bijlage-f--logbestand-formaat-en-logparser-script).

### 12.4 Sensor-poll-interval afstemmen

| Configuratie item | Beschrijving | Default | Bereik |
|---|---|---|---|
| `Sensor poll interval` | Sensor-leesfrequentie | 30 s | 30–300 s |

> **Reboot vereist** na wijziging.

- Korter (15–30 s): snellere reactie, meer Modbus-traffic
- Langer (60–120 s): minder bus-belasting, minder snelle reactie, langer voordat sensor-fault wordt gedetecteerd (2 mislukte polls)

### 12.5 RGB-LED kleuren samengevat

Zie [boer-handleiding §12.3](handleiding.md#123-rgb-led-kleuren-samengevat). LCD-achtergrond mirror-t dezelfde status (blauw=OK, rood=alarm).

### 12.6 Logbestand-formaten

- **NVS-ringbuffer**: laatste ~100 events, altijd in geheugen
- **SD-bestanden**: CSV-bestanden per opstart-sessie, opgeslagen op SD-kaart als die gemount is. Bestandsnamen volgen het patroon `YYYYMMDDHHMMSS.csv` (lokale tijd)
- **CSV-velden**: timestamp (ISO 8601 UTC), event_type, initiator, ch, param, value_a, value_b
- **Event-types**: `SENSOR`, `RELAY`, `MODE`, `SETPT`, `SESSION`, `ALARM`, `SYSTEM`
- Download via webinterface tab **Log**

Voor de complete uitleg van het logbestand-formaat (alle velden, event-types, parameter-ID's, channel-states, alarm-codes) en het gebruik van het meegeleverde `logparser`-script: zie [Bijlage F](#bijlage-f--logbestand-formaat-en-logparser-script).

---

## 13. Inschakelen na stroomuitval

Voor algemene procedure: zie [boer-handleiding §13](handleiding.md#13-inschakelen-na-stroomuitval).

### Beheerder-specifieke checklist

1. **Datum/tijd** klopt? RTC-batterij oké? (Zo niet: handmatig instellen via LCD time-status `#`, eventueel batterij vervangen)
2. **WiFi-verbinding** komt terug? Anders: AP activeren en client-config opnieuw instellen (zie [§11.1](#111-eerste-keer-WiFi-configureren-na-fabrieksreset-of-nieuwe-installatie))
3. **Setpoints** nog correct? worden in het permanente geheugen bewaart, dus zou onveranderd moeten zijn
4. **Kalibratie** loopt door? Controleer `Mode:Window Cal.` daarna `Mode: AUTO`

### Kalibratie-problemen

- **Raam blijft in `MOV>` of `MOV<` hangen**: eindschakelaar in RRK-3 niet bereikt → mechanische obstructie of eindschakelaar-defect
- **`Mode: ALARM` direct na opstart**: motor-alarm was al actief tijdens stroomuitval → reset RRK-3, dan power-cycle kascontroller om kalibratie af te dwingen

### RTC-batterij verlies

Symptomen:
- Datum/tijd springt na stroomuitval naar epoch (1970/2000)
- Dag/nacht-omschakeling klopt niet meer
- Logbestanden krijgen onjuiste timestamps

→ Vervang **CR2032** op het microprocessorboard, zie [§14](#14-onderhoud--wat-de-beheerder-doet).

### Langdurige stroomuitval
- Permanente geheugen blijft volledig bewaard (flash-geheugen)
- RTC loopt door tot CR2032 leeg is (~5–7 jaar)
- Bij terugkomst van stroom: normale kalibratie-cyclus start gevolgd door normale besturing

---

## 14. Onderhoud — wat de beheerder doet

### Periodiek onderhoud (jaarlijks aanbevolen)

- **Sensor-kalibratie controleren**: vergelijk Temperatuur/Luchtvochtigheid-meting met onafhankelijke referentie (geijkte hygro-thermometer); bij afwijking >1 °C of >5%RH overwegen sensor te vervangen of kalibreren
- **Wind-sensor kalibratie**: visueel controleren dat de ultrasonore stralenbundel niet bedekt of beschadigd is
- **Bedrading inspecteren**: corrosie, breuken, losse aansluitingen — vooral bij feedthroughs door de kasconstructie
- **Eindschakelaars in RRK-3**: handmatig elk raam open/dicht laten gaan (via handbediening, zie [§15](#15-handmatige-overname-via-de-motorbox)) en horen / zien dat de motor stopt op de eindschakelaar
- **Stof / vuil uit kascontroller-kast**: open de kast met spanning eraf, droge perslucht (geen vochtige doek)
- **CR2032 RTC-batterij**: meet spanning (>2.7 V = goed; <2.5 V = vervangen)

### CR2032 RTC-batterij vervangen

`[FOTO: CR2032 batterijhouder op het microprocessorboard, met de juiste oriëntatie + plus zichtbaar]`

1. Voeding van kascontroller eruit (stekker of zekering uit)
2. Open de kast
3. Lokaliseer de batterijhouder op het microprocessorboard
4. Klik de oude CR2032 voorzichtig uit de houder
5. Plaats nieuwe CR2032 met `+` zijde naar boven (zoals aangegeven in de houder)
6. Sluit de kast
7. Voeding aan; controleer dat datum/tijd klopt op LCD; corrigeer indien nodig (LCD time-scherm → `#`)

### Firmware-update / OTA

> **Belangrijk**: doe firmware-updates op een rustig moment. De kascontroller is tijdens de update niet beschikbaar voor klimaatregeling.

#### Voorbereiding
- Download de juiste `.bin` van de leverancier of de `bin/` map van dit repo met de gewenste software versie
- Download de juiste `.zip` van de leverancier of de `bin/` map van dit repo met de gewenste software versie
- Zorg voor stabiele WiFi-verbinding tussen laptop en kascontroller

#### Procedure
1. Inloggen als Beheerder in webinterface
2. Tab **System** → sectie **OTA update**
3. Klik **Browse** bij **Firmware (.bin)** en selecteer het binary-bestand
4. Klik **Upload**
5. Klik **Browse** bij **Web assets (.zip)** en selecteer het zip-bestand
6. Klik **Upload**
7. De controller schrijft het bestand naar de inactieve flash-bank
8. Bij succes: controller reboot automatisch in de nieuwe firmware-versie
9. Controleer firmware-versie na reboot (Status-tab toont versie: ` Idle — Bank A, accepting` en nadat de firmware is geaccepteerd: ` Idle — Bank A, accepted`)

#### Verificatie na de update

Beide pakketten — firmware **én** web-assets — horen in één OTA-cyclus mee te gaan. Vanaf firmware 1.17.20 controleert de controller dit automatisch en meldt een afwijking. Drie onafhankelijke verificatie-bronnen:

| # | Wat | Verwachte waarde na update |
|---|---|---|
| 1 | Webgui footer (firmware-versie) | `vX.Y.Z` van het geüploade `.bin` |
| 2 | `http://<controller-ip>/manifest.json` | `{"asset_version":"X.Y.Z",...}` met dezelfde versie als de footer |
| 3 | Alarms-tegel op Status-tab | Geen rode **MISMATCH**-badge (of weergave **OK** als er ook geen andere alarmen actief zijn) |

Een **MISMATCH**-badge in de Alarms-tegel wijst op een onvolledige OTA-update: de firmware-bank is wel omgezet maar de web-assets niet (of andersom). Eerste actie: harde refresh van de webpagina (`Ctrl+Shift+R`); blijft de melding staan, voer de OTA-procedure dan opnieuw uit met **beide** pakketten in dezelfde sessie. Zie [§6 Status-tab — versie-controle van firmware en web-assets](#status-tab--versie-controle-van-firmware-en-web-assets) voor de achtergrond van dit mechanisme.

#### Dual-bank rollback
- Bij **3 opeenvolgende boot-mislukkingen** gaat de controller automatisch terug naar de vorige firmware-versie
- Symptomen van rollback: onverwachte oude versie na update — controleer ook de Alarms-tegel (na rollback met oude web-assets verschijnt **MISMATCH** zolang nog niet beide pakketten opnieuw geladen zijn)

#### Firmware-update faalt
- Controleer laptop-WiFi stabiel
- Probeer kleinere chunks (browser-instelling)
- Bij blijvende fout: USB-flash via het LOLIN S3-board (procedure: zie `firmware/README.md` of leverancier)

### SD-kaart beheer

De kascontroller schrijft logbestanden naar een SD-kaart. Wanneer er geen kaart aanwezig of niet leesbaar is, wordt logging automatisch teruggevallen op de **NVS-ringbuffer** (~100 events in flash-geheugen). De controller blijft dus altijd loggen — er gaat alleen geen historie naar de SD-kaart als die niet beschikbaar is.

#### Eisen aan de SD-kaart

- **Bestandssysteem**: **FAT32** (verplicht). exFAT en NTFS worden niet ondersteund
- **Capaciteit**: in de praktijk **maximaal 32 GB** (grotere SDXC-kaarten worden door Windows en macOS standaard als exFAT geformatteerd, en moeten met een tool als `guiformat` of `mkfs.fat` handmatig naar FAT32 worden geformatteerd)
- **Klasse / snelheid**: geen minimum vereist; een Class 4 of hoger volstaat ruimschoots voor de geringe schrijfbelasting van logging
- **Type**: standaard SD- of SDHC-kaart (volwaardig formaat, geen microSD met adapter — al werkt dat technisch wel)

#### Wat zijn "mounten" en "unmounten"?

- **Mounten** is het beschikbaar maken van de SD-kaart voor de firmware. Voor het mounten kan er nog geen file (gemaakt of gelezen worden van de SD-kaart. De firmware leest het FAT32 bestandssysteem in, controleert dat de kaart leesbaar is, en opent een logbestand om naar te schrijven. Pas na succesvol mounten kan logging naar SD plaatsvinden.
- **Unmounten** is het netjes afsluiten van de SD-kaart: openstaande bestanden worden gesloten en eventuele buffers naar de kaart geschreven. Pas na unmounten mag je de kaart fysiek verwijderen — anders kunnen log-events verloren gaan of kan het bestandssysteem corrupt raken.

#### Automatisch mounten

De firmware probeert de SD-kaart **automatisch te mounten**:

- **Bij het opstarten** van de kascontroller (direct na boot, tijdens initialisatie van de event-logger taak)
- **Daarna elke 60 seconden** zolang de kaart niet gemount is. Dit betekent dat je een SD-kaart kunt **plaatsen terwijl de controller draait** en dat er binnen één minuut automatisch wordt geprobeerd hem te mounten — een power-cycle is niet nodig

> **Hardware card-detect ontbreekt**: er is geen mechanische schakelaar die detecteert of een kaart is geplaatst. De controller "weet" pas dat een kaart aanwezig is wanneer de mount-poging slaagt.

#### Handmatig mounten via de webinterface

Soms wil je het mounten niet afwachten (bijvoorbeeld na vervanging van de kaart):

1. Tab **Log** → klik **Mount SD**
2. Bij succes: SD-kaart verschijnt in de bestandslijst
3. Bij mislukking: foutmelding in de webinterface; controleer of de kaart correct is geplaatst en op FAT32 staat

#### Handmatig unmounten via de webinterface

**Verplicht voordat je een SD-kaart fysiek verwijdert.** Anders riskeer je verlies van de laatste log-events of een corrupt bestandssysteem.

1. Tab **Log** → klik **Unmount SD**
2. Bij succes: SD-kaart verdwijnt uit de bestandslijst en de auto-mount-poging om de 60 seconden start opnieuw
3. Verwijder de SD-kaart fysiek
4. Plaats eventueel een vervangende kaart — die wordt binnen 60 seconden automatisch gemount, of je triggert mounten handmatig

#### Logbestanden downloaden

1. Tab **Log** → klik op een bestand in de lijst → **Download**

Voor het omzetten van CSV-logbestanden naar leesbare tekst: zie [Bijlage F](#bijlage-f--logbestand-formaat-en-logparser-script).

#### SD-kaart vervangen / formatteren

1. Unmount via webinterface (zie hierboven)
2. Veiligheidshalve: voeding van de kascontroller eruit
3. Vervang SD-kaart, of formatteer hem opnieuw op FAT32 (Windows/macOS bij grotere SDXC-kaarten: gebruik een dedicated FAT32-formattertool)
4. Voeding aan; binnen ~60 sec. wordt de kaart automatisch gemount, of trigger handmatig via Mount SD

#### Vrije ruimte en bestandsrotatie

Om te voorkomen dat de SD-kaart vol raakt:

- **Per logbestand** wordt geroteerd na ~512 KB; daarna start de firmware een nieuw bestand met een nieuwe timestamp-naam
- **Maximaal 10 logbestanden** worden bewaard; bij meer wordt het oudste bestand verwijderd
- **Minimaal 3 bestanden** blijven altijd bewaard (vloer): zelfs bij weinig vrije ruimte wordt nooit onder dit aantal verwijderd
- **Minimaal 2 MB vrije ruimte** vereist; daaronder probeert de firmware oudste bestanden te verwijderen om ruimte vrij te maken
- Zit de controller op de bestands-vloer (3) **én** is er minder dan 2 MB vrij, dan wordt SD-logging tijdelijk **opgeschort** en valt logging terug op alleen NVS. Een `SYSTEM`-event met `value_a = -2` markeert dit moment in het log

> **Praktijk**: bij gewone bedrijfsvoering is een 8 GB-kaart ruim voldoende voor jaren logging. Bij vermoeden van problemen: download alle bestanden, formatteer de kaart opnieuw, plaats hem terug.

### Power-cycle uitvoeren

Zie [boer-handleiding §14](handleiding.md#14-onderhoud--wat-de-boer-zelf-doet). Identieke procedure.

`[FOTO: voedingstekker en stopcontact bij de kascontroller-kast]`
`[FOTO: microprocessorboard met RESET-knop en BOOT-knop duidelijk gemarkeerd]`

#### Power-cycle leidt altijd tot een nieuwe Window Cal.

Bij **élke** power-cycle (of een druk op de RESET-knop) doorloopt de controller automatisch een **CLOSE_ALL kalibratie**: hij stuurt alle drie de motoren tegelijkertijd naar volledig dicht en wacht tot de travel-times verstreken zijn. Pas na afloop van die kalibratie weet de controller met zekerheid in welke positie de ramen zich bevinden. Tijdens de procedure staat `Mode:Window Cal.` op het LCD.

**Waarom is dit belangrijk om te onthouden?**

- De controller heeft **geen positie-feedback** van de motoren — hij volgt de raamposities intern bij op basis van de open/sluit-commando's die hij zelf heeft verstuurd. Tijdens een stroomuitval, een handmatige beweging op de RRK-3, of een motor-alarm gaat die interne aanname verloren of klopt niet meer met de werkelijkheid
- De CLOSE_ALL kalibratie is de **enige manier** om die interne aanname weer in lijn te brengen met de fysieke werkelijkheid
- **Duur van de kalibratie**: ~26 sec. voor M1 en M2 (gelijktijdig), ~176 sec. voor M3 — totaal dus ongeveer **3 minuten** voordat `Mode: AUTO` weer verschijnt
- **Kalibratie wordt alleen overgeslagen** als bij opstart al een motor-alarm actief is op GPIO42 (RRK-3 alarm-uitgang LOW). Mode toont dan direct `Mode: ALARM`. Eerst de RRK-3 resetten, daarna nogmaals power-cyclen om de kalibratie alsnog uit te voeren

**Wanneer is een geforceerde power-cycle nodig?**

| Situatie | Waarom power-cycle? |
|---|---|
| Na handmatige overname op de motorbox | Controller-aanname klopt niet meer met de werkelijke raamposities |
| Na onderhoud aan een raam-motor of bedrading | Idem |
| Na herstel van een stroomuitval | Bij twijfel of de kalibratie correct doorliep |
| Bij wijziging van motor-travel-times in de webinterface | Niet strikt vereist (waardes worden direct toegepast op de volgende beweging), maar een kalibratie verifieert de nieuwe waardes meteen |
| Na firmware-update (OTA) | De controller herstart automatisch en doorloopt CLOSE_ALL — geen extra power-cycle nodig |
| Bij motor-alarm-clearance | Niet vereist — de controller doet automatisch een 60-seconden guard + CLOSE_ALL re-kalibratie zodra het alarm wegvalt (zie [boer-handleiding §12.6](handleiding.md#126-motor-alarm-in-detail)) |

> **Praktische tip**: plan een power-cycle altijd op een rustig moment (bv. avond, niet midden in een hete dag). Tijdens de ~3 minuten kalibratie staan alle ramen dicht en is de klimaatregeling tijdelijk inactief.

---

## 15. Handmatige overname via de motorbox

Voor algemene uitleg en consequenties: zie [boer-handleiding §15](handleiding.md#15-handmatige-overname-via-de-motorbox).

### Voor de beheerder bij onderhoud aan motoren of constructie

**Aanbevolen procedure tijdens onderhoud aan een raam-motor**:

1. Controleer LCD: `Mode: AUTO` actief? Geen lopende beweging?
2. Op de Hotraco RRK-3: zet de schakelaar van het betreffende raam **op handbediening** (of bij voorkeur **alle drie** als je niet zeker weet aan welk raam je werkt)
3. **Indien werkzaamheden aan motor zelf** (bedrading, vervangen): zet bovendien de motor-zekering in de RRK-3 uit, of haal de stekker eruit
4. Voer onderhoud uit
5. Na onderhoud: **eerst zekering / stekker terug**, dan **schakelaars terug op automatisch**
6. **Power-cycle de kascontroller** (zie [§14](#14-onderhoud--wat-de-beheerder-doet)) om CLOSE_ALL kalibratie af te dwingen — alleen zo weet de controller weer met zekerheid waar de ramen staan

> Zie [boer-handleiding §15 — De kascontroller weet niet dat hij is uitgeschakeld](handleiding.md#de-kascontroller-weet-niet-dat-hij-is-uitgeschakeld) voor de gevolgen van handmatige stand zonder power-cycle achteraf.

### Foto-vereisten
`[FOTO: Hotraco RRK-3 motorbox met de drie schakelaars per kanaal duidelijk in beeld; markeer welke positie hoort bij "automatisch" en welke bij "handbediening"]`

---

## 16. Probleemoplossing — Beheerder-niveau

### 16.1 Hardware-problemen

| Probleem | Diagnose | Actie |
|---|---|---|
| LCD blank, heartbeat-LED uit | Voeding weg | Voeding controleren; zekering nameten |
| LCD blank, heartbeat-LED knippert | LCD-bus probleem | Power-cycle; bij blijvende fout LCD-module vervangen |
| Heartbeat-LED steady aan (niet knipperend) | Firmware vastgelopen | Power-cycle of reset; bij herhaling firmware re-flash |

### 16.2 Sensor-problemen

| Probleem | Diagnose | Actie |
|---|---|---|
| `** SENSOR FAULT` constant | Temperatuur/Luchtvochtigheid-sensor reageert niet via Modbus | Zie [§12.3](#123-sensor-fault--diagnose) |
| Wind-meting `--` constant | Wind-sensor reageert niet | Zelfde diagnose, focus op buitenbedrading |
| T-meting onbetrouwbaar (springt) | EMI-bron of bekabeling | Afscherming controleren, bron isoleren |
| RH altijd 100% | Condensvorming in sensor | Wachten / vervangen sensor |
| Wind-meting altijd 0 | Sensor bedekt of intern defect | Visuele inspectie sensor |

### 16.3 Motor-problemen

| Probleem | Diagnose | Actie |
|---|---|---|
| M1/M2/M3 beweegt niet | Schakelaar op RRK-3 in handbediening? | Schakelaar op AUTO zetten |
| Idem | Travel-time te kort? | Webinterface Motors-tab → travel verhogen |
| Idem | Motor-zekering in RRK-3 | Zekering controleren |
| Motor-alarm blijft hangen | Eindschakelaar bereikt niet | Mechanisch nazien, eindschakelaar testen |
| Kalibratie mislukt bij opstart | Een raam blokkeert | Visueel + mechanisch nazien |
| Raam draait door na bereiken eind | Eindschakelaar defect | Vervangen |

### 16.4 WiFi en netwerk

| Probleem | Diagnose | Actie |
|---|---|---|
| AP komt niet op na enable | Hardware-fout in WiFi-module | Power-cycle; bij blijvend probleem ESP32-board vervangen |
| Client-mode kan niet verbinden | SSID / PSK fout, of router buiten bereik | SSID/PSK opnieuw invoeren via AP-modus |
| IP-conflict | Twee apparaten op zelfde IP | DHCP-reservering instellen |
| NTP synchronisatie faalt | Geen internet, of NTP-poort geblokkeerd | Internet-toegang controleren, RTC manueel zetten |
| Geolocatie auto-detect mislukt | ip-api.com niet bereikbaar | Lat/lon handmatig invoeren via System-tab |
| WiFi disconnect frequent | RSSI te laag | Antenne-positie verbeteren, dichter bij router |

### 16.5 Webinterface

| Probleem | Diagnose | Actie |
|---|---|---|
| Pagina laadt niet | IP fout / niet verbonden | Opnieuw IP aflezen op LCD |
| Login werkt niet | Lockout actief | 5 min wachten of via BOOT-knop niveau 1 reset |
| OTA upload faalt | WiFi instabiel | Dichterbij router, opnieuw proberen |
| Sessie eindigt te snel | Session-timeout te kort | System-tab → `cfg-session-timeout` verhogen |
| WebSocket disconnect | Browser-tab verloor focus | Reload pagina |

### 16.6 Klimaatregeling

| Probleem | Diagnose | Actie |
|---|---|---|
| Setpoint wordt niet gehaald | Ventilatie-capaciteit onvoldoende | Setpoint realistischer, of mechanische ventilatie toevoegen |
| Ramen oscilleren (vaak open/dicht) | hysteresis te klein, of dwell te kort | `hyst_t`/`hyst_rh` verhogen, of motor-dwell verhogen |
| Trage reactie | Glijdend gemiddelde te lang | `avg_win_t`/`avg_win_rh` verlagen |
| Dag/nacht klopt niet | Lat/lon of timezone fout | System-tab nazien |
| Ramen openen 's nachts onverwacht | Nacht-setpoints te streng | Nacht-T en RH-grenzen aanpassen |

### 16.7 Logging

| Probleem | Diagnose | Actie |
|---|---|---|
| SD-kaart niet herkend | FAT32? Card defect? | Andere kaart proberen, FAT32-formatteren |
| Log-download faalt | Bestand te groot? | Tab Log → kleinere selectie |
| NVS-ringbuffer vol | Normale toestand (cyclisch) | Geen actie nodig |

### 16.8 Tijd

| Probleem | Diagnose | Actie |
|---|---|---|
| Tijd 1970/2000 | RTC-batterij leeg, geen NTP | CR2032 vervangen, NTP herstellen |
| Tijd loopt fout | Tijdzone niet juist | `tz_str` controleren |
| Dag/nacht-omschakeling op verkeerde tijd | Lat/lon fout | Locatie opnieuw instellen |

### 16.9 PIN's en toegang

| Probleem | Diagnose | Actie |
|---|---|---|
| Beheerder-PIN vergeten | — | BOOT-knop niveau 1 reset (zie [§18](#18-reset-procedure-boot-knop-op-microprocessorboard)) |
| Farmer-PIN vergeten | — | Beheerder reset via webinterface Access-tab |
| Lockout opheffen | Wachttijd resterend | 5 min wachten OF BOOT-knop niveau 1 (reset PIN's én lockout) |

---

## 17. Verklarende woordenlijst

### Algemene termen
| Term | Betekenis |
|---|---|
| **Setpoint** | Gewenste waarde |
| **hysteresis** | Bandbreedte rondom setpoint waarbinnen niet wordt geschakeld |
| **Sliding average / glijdend gemiddelde** | Voortschrijdend gemiddelde over tijdsvenster (1–30 min) |
| **Wind override** | Automatisch sluiten van alle ramen bij te harde wind |
| **AP / Access Point** | Modus waarin de kascontroller zelf een WiFi-netwerk uitzendt |
| **Client mode** | Modus waarin de controller verbindt met bestaand WiFi-netwerk |
| **Conflict-prioriteit** | Welke regelactie voorrang krijgt als T en RH tegelijk om actie vragen |
| **Dwell-tijd** | Minimum tijd dat een raam in een stand blijft voor opnieuw schakelen |
| **CLOSE_ALL kalibratie** | Procedure bij opstart waarbij alle ramen worden gesloten |
| **Power-cycle** | Voeding uit, kort wachten, voeding weer aan |

### Beheerder-/technische termen
| Term | Betekenis |
|---|---|
| **NVS** (Non-Volatile Storage) | Persistent geheugen op ESP32 voor instellingen |
| **OTA** (Over-The-Air) | Firmware-update via netwerk |
| **DHCP** | Automatische IP-toewijzing door router |
| **POSIX TZ-string** | Formaat voor tijdzone-definitie incl. zomer/wintertijd |
| **Hash / SHA-256** | Cryptografische hashfunctie; niet omkeerbaar — gebruikt voor PIN-opslag |
| **Salt** | Random bytes toegevoegd voor het hashen, voorkomt rainbow-table aanvallen |
| **Modbus RTU** | Seriële variant van Modbus over RS485 — zie [Bijlage E](#bijlage-e--modbus-rtu-een-digitale-sensor-interface) voor uitleg |
| **RS485** | Differentiële seriële bus, lange afstand, ruisbestendig — zie [Bijlage E](#bijlage-e--modbus-rtu-een-digitale-sensor-interface) |
| **Afsluitweerstand** | 120 Ω weerstand op uiteinden RS485-bus, dempt reflectie |
| **Eindschakelaar** | Schakelaar in raammechanisme die motor stopt aan einde slag |
| **Hotraco RRK-3** | Externe motor-relais-controller met eigen alarmuitgang |
| **Opto-koppelaar** | Galvanische scheiding via lichtkoppeling — voor signaal-isolatie |
| **DS1307** | Real-time clock chip met I2C-interface, 5–7 jaar batterijbackup |
| **WS2812B** | Adresseerbare RGB-LED met seriële interface |
| **AiP31068L** | LCD-controller chip, I2C-interface |
| **PCA9633** | I2C-bestuurde RGB-PWM driver voor LCD-achtergrondkleur |
| **MAX485** | RS485-transceiver chip |
| **FreeRTOS** | Real-time operating system gebruikt door ESP-IDF |

### Engels-Nederlands LCD-termen
Zie [boer-handleiding §16](handleiding.md#16-verklarende-woordenlijst) — identieke set strings.

---

## 18. Reset-procedure (BOOT-knop op microprocessorboard)

Zie ook [boer-handleiding §17](handleiding.md#17-reset-procedure-boot-knop-op-microprocessorboard) voor de basisprocedure.

### Niveau-overzicht

| Inhouden | LCD-melding | Effect |
|---:|---|---|
| 0–5 sec. | (geen melding) | Geen actie |
| 5–10 sec. | `Reset PIN?` | Niveau 1 — PIN's terug naar fabrieksstandaard |
| 10–15 sec. | `Reset settings?` | Niveau 2 — alle instellingen + PIN's reset |
| 15–20 sec. | `Restarting?` | Niveau 3 — volledige reset + reboot |
| 20+ sec. | `Restart!` / `Restarting...` | Auto-trigger niveau 3 |

### Wanneer welk niveau?

| Situatie | Niveau |
|---|---|
| Boer-PIN vergeten en boer kan beheerder bereiken | — (Beheerder reset via web) |
| Boer-PIN vergeten en geen toegang tot web | Niveau 1 |
| Beheerder-PIN vergeten | Niveau 1 |
| Vermoeden van corrupte instelling | Niveau 2 |
| Volledig terug naar fabriek | Niveau 3 |
| Verhuizing kascontroller | Niveau 3 (alle locatiegebonden config wissen) |

### Na niveau 2 of 3 — checklist herinstallatie

1. ☐ WiFi configureren (AP → client-mode, [§11.1](#111-eerste-keer-WiFi-configureren-na-fabrieksreset-of-nieuwe-installatie))
2. ☐ AP-wachtwoord wijzigen ([§11.3](#113-ap-wachtwoord-wijzigen))
3. ☐ Tijdzone instellen ([§11.7](#117-ntp-en-tijdzone))
4. ☐ Geografische locatie controleren / instellen ([§11.8](#118-geografische-locatie))
5. ☐ Sessie-timeout naar wens instellen ([§11.9](#119-sessie-timeout))
6. ☐ Motor-tijden controleren / instellen ([§10.4](#104-motors-tab-alleen-Beheerder))
7. ☐ Klimaat-setpoints instellen (boer of Beheerder namens boer)
8. ☐ hysteresis, glijdend gemiddelde fijnafstemmen
9. ☐ Wind v_max en eventueel uitsluitings-zone
10. ☐ LED-helderheid dag/nacht ([§5.3](#53-led-indicatoren))
11. ☐ Beheerder-PIN wijzigen van fabrieksstandaard
12. ☐ Farmer-PIN wijzigen / aan boer doorgegeven
13. ☐ Test-cyclus: Mode: WIND afdwingen, Mode: ALARM afdwingen, herstel verifiëren
14. ☐ Logging-functionaliteit testen (SD-kaart mount, download)
15. ☐ OTA-update testen op een rustig moment

> Bewaar deze checklist of een ingevulde versie in de installatie-map per kas.

---

## 19. Bijlagen

### Bijlage A — Contactgegevens leverancier en escalatie

#### Leverancier kascontroller
- Naam: \[invullen]
- Telefoon: \[invullen]
- E-mail: \[invullen]
- Bereikbaarheid: \[invullen]

#### Garantie en hardware-defect
- Garantieperiode: \[invullen]
- Procedure RMA: \[invullen]

#### Firmware-bug rapporteren
- GitHub-issue: \[invullen URL]
- E-mail firmware-team: \[invullen]

#### Boer-contactgegevens
- Naam: \[invullen]
- Telefoon: \[invullen]

### Bijlage B — Standaard configuratiewaarden

Referentietabel — alle default instellingen van de controller:

| Parameter | Default | Eenheid |
|---|---|---|
| t_max_dag | 28 | °C |
| t_max_ngt | 20 | °C |
| rh_max_dag | 75 | % |
| rh_min_dag | 50 | % |
| hyst_t | 5 | °C |
| hyst_rh | 12 | % |
| avg_win_t | 6 | min. |
| avg_win_rh | 10 | min. |
| rh_ctrl_en | 1 | — |
| cr_priority | 0 | — |
| v_max | 6 | m/s |
| wind_prot_en | 1 | — |
| poll_interval_s | 30 | s |
| session_timeout_min | 5 | min. |
| ap_timeout | 30 | min. |
| m1_travel_s, m2_travel_s | 21 | s |
| m3_travel_s | 171 | s |
| m1_dwell_open_s, m2_dwell_open_s | 300 | s |
| m3_dwell_open_s | 1500 | s |
| m1_dwell_close_s, m2_dwell_close_s | 300 | s |
| m3_dwell_close_s | 600 | s |
| led_dag_brt | 200 | (0–255) |
| led_nite_brt | 20 | (0–255) |
| lat_deg | 52 | ° |
| lon_deg | 5 | ° |
| tz_str | `CET-1CEST,M3.5.0,M10.5.0/3` | POSIX |
| ap_psk | `0123456789` | string |

### Bijlage C — NVS-namespaces (permanent geheugen) overzicht

| Namespace | Inhoud | Wie kan schrijven? |
|---|---|---|
| `climate` | Temperatuur/Luchtvochtigheid setpoints, hysteresis, gemiddelden, vochtregeling, conflict-prioriteit | Farmer (beperkt) + Beheerder |
| `wind` | v_max, uitsluitings-zone, wind_prot_en | Farmer (alleen wind_prot_en) + Beheerder |
| `motor` | M1/M2/M3 travel + dwell open/close | Beheerder |
| `system` | Poll-interval, sessie-timeout, AP-timeout, locatie, timezone, LED-helderheid | Beheerder |
| `WiFi` | SSID, PSK, AP-PSK, AP-enable | Beheerder |
| `mqtt` | (Optioneel) MQTT broker config | Beheerder |
| `access` | PIN-hashes, lockout-counters | Beheerder (via `/api/pin`) |

### Bijlage D — GPIO-pinout (samenvatting)

Volledige GPIO-mapping in `firmware/config/pin_config.h`. Belangrijkste:

| GPIO | Functie |
|---|---|
| 38 | RGB-LED (WS2812B) |
| 41 | Heartbeat-LED (amber) |
| 42 | Motor-alarm input (active-low, opto-coupler) |
| 0 | BOOT-knop (factory reset) |
| (I2C) | LCD AiP31068L + DS1307 RTC + PCA9633 |
| (UART1) | Modbus via MAX485 |
| (3× OPEN + 3× CLOSE) | Relais-uitgangen naar RRK-3 |

Voor exacte pinnen: `firmware/config/pin_config.h`.

### Bijlage E — Modbus RTU: een digitale sensor-interface

De sensoren in dit systeem zijn **digitaal**, niet analoog. In plaats van een spanning of stroom (zoals 4–20 mA, 0–10 V of een ohmwaarde van een NTC) leveren ze hun meetwaarden direct als getal aan de kascontroller, via een digitaal protocol genaamd **Modbus RTU**.

**Hoe werkt het op hoofdlijnen?**

- **Master-slave architectuur**: de kascontroller is de **master** en stelt vragen. Elke sensor is een **slave** met een uniek bus-adres (bijv. adres `1` voor de T/RH-sensor en `2` voor de wind-sensor). Slaves spreken alleen wanneer ze worden aangesproken
- **Request/response**: de master stuurt periodiek (elke poll-cyclus) een leesverzoek naar een specifiek slave-adres met een specifiek register-nummer ("geef mij de temperatuur"); de slave antwoordt met de waarde
- **Digitale waarden**: alle metingen zijn al binnen de sensor gedigitaliseerd en gekalibreerd. De kascontroller hoeft niets om te rekenen vanuit een spanning of stroom — hij krijgt direct een getal zoals `230` voor 23,0 °C, of `652` voor 65,2 % RH
- **Foutdetectie**: elk Modbus-bericht bevat een CRC-checksum. Bij verstoring detecteert de master dit, en de meting wordt als ongeldig beschouwd. Twee mislukte polls op rij triggeren een sensor-fault (zie [§12.3](#123-sensor-fault--diagnose))

**Waarom Modbus / digitaal in plaats van analoog?**

- **Meerdere sensoren op één paar draden**: omdat elke slave een eigen adres heeft, kun je tientallen sensoren op één twee-aderige bus aansluiten. Bij analoge sensoren heeft elk meetkanaal een eigen kabel naar de controller nodig
- **Geen verstoring door kabellengte**: een analoog signaal verzwakt en vervormt over lange kabellengtes; een digitaal signaal niet. Voor lange kabels in een kas is dit een groot voordeel
- **Hogere ruisbestendigheid**: combinatie van RS485 (differentieel signaal) en CRC-checksum maakt het systeem ongevoelig voor elektrische storingen van bijvoorbeeld dimmers of motoren
- **Geen kalibratie per kabellengte**: bij analoge sensoren moet je doorgaans per installatie compenseren voor kabelverlies. Bij digitaal niet
- **Sensor levert direct meetwaarde**: temperatuur in 0,1 °C of vochtigheid in 0,1 % RH, in plaats van een ruwe ADC-waarde

**RS485 — de fysieke laag**

Modbus RTU wordt verstuurd over **RS485**, een differentieel seriële bus:
- Twee draden (`A` en `B`, vaak in een twisted-pair) waarop hetzelfde signaal in tegenfase staat — eventuele storingen die op beide draden landen worden door de ontvanger weggefilterd
- **Eén bus voor alle sensoren** — de FG6485A en SenseCAP S200 zitten parallel op dezelfde A/B-aders
- **Master-master botsingen niet mogelijk** omdat alleen de kascontroller aanvalt en de slaves alleen op verzoek antwoorden — bus-arbitrage is dus simpel

> **Modbus-bus in deze installatie**: Alle sensoren zijn digitaal en delen één RS485-bus naar de kascontroller (UART1, MAX485 transceiver, DE/RE direction control). Afsluitweerstand 120 Ω op het verste eind van de bus. Bekabeling: twisted-pair voor A/B + aparte aarding/afscherming en 24 V voor de sensorvoeding.

### Bijlage F — Logbestand-formaat en `logparser` script

De kascontroller schrijft gebeurtenissen naar twee bronnen:

- **NVS-ringbuffer** — laatste ~100 events, altijd in het flash-geheugen aanwezig
- **SD-kaart** — CSV-bestanden per opstart-sessie, bestandsnaam `YYYYMMDDHHMMSS.csv` (lokale tijd)

Beide bronnen leveren CSV-bestanden in hetzelfde formaat. Download via webinterface tab **Log** (Beheerder-rol vereist).

#### CSV-formaat

Elke regel is één event met de volgende kolommen:

| Kolom | Beschrijving |
|---|---|
| `timestamp` | ISO 8601 UTC (`YYYY-MM-DDTHH:MM:SS`) |
| `event_type` | Een van: `SENSOR`, `RELAY`, `MODE`, `SETPT`, `SESSION`, `ALARM`, `SYSTEM` |
| `initiator` | Wie veroorzaakte het event: `SYS`, `FARMER`, `ADMIN`, `MQTT`, `WEB` |
| `ch` | Motor-kanaal (1=M1, 2=M2, 3=M3) of 0 |
| `param` | Parameter-ID (alleen bij `SETPT`-events) of 0 |
| `value_a` | Eerste waarde, betekenis afhankelijk van event-type |
| `value_b` | Tweede waarde, betekenis afhankelijk van event-type |

#### Event-types op hoofdlijnen

| Type | Wanneer gepost | Belangrijke velden |
|---|---|---|
| `SENSOR` | Iedere sensor-poll-cyclus | `value_a` = T (°C), `value_b` = RH (%) |
| `RELAY` | Bij motor-toestandsovergang | `ch` = motor, `value_a` = nieuwe state (0–6) |
| `MODE` | Bij wijziging van ventilatie-stap | `value_a` = stap (0–3) |
| `SETPT` | Bij wijziging van een setpoint | `param` = parameter-ID, `value_a` = oud, `value_b` = nieuw |
| `SESSION` | Bij login/logout | `value_a` = niveau (0/1/2) |
| `ALARM` | Wind-override + motor-alarm | Specifieke codering per alarm-type |
| `SYSTEM` | Systeem-events (boot, queue overflow, SD-fout) | Verschilt per sub-event |

#### `logparser.py` — Python-script in de repository

In de [repository](https://github.com/pe1mew/greenhouse-Controller/tree/main/log) staat een Python-script waarmee je een ruw CSV-logbestand omzet naar een geformatteerde tekst-versie met begrijpelijke beschrijvingen per event:

| Item | Locatie in de git-repo |
|---|---|
| Het script | `log/logparser.py` |
| Volledige handleiding bij het script | `log/logparser.md` |

**Vereisten**: Python 3.10 of hoger; alleen de standaard-library, geen `pip install` nodig.

**Korte gebruiksaanwijzing**:

```bash
# Eén bestand parsen
python logparser.py nvs_log.csv
# → output: parsed_nvs_log.txt

# Alle SD-bestanden in de huidige map parsen (chronologisch geordend, samengevoegd)
python logparser.py *
# → output: parsed_YYYYMMDD.txt
```

**Voorbeeld-output**:

```
Timestamp (UTC)      Type        Initiator       Description
--------------------------------------------------------------------
2025-06-07 14:30:22  [SENSOR ]   System          T=23 °C   RH=65 %
2025-06-07 14:30:52  [RELAY  ]   System          M1: → MOVING_OPEN
2025-06-07 14:31:10  [MODE   ]   System          Vent step → 1 (M1 open)
2025-06-07 14:35:00  [SETPT  ]   Admin (LCD)     t_max_day: 25 °C → 27 °C
2025-06-07 14:45:00  [ALARM  ]   System          WIND OVERRIDE: SET — speed 8.5 m/s ≥ v_max 5.0 m/s
2025-06-07 14:50:00  [ALARM  ]   System          WIND OVERRIDE: CLEARED — speed 3.2 m/s, direction 180°
```

#### Volledige documentatie

De **complete uitleg** — met daarin alle velden, alle event-types, alle parameter-ID's voor `SETPT`-events, alle channel-states voor `RELAY`-events, de codering van `ALARM`-events en bekende beperkingen — staat in **`log/logparser.md`** in de git-repository. Houd dit document naast deze handleiding bij het analyseren van logbestanden.

---

## 20. Versie en wijzigingshistorie

| Versie | Datum | Wijziging |
|---|---|---|
| 1.0 | \[invullen] | Eerste uitgave — gebaseerd op firmware 1.16.34 |
| 1.1 | 2026-05-09 | Bijgewerkt voor firmware 1.16.35–1.16.38: conflict-prioriteit (`cr_priority`) toegankelijk via Climate-menu (LCD optie 3) en als keuzelijst in webinterface tab Climate (Boer-bewerkbaar); nieuwe `#=Set` snelweg op de T/RH- en Wind-statusschermen die het Climate- of Wind-menu opent met een Boer-PIN-prompt; LCD-render bug `LFS_BUF_SIZE` opgehoogd naar 64 KiB om afgeknotte HTML te voorkomen; Wind-statusscherm rij 2 zonder haakjes om kompasletter (`Dir:180° S #=Set`) |
| 1.2 | 2026-05-10 | Bijgewerkt voor firmware 1.16.39: zichtbare `#=Set`/`#=AP`-hints verwijderd van alle vier de statusschermen (T/RH, Wind, WiFi, Datum/tijd) — `#`-snelweg blijft werken naar het bijhorende menu, alleen de hint op rij 2 is weg; Wind-statusscherm rij 2 toont kompasletter weer tussen haakjes (`Dir:180 ° (S )`) zoals vóór 1.16.37 |
| 1.3 | 2026-05-10 | Bijgewerkt voor firmware 1.17.0–1.17.1: nieuwe Beheerder-tab **Web** voor status-rapportage naar een extern PHP-eindpunt (URL, gedeelde token, interval 60–300 s, zes tegel-zichtbaarheidsvinkjes, dagelijkse log-upload tijd en upload-op-rotatie); status-rapportage standaard uit en volledig instelbaar zonder de LCD aan te raken; HTTPS-eindpunten ondersteund (geen certificaatcontrole); `Uptime`-regel toegevoegd aan de Klok-tegel van de Status-tab zodat onverwachte reboots zichtbaar zijn (§6 Status-tab — Klok-tegel, §11.10). |
| 1.4 | 2026-05-11 | Bijgewerkt voor firmware 1.17.2–1.17.8a: extern dashboard toont nu **lokale tijd** voor zonsopkomst/zonsondergang en `time_iso` (UTC→lokaal-conversie in `dm_status_snapshot`, DST automatisch); status-JSON-veldnamen aangelijnd op het bestaande publieke dashboard (`temp_c`/`speed_ms`/`direction_deg`/`mode={current,flags[]}` etc.); HTTPS-uitgaande verbindingen krijgen ruimere stack (12 KB) en mbedTLS-handshake werkt nu betrouwbaar; T11 status-JSON-buffer vergroot 1024 → 2048 bytes; `/api/web` POST nu synchroon (geen race meer bij Apply); URL-validatie eist `api.php`-suffix; webgui-Apply velden worden niet meer overschreven door auto-refresh; cosmetische verbeteringen (klok-waarde bold, URL-veld donker thema, datum-spinner-knoppen passen); nieuwe diagnostische **OTA diagnostic (temp)**-tegel op Status-tab toont Firmware-vs-Assets-versie + MISMATCH-badge (§6 Status-tab — OTA diagnostic (temp)); web-assets dragen nu een eigen `asset_version` via `manifest.json` (in de ZIP gebakken door `bin/build_release.ps1`); `GET /manifest.json` en `<!-- web-assets X.Y.Z -->` HTML-comment voor onafhankelijke verificatie. |
| 1.5 | 2026-05-11 | Bijgewerkt voor firmware 1.17.9–1.17.20: hoofd-bugfix in `drivers/littleFS/src/littlefs_storage.cpp` — beide LittleFS-partities deelden VFS-mountpoint `/lfs`, waardoor T13 tijdens een gekoppelde OTA wel firmware naar de inactieve bank schreef maar de assets nooit op de bijhorende LFS-partitie terechtkwamen; iedere partitie heeft nu een eigen mountpoint (`/lfsa` en `/lfsb`), waarmee de OTA-cross-bank fout (zichtbaar als oude assets na een succesvolle firmware-update) verholpen is. De tijdelijke **OTA diagnostic (temp)**-tegel uit 1.17.4–1.17.9a is verwijderd; de versie-controle blijft behouden en is geïntegreerd in de **Alarms**-tegel als **MISMATCH**-badge (§6 Status-tab — versie-controle van firmware en web-assets). De diagnostische verificatie-bronnen blijven beschikbaar voor onafhankelijke controle: `GET /manifest.json`, View Source-stempel `<!-- web-assets X.Y.Z -->`, en `?v=<versie>` cache-busters op `app.js` / `style.css`. OTA-procedure in §14 uitgebreid met expliciete **Verificatie na de update**-stap. |

---

*Einde van de beheerder-handleiding.*
