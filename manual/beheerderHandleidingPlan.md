# Plan: Beheerder-handleiding voor de kascontroller

## Context

De kascontroller wordt door de boer dagelijks bediend, maar de **technisch beheerder** is verantwoordelijk voor de installatie, configuratie, onderhoud en het oplossen van storingen. De boer-handleiding (`manual/handleiding.md`) is gericht op dagelijks gebruik; de beheerder heeft een aparte handleiding nodig die alle admin-functies, de complete configuratie van het systeem, en de procedures bij storing en onderhoud beschrijft.

Het beoogde resultaat is **één compleet markdown-document** (`manual/beheerderHandleiding.md`) waarmee de beheerder:
- Het systeem in eerste gebruik kan installeren en configureren (wifi, locatie, motor-tijden, sensoren).
- Alle admin-functies op LCD én webinterface kan vinden en toepassen.
- Klimaat-, wind- en motor-parameters fijn kan afstemmen (hysterese, gemiddelden, dwell-tijden).
- Bij storing zelfstandig kan diagnosticeren (alarmen, sensor-fouten, motor-alarm, network-issues).
- PIN's, firmware-updates, log-bestanden en de SD-kaart kan beheren.
- Weet wanneer hij de leverancier moet bellen en welke informatie hij dan paraat heeft.

## Beslissingen (afgestemd met gebruiker)

- **Structuur**: één bestand `manual/beheerderHandleiding.md` (geen gesplitste hoofdstukken), **dezelfde hoofdstuk-volgorde** als de boer-handleiding zodat beide naast elkaar te leggen zijn.
- **Scope**: volledige beheerder-handleiding. Boer wordt kort genoemd waar de rolverdeling relevant is. Verwijzingen naar de boer-handleiding waar mogelijk om verdubbeling te beperken.
- **LCD-termen**: Engelse termen letterlijk weergeven (zoals ze op het scherm staan) met Nederlandse uitleg ernaast — gelijk aan de boer-handleiding.
- **Webinterface-velden**: weergegeven met de exacte field-id of label uit `firmware/data/index.html`, plus Nederlandse uitleg en (waar van toepassing) standaardwaarde + bereik uit `firmware/config/cfg_defaults.h` en `cfg_limits.h`.
- **Afbeeldingen**: tekstplaceholders zoals `[FOTO: ...]` en `[SCHERMAFBEELDING: ...]`; verwijzen naar bestaande PNG'en in `design/` waar relevant.
- **Authoritatieve bron voor PIN-lengtes**: `firmware/src/auth/pin_auth.h` (4 cijfers Farmer / 8 cijfers Admin).
- **Geen default Admin-PIN noemen** in het document; voor de boer-handleiding al beslist en consistent doortrekken.
- **Diepere technische details** (Modbus-adressen, GPIO-mapping, FreeRTOS-taken) horen in `design/technicalSoftwareDesignSpecification.md`, niet in deze beheerder-handleiding. Verwijzen waar nodig.

## Te maken bestanden

| Pad | Doel |
|---|---|
| `manual/beheerderHandleiding.md` | Beheerder-handleiding, één bestand, ~2500–3500 regels markdown |
| `manual/imagesBeheerder/` | Nieuwe map; aanvullende beheerder-foto's en schermafbeeldingen worden hier toegevoegd |

## Inhoud van `manual/beheerderHandleiding.md`

Eén document met onderstaande hoofdstukken — dezelfde nummering als de boer-handleiding zodat hoofdstukken makkelijk vergeleken kunnen worden. Stijl: **kort, concreet, met genummerde stappen** waar mogelijk; admin-handleiding mag wel iets technischer zijn dan de boer-handleiding.

### Voorblad
- Titel: **Handleiding Kascontroller — voor de beheerder**
- Versie + datum (placeholder)
- Korte zin: "voor wie is dit document"
- Veiligheidswaarschuwing: 230V binnen kast; werk alleen aan bedrading met spanning eraf; kennis van elektrische installatie vereist
- Verwijzing naar de boer-handleiding voor dagelijks gebruik

### Inhoudsopgave
- Genummerde TOC met links naar elke sectie

### 1. Over deze handleiding
- Doelpubliek: technisch beheerder / installateur
- Vereiste kennis: elektrische installaties, basis netwerkkennis, eenvoudige Modbus-concepten
- Wat staat er **niet** in (firmware-bron, hardware-design → `design/` map; teelt-advies → leverancier planten)
- Bij escalatie: contactgegevens leverancier (placeholder)

### 2. Wat doet de kascontroller?
- **Korte herhaling van de boer-versie**, plus extra:
  - Ventilatieregeling op basis van setpoints, hysterese en glijdend gemiddelde
  - Drie-traps ventilatie-strategie (M1 → M1+M2 → M1+M2+M3)
  - Veiligheids-mechanismen: wind-override, motor-alarm, sensor-fault detectie
  - Geen positie-feedback van motoren — controller werkt op tijd-gestuurde commando's via de Hotraco RRK-3
  - Persistente opslag (NVS) van alle setpoints en configuratie

### 3. De kas en het systeem
- Globaal blokdiagram (zelfde als boer-versie)
- **Aanvullend voor beheerder**:
  - Specificaties van de drie ramen (oppervlak, motor-tijden default 21/21/171 sec.)
  - Sensor-typen + Modbus-adressen + bekabeling RS485
    - FG6485A — T/RH binnen
    - SenseCAP S200 — wind buiten
  - Hotraco RRK-3 — relais-uitgangen + alarm-uitgang via opto-koppelaar (GPIO42)
  - Kascontroller hardware: WEMOS LOLIN S3 (ESP32-S3), 16×2 LCD (AiP31068L, I2C 0x3E), 4×4 keypad, RGB-LED (WS2812B GPIO38), heartbeat-LED (GPIO41), MAX485 transceiver, opto-isolated alarm input
  - Voedingsspanning, stroomverbruik, IP-rating placeholder
  - `[FOTO: open kast met bedrading en bordjes met etiketten]`

### 4. Hoe regelt de controller het klimaat?
- **Korte herhaling**, plus diepere uitleg van regel-mechanisme:
  - **Setpoints, hysterese, glijdend gemiddelde** (admin tunbaar):
    - `hyst_t` (default 5 °C, 2–15)
    - `hyst_rh` (default 12 %, 2–20)
    - `avg_win_t` (default 6 min, 1–30)
    - `avg_win_rh` (default 10 min, 1–30)
  - **Dwell-tijden** per motor — minimum tijd dat een raam in een stand blijft voordat de controller weer mag schakelen. Voorkomt oscillatie, vooral bij M3 (default 1500 s open / 600 s dicht — voor kas 2 gekalibreerd)
  - **Conflict-prioriteit** (`cr_priority` 0/1/2)
  - **Vochtregeling aan/uit** (`rh_ctrl_en` toggle) — wanneer toegepast
  - **Dag/nacht-omschakeling** — berekend uit lat/lon + datum + timezone (wordt bepaald op basis van internet aansluiting) 
  - Welke parameters reboot-vereisen vs. live-actief

### 5. De controller (fysiek)
- `[FOTO: vooraanzicht kast met LCD, toetsenbord, LEDs]`
- **De kast**: schroefverbinding/kabelinvoer, IP-rating, montage

#### 5.1 LCD-display
- Zelfde inhoud als boer-versie, plus:
  - LCD1602RGB-achtergrondkleur — wit/blauw/rood en wat ze betekenen (vanuit `update_backlight_status()`)
  - I2C-bus delen met RTC; mutex MX1
- Verwijzing naar boer-handleiding voor scherm-voorbeelden

#### 5.2 Toetsenbord
- Zelfde inhoud als boer-versie

#### 5.3 LED-indicatoren
- Plus configureerbare helderheid:
  - `led_day_brt` (0–255, default 200)
  - `led_nite_brt` (0–255, default 20)
  - `led_nite_from`, `led_nite_to` (uren voor nacht-modus)
  - Hoe deze waarden in te stellen (alleen via webinterface, System tab)
- Heartbeat-LED en wat te doen als hij niet knippert

### 6. De webinterface (via wifi)
- Volledige overzicht van alle tabs, inclusief admin-only:
  - **Status** — iedereen
  - **Climate** — Farmer + Admin (admin ziet ook hyst, avg_win)
  - **Wind** — Farmer + Admin (admin ziet ook v_max, dir_excl)
  - **Motors** — alleen Admin (M1/M2/M3 travel + dwell open/close)
  - **System** — alleen Admin (sessie, AP, wifi-client, NTP-tijdzone, locatie, OTA)
  - **Log** — alleen Admin (SD-kaart, log-download)
  - **Access** — alleen Admin (PIN-management Farmer + Admin)
- Endpoint-overzicht (`/api/status`, `/api/config`, `/api/wifi`, `/api/pin`, `/api/ota/firmware`, `/api/ota/assets`, `/api/sd/*`, `/api/log/*`) en welke role vereist is per endpoint
- WebSocket `/ws` voor live status

### 7. De twee gebruikersrollen
- **Farmer**: 4-cijferige PIN, fabrieksstandaard `1234`
- **Admin**: 8-cijferige PIN, **geen default in handleiding genoemd** — de beheerder krijgt deze bij oplevering, of stelt hem in via de fysieke reset-procedure
- **Lockout**: 5× foute PIN → 5 minuten geblokkeerd per rol
- **PIN's worden gehasht opgeslagen** (SHA-256 + salt) — niet terug te lezen
- **PIN-management** voor de Admin:
  - Eigen PIN wijzigen (Web → Access tab)
  - Farmer-PIN wijzigen (Web → Access tab) — bijvoorbeeld als boer zijn PIN vergeten is
  - Beide PIN's terugzetten naar fabrieksstandaard via fysieke reset (BOOT-knop, niveau 1)
- Sessietime-out (`session_timeout_min`, default 5 min, range 1–1440 min) — door Admin instelbaar

### 8. Gebruik zonder inloggen — informatiemenu
- Zelfde inhoud als boer-versie (geen admin-specifieke uitbreidingen — alle admin-functies vereisen login)

### 9. Inloggen als beheerder
- Login op de LCD: hoofdmenu → `3:Access` → `2:Admin` → 8-cijferige PIN → `#`
- Login in webinterface: tab Access → knop **Admin** → 8-cijferige PIN → Login
- Snelweg via #-toets op WiFi-status of Time-status: vraagt ook direct om Admin-PIN
- Uitloggen: zelfde routes als boer
- Verschillen met boer-login: 8 cijfers, andere lockout-teller

### 10. Klimaat instellen
- Verwijzing naar boer-handleiding voor de basis-route
- **Aanvullend voor de beheerder**:
  - **Hysterese instellen** (`hyst_t`, `hyst_rh`) — webinterface only, Climate tab onder admin-only sectie
  - **Glijdend-gemiddelde-vensters** (`avg_win_t`, `avg_win_rh`) — webinterface only
  - **Vochtregeling toggle** (`rh_ctrl_en`) — door wie wanneer in/uit te zetten
  - **Conflict-prioriteit** opnieuw uitgelegd voor admin
  - Tabel met alle climate-parameters: naam, NVS-key, default, range, scope (Farmer/Admin), reboot-vereist
  - Adviezen voor instelling per teelttype (placeholder)

### 11. Wifi en webinterface — installatie en beheer
- **Eerste keer wifi configureren** (na fabrieksreset of nieuwe installatie):
  1. Inschakelen Access Point via fysieke locatie (LCD: `#` op WiFi-status, voer Admin-PIN in)
  2. Verbind je laptop/telefoon met SSID `Greenhouse-XXXX`
  3. Open browser op `http://192.168.4.1`
  4. Login als Admin
  5. Tab System → WiFi client → vul SSID en wachtwoord van het kas-/thuisnetwerk in
  6. Klik Connect; wacht ~30 sec op verbinding
  7. Lees nieuw IP-adres af van LCD WiFi-scherm
- **AP-wachtwoord wijzigen** (`cfg-ap-psk`, default `0123456789`) — sterk aangeraden bij installatie
- **AP automatisch uitschakelen** (`ap_timeout`, default 30 min, 0 = nooit) — voorkomt dat een open AP onbedoeld bereikbaar blijft
- **WiFi client SSID/PSK** wijzigen
- **mDNS / hostname** — niet geïmplementeerd; gebruik IP-adres
- **Statisch IP** — niet mogelijk via webinterface; via DHCP-reservatie op de router
- **NTP / tijdzone** (`tz_str`, default Nederlandse tijdzone) — POSIX-formaat, voorbeeld `CET-1CEST,M3.5.0,M10.5.0/3`
- **Geografische locatie** (`lat_deg/frac`, `lon_deg/frac`) — voor zonsopkomst/zonsondergang en dag/nacht-omschakeling, wordt bepaald op basis van internet aansluiting (maak opmerking over het gebruik van 4G wat een verkeerde locatie kan pleveren) 
- **Sessietime-out** (`session_timeout_min`)

### 12. Alarmen en bedrijfsmodi — diagnose en herstel
- Verwijzing naar boer-handleiding voor algemene uitleg van de modes
- **Aanvullend voor de beheerder**:
  - **Diepere uitleg windbeveiliging**: rol van `v_max`, `dir_excl_low/high`, `avg_win_w`. Aanpassen aan kas-locatie (heersende windrichting)
  - **Diepere uitleg motor-alarm**: opto-koppelaar GPIO42, 75 ms debounce, 60 sec guard, automatische CLOSE_ALL re-kalibratie
  - **Sensor-fault detectie**: 2 mislukte Modbus-uitlezingen → fault. Wat te controleren bij `** SENSOR FAULT`:
    - Stroom op de sensor?
    - Bedrading RS485 (A/B niet verwisseld?)
    - Modbus-adres correct?
    - Termineerweerstanden 120 Ω op de bus?
  - **Wind-sensor fault**: zelfde diagnose plus: kabelafscherming, EMI-bron, sensor-druipschaduw
  - **Motor-alarm reset-procedure** op de Hotraco RRK-3 (geen kascontroller-handeling) — verwijzing naar Hotraco-handleiding
  - **Logbestand-analyse** voor fout-historie

### 13. Inschakelen na stroomuitval
- Zelfde basis als boer-versie
- **Aanvullend**:
  - Wat te doen als kalibratie niet correct verloopt (raam blijft hangen op MOV>): controleer eindschakelaar in RRK-3
  - Wat te doen als motor-alarm direct bij opstart actief is: hoe RRK-3 te resetten, dan kalibratie opnieuw afdwingen via power-cycle
  - RTC-batterij verlies — symptoom en vervangingsprocedure
  - Bij langdurige stroomuitval: NVS-data blijft bewaard; geen actie nodig

### 14. Onderhoud — wat de beheerder doet
- **Periodiek (jaarlijks)**:
  - Sensor-kalibratie controleren (vergelijk met onafhankelijke referentie-meter)
  - Bedrading inspecteren op corrosie of beschadiging
  - Eindschakelaars in RRK-3 controleren
  - Stof / vuil uit kast verwijderen (bij voorkeur perslucht, geen vochtige doek)
- **RTC-batterij vervangen** (CR2032) — procedure met foto-placeholder
- **Firmware-update / OTA**:
  - Via webinterface, System tab, Firmware upload (.bin)
  - Web-assets update (.zip) voor wijzigingen aan webinterface
  - Dual-bank rollback bij 3 boot-mislukkingen
  - Wat te doen bij firmware-update faal: fysieke flash via USB
- **SD-kaart beheer**:
  - Mounten/unmounten via webinterface (Log tab)
  - Logbestanden downloaden
  - Kaart vervangen / reformatteren
- **Power-cycle uitvoeren** — zelfde als boer-versie

### 15. Handmatige overname via de motorbox
- Zelfde basis als boer-versie
- **Aanvullend**:
  - Voor de beheerder bij onderhoud aan motoren of constructie
  - Belangrijk: kascontroller weet niet dat hij is uitgeschakeld; consequenties (verwijzing naar boer-handleiding)
  - Aanbevolen procedure tijdens onderhoud: eerst LCD `Mode: AUTO` checken, dan motorbox handmatig zetten, daarna power van motoren afhalen indien aan motor zelf gewerkt wordt
  - Na onderhoud: power-cycle van kascontroller verplicht voor kalibratie

### 16. Probleemoplossing — admin-niveau
Veel uitgebreidere FAQ dan de boer-versie. Per probleemcategorie:
- **Hardware**: LCD blank, heartbeat-LED, RGB-LED-fouten, ESP32-board diagnose
- **Sensoren**: T/RH-sensor uitval, wind-sensor uitval, Modbus-bus verstoring, sensor-waarden onbetrouwbaar
- **Motoren**: M1/M2/M3 bewegen niet, eindschakelaar-fout, motor-alarm blijft hangen, kalibratie mislukt
- **Wifi/netwerk**: AP komt niet op, client-mode kan niet verbinden, IP-conflict, NTP synchronisatie faalt, geolocatie-detectie mislukt
- **Webinterface**: pagina laadt niet, login werkt niet, OTA-upload faalt, sessie eindigt te snel, WebSocket disconnect
- **Klimaatregeling**: setpoint wordt niet gehaald, ramen schakelen te vaak (oscillatie), dag/nacht klopt niet
- **Logging**: SD-kaart wordt niet herkend, log-download faalt, NVS-ringbuffer vol
- **Tijd**: tijd loopt verkeerd, RTC-batterij leeg, dag/nacht-omschakeling klopt niet
- **PIN's**: Admin-PIN vergeten (recovery via BOOT-knop), Farmer-PIN reset, lockout opheffen

### 17. Verklarende woordenlijst
- Zelfde basis als boer-versie
- **Uitgebreid met admin-termen**:
  - **NVS** (Non-Volatile Storage) — persistent geheugen op ESP32
  - **OTA** (Over-The-Air) — firmware-update via netwerk
  - **MQTT** — light-weight publish/subscribe-protocol
  - **DHCP** — automatische IP-toewijzing
  - **POSIX TZ-string** — formaat voor tijdzone
  - **Hash / SHA-256** — cryptografische hashfunctie voor PIN-opslag
  - **Sliding average / glijdend gemiddelde**
  - **Hotraco RRK-3** — externe motor-relais-controller met eigen alarmuitgang
  - **Modbus-adres** — uniek slave-adres op RS485 bus
  - **Termineerweerstand** — 120 Ω weerstand om reflectie op RS485 te dempen
  - **Eindschakelaar** — schakelaar in raammechanisme die de motor stopt aan einde slag

### 18. Reset-procedure (BOOT-knop op microprocessorboard)
- Volledige procedure herhaald uit boer-versie (de beheerder moet hem ook kennen)
- **Aanvullend**:
  - Wanneer welk niveau te gebruiken (admin-perspectief)
  - Wat na niveau 2/3 opnieuw geconfigureerd moet worden (volledige checklist)
  - Hoe een fabriekstest uit te voeren

### 19. Bijlage A — contactgegevens leverancier en escalatie
- Naam leverancier kascontroller \[invullen]
- Telefoon / e-mail \[invullen]
- Bij hardware-defect: garantie-procedure
- Bij firmware-bug: GitHub-issue / e-mail \[invullen]
- **Bijlage B — Standaard configuratiewaarden** (referentietabel met alle defaults voor terugzetten na fabrieksreset)
- **Bijlage C — NVS-keys overzicht** (alle namespaces en keys, met scope per rol)
- **Bijlage D — API-endpoints overzicht** (`/api/*` endpoints + role-vereisten)
- **Bijlage E — GPIO-pinout** (verwijzing naar `firmware/config/pin_config.h`)

### 20. Versie en wijzigingshistorie
- Versie 1.0 — eerste uitgave (datum)
- Gebaseerd op firmware 1.16.34

## Bronnen die in het document gebruikt/gerefereerd worden
- `manual/handleiding.md` — boer-handleiding (referentie voor gedeeld materiaal)
- `firmware/data/index.html`, `firmware/data/app.js` — webinterface tabs en velden
- `firmware/src/web_server/web_server.cpp` — API-endpoints en role-checks
- `firmware/src/ui_display/ui_display.cpp` — LCD-FSM, admin-paden
- `firmware/src/relay_controller/relay_controller.cpp` — motor-tijden, alarm-handling, kalibratie
- `firmware/src/auth/pin_auth.h` — authoritatieve PIN-lengtes
- `firmware/src/data_manager/data_manager.h` — NVS-namespaces
- `firmware/config/cfg_defaults.h` — alle standaardwaarden
- `firmware/config/cfg_limits.h` — alle min/max-bereiken
- `firmware/config/pin_config.h` — GPIO-mapping
- `design/technicalSoftwareDesignSpecification.md` — diepere technische details (waarvoor in handleiding geen plaats is)
- `design/functionalRequirementsSpecification.md` — eisen-spec
- `documentation/Sensors/sensors.md` — sensor-typen + Modbus-adressen

## Verificatie
Omdat dit een tekst-document is en geen code, gebeurt verificatie handmatig:
1. **Inhoudelijke review door gebruiker** (Remko) — check of alle uitleg klopt met huidige firmware (1.16.34) en hardware
2. **Termen-check**: vergelijk de Engelse termen / field-id's in het document met wat daadwerkelijk in firmware en webinterface verschijnt
3. **Defaults- en bereiken-check**: alle genoemde waarden vergelijken met `cfg_defaults.h` en `cfg_limits.h`
4. **Beheerder-test**: laat een collega-installateur het document doorlezen; kan hij/zij met alleen dit document een nieuwe controller installeren en configureren?
5. **Markdown render check**: open `manual/beheerderHandleiding.md` in een markdown-viewer; controleer dat tabellen, ASCII-schermblokken en TOC correct renderen
6. **Foto-placeholders**: lijst alle `[FOTO: ...]` en `[SCHERMAFBEELDING: ...]` zodat duidelijk is welke afbeeldingen nog gemaakt moeten worden
7. **Cross-references**: alle verwijzingen naar de boer-handleiding controleren
