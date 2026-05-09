# Plan: Boer-handleiding voor de kascontroller

## Context

De kascontroller (ESP32-S3 firmware in dit repo) wordt door de boer dagelijks gebruikt om temperatuur en vochtigheid in de kas te beheren door drie motorgestuurde ramen automatisch te openen/sluiten. Tot nu toe is er alleen technische documentatie (`design/`, README) — geen gebruikshandleiding voor de eindgebruiker. De boer heeft een Nederlandstalige handleiding nodig die uitlegt wat hij op het apparaat ziet, wat hij zelf kan instellen, hoe hij inlogt, en wat alarmen betekenen — zonder dat hij de technische ontwerpdocumenten moet doornemen.

Het beoogde resultaat is **één compleet markdown-document** (`manual/handleiding.md`) waarmee de boer:
- Begrijpt wat de controller doet en waarom (klimaatregeling via alleen ventilatie).
- De informatie op LCD en webinterface kan lezen en interpreteren.
- Klimaat-setpoints en wifi kan instellen.
- Alarmen kan herkennen en weet wanneer hij de beheerder moet bellen.

## Beslissingen (afgestemd met gebruiker)

- **Structuur**: één bestand `manual/handleiding.md` (geen gesplitste hoofdstukken).
- **Scope**: alleen boer-handleiding. Admin wordt kort genoemd in de rolverdeling; eventuele aparte beheerdershandleiding komt later.
- **LCD-termen**: Engelse termen letterlijk weergeven (zoals ze op het scherm staan) met Nederlandse uitleg ernaast.
- **Afbeeldingen**: tekstplaceholders zoals `[FOTO: vooraanzicht kast]` plus map `manual/images/` aanmaken; verwijzen naar bestaande PNG'en in `design/` waar relevant (`lcd_gui_state_diagram.png`).
- **Authoritatieve bron voor PIN-lengtes**: `firmware/src/auth/pin_auth.h` (4 cijfers Farmer / 8 cijfers Admin). De LCD-design-doc noemt 6 cijfers, maar de broncode is wat op het apparaat draait.

## Te maken bestanden

| Pad | Doel |
|---|---|
| `manual/handleiding.md` | Hoofdhandleiding, één bestand, ~1500–2500 regels markdown |
| `manual/images/.gitkeep` | Lege map waar foto's later komen |

## Inhoud van `manual/handleiding.md`

Eén document met onderstaande hoofdstukken in deze volgorde. De stijl is **kort, concreet, met genummerde stappen** waar mogelijk; geen lange technische uitwijdingen.

### Voorblad
- Titel: **Handleiding Kascontroller — voor de boer**
- Versie + datum (placeholder)
- Korte zin: "voor wie is dit document"
- Veiligheidswaarschuwing: niet handmatig aan ramen werken als motoren actief zijn

### Inhoudsopgave
- Genummerde TOC met links naar elke sectie

### 1. Over deze handleiding
- Doelpubliek: de boer / kasgebruiker
- Wat staat er **niet** in (technische details → beheerder)
- Bij problemen: contactgegevens beheerder (placeholder)

### 2. Wat doet de kascontroller?
- Doel: automatische klimaatregeling van één kas
- Werking in één alinea: meet binnen-T en binnen-RH, opent/sluit drie ramen op basis van setpoints, dag/nacht-onderscheid automatisch
- Wat hij **niet** doet: geen verwarming, geen koeling, geen schermen, geen besproeiing. Alleen ventilatie.
- Geen tussenstanden van ramen — een raam is **OPEN** of **DICHT** (geen 30%, 60%); de controller werkt in stappen door verschillende ramen op verschillende momenten te openen.

### 3. De kas en het systeem
- `[FOTO: bovenaanzicht / plattegrond kas met M1, M2, M3]`
- Kas: 40 m × 16 m, gevelvormig dak oost-west
- **De drie ramen** (gebruik termen die ook in webinterface en LCD verschijnen):
  - **M1 — Dakbeluchting Zuid** (8 m², ca. 21 s open/dicht)
  - **M2 — Dakbeluchting Noord** (8 m², ca. 21 s open/dicht)
  - **M3 — Zijwandbeluchting Noord** (80 m², ca. 171 s open/dicht)
- **Sensoren**:
  - T/RH-sensor binnen (FG6485A, Modbus)
  - Wind-sensor buiten (SenseCAP S200, snelheid + richting)
- **Motor-relaisbox** Hotraco RRK-3 — in de kas gemonteerd, rechts bij binnenkomst door de ingang, op dezelfde plek als de kascontroller. Bedient de raam-motoren, heeft eindschakelaars die de motoren stoppen wanneer een raam volledig open of dicht is, en een eigen alarmuitgang die de kascontroller waarschuwt bij motorstoring.
- **Kascontroller** — de elektronische besturing (microprocessor, LCD, toetsenbord, sensor-interfaces, wifi) die het hele systeem aanstuurt op basis van metingen, regels en setpoints.
- Eenvoudig blokdiagram (ASCII of placeholder)

### 4. Hoe regelt de controller het klimaat?
- **Setpoints**: minimum/maximum T en RH, apart voor **dag** en **nacht**
- Dag/nacht-omschakeling automatisch op basis van zonsopkomst/-ondergang (locatie ingesteld door beheerder)
- **Hysterese**: kleine bandbreedte rondom setpoint om te voorkomen dat ramen elke seconde openen/sluiten
- **Sliding average** (glijdend gemiddelde): meetwaarden worden gemiddeld over 1–60 min
- **Stapsgewijs ventileren**: eerst M1, dan M1+M2, dan alle drie — de controller kiest hoeveel raamoppervlak nodig is
- **Conflict-prioriteit** (T eerst / RH eerst / automatisch op basis van afwijking) — instelbaar door boer
- **Windbeveiliging**: bij te harde wind sluiten alle ramen, ongeacht klimaat-vraag

### 5. De controller (fysiek)
- `[FOTO: vooraanzicht kast met LCD, toetsenbord, LEDs]`
- **De kast**: globale beschrijving, waar zit hij, IP-rating placeholder

#### 5.1 LCD-display (16 × 2 tekens)
- Schermen wisselen automatisch elke 5 seconden door de informatieschermen heen
- Met toets `D ← BACK` kan de gebruiker versneld doorheen stappen — handig om snel een specifiek scherm te bereiken zonder te wachten tot de auto-rotatie er aan toe is
- Voorbeeldschermen (ASCII-blokken) met **Engelse weergave + Nederlandse uitleg**, gebaseerd op de daadwerkelijke firmware-strings (`firmware/src/ui_display/ui_display.cpp`):
  - **Mode-regel** — toont één van:
    - `Mode: AUTO` — normale automatische werking
    - `Mode: WIND` — wind-override actief, alle ramen dicht
    - `Mode: ALARM` — motor-alarm actief
    - `Mode:Window Cal.` — kalibratie van de ramen
  - **Sensor-status** — bij sensor-uitval verschijnt `** SENSOR FAULT` op regel 2
  - T, RH, wind-meting
  - Raamposities per motor
  - Day/Night Setpoints
  - WiFi-status (SSID, IP)

#### 5.2 Toetsenbord (4 × 4)
- Tabel met alle toetsen, hun symbolen, en functies in **navigatie**, **menu** en **bewerk-modus** — gebaseerd op `design/LCD_GUI_Design.md` §3.2
- Korte legenda: `▲` `▼` `⏎ ENTER` `← BACK` `# OK` `* CLR` + cijfers 0–9

#### 5.3 LED-indicatoren
- **RGB-LED** (zichtbaar door de doorzichtige kap):
  - **Groen** = normale werking (`OPERATIONAL`)
  - **Oranje/amber** = waarschuwing — sensorfout, windbeveiliging uitgeschakeld, vochtregeling uit, of wind-override actief
  - **Rood** = kritiek alarm — motor-noodstop of systeemfout
- LED dimt 's nachts automatisch (instelbaar door beheerder)
- **Heartbeat-LED** (kleine amber LED): knippert 1× per seconde → bewijst dat de firmware draait. Knippert niet → controller is bevroren of uit

### 6. De webinterface (via wifi)
- De webinterface biedt meer overzicht en mogelijkheden dan het LCD-scherm: live grafieken, sensorhistorie, en alle setpoints op één scherm
- **Voorwaarde**: de beheerder moet de kascontroller eerst hebben verbonden met het kas/thuis-wifinetwerk (client mode). Zonder die configuratie heeft de boer geen toegang tot de webinterface. AP-modus is een hulpmiddel dat alleen de beheerder gebruikt — dat hoeft de boer niet zelf te doen.
- **Bereiken** (zodra de beheerder wifi heeft ingesteld):
  - IP-adres van de controller staat op het LCD-scherm WiFi (bijvoorbeeld `http://192.168.1.100`)
  - Open een browser op een laptop, tablet of smartphone die op hetzelfde netwerk zit
  - Typ het IP-adres in en de webinterface opent
- **Hoofdtabs** (afbeeldingen later toevoegen):
  - **Status** — live overzicht zonder login (T, RH, wind, raamposities, modus, alarmen, klok, wifi, SD-kaart)
  - **Climate** — setpoints (zichtbaar voor Farmer)
  - **Wind** — windbeveiliging-instellingen
  - **Motors** — alleen Admin
  - **System** — alleen Admin (wifi, locatie, etc.)
  - **Access** — login en PIN-beheer
- Sensorhistorie: toegankelijk zonder login, ververst elke 2 minuten
- Sessie verloopt automatisch na 5 min inactiviteit

### 7. De twee gebruikersrollen
- **Farmer** (boer / kasgebruiker)
  - 4-cijferige PIN — bij eerste levering staat deze op fabrieksstandaard `1234`; **wijzig deze direct na ingebruikname**
  - Mag: klimaat-setpoints, vochtregeling aan/uit, conflict-prioriteit, windbeveiliging aan/uit (met logging), eigen PIN wijzigen
- **Admin / Technisch beheerder**
  - 8-cijferige PIN — door beheerder ingesteld
  - Mag: alle systeeminstellingen, wifi-config, motor-instellingen, sensor-instellingen, locatie
  - Voor de boer relevant: bel de beheerder als motor-instellingen, wifi of sensoren niet kloppen
- **Lockout**: 5× foute PIN → 5 minuten geblokkeerd (per rol, geldt zowel op LCD als web)
- PIN's worden versleuteld opgeslagen — kunnen niet worden teruggelezen, alleen vervangen

### 8. Gebruik zonder inloggen — informatiemenu
- Op de LCD: schermen rouleren automatisch elke 5 sec
- **Volgorde van schermen** (1–8, zoals in `design/LCD_GUI_Design.md` §4): PIN-entry → System Status → Roof Vent → Side Vent → Day Setpoints → Night Setpoints → WiFi → Alarms (indien actief)
- **Handmatig navigeren** zonder login: `▲` vorige, `▼` volgende, `*` terug naar eerste scherm
- **In de webinterface**: tab "Status" zonder login zichtbaar
- Wat is **niet** zichtbaar zonder login: setpoints wijzigen, windbeveiliging toggle, wifi-instellingen wijzigen

### 9. Inloggen als boer
- **Op de LCD**:
  1. Druk `#` om naar PIN-entry te springen
  2. Voer 4-cijferige PIN in (cijfers 0–9)
  3. Druk `#` of `⏎ ENTER`
  4. Bij correct: scherm `Login OK`, daarna `User: FARMER`
  5. Bij fout: `Wrong PIN!` — opnieuw proberen
- **In de webinterface**:
  1. Open browser, ga naar IP van controller
  2. Klik knop **Farmer**
  3. Vul PIN in
- **Uitloggen**:
  - LCD: menu → `Logout`
  - Web: knop "Logout" in `Access` tab
  - Automatisch na 120 sec rust in menu

### 10. Klimaat instellen
- Allereerst: inloggen als Farmer (zie §9)

#### 10.1 Op de LCD
Menu-items in volgorde, gebaseerd op `design/LCD_GUI_Design.md` §5.1. Per item: voorbeeldscherm + bediening:
- **Day Temp** — bereik 15–30 °C, stappen van 1 °C
- **Night Temp** — bereik 10–25 °C; moet ≤ Day Temp
- **Day Humidity** — bereik 40–90%, stappen van 5%
- **Night Humidity** — bereik 40–90%
- **Vochtregeling aan/uit** (Humidity Control)
- **Windbeveiliging aan/uit** (Wind Protection) — met bevestiging; wordt gelogd
- **Conflict-prioriteit** (Conflict Resolution) — Temperature first / Humidity first / Auto
- **Change PIN** — 4 cijfers tweemaal invoeren ter bevestiging
- **Logout**

Bediening per scherm: `▲` verhogen, `▼` verlagen, `⏎ ENTER` of `#` opslaan, `← BACK` annuleren. Time-out van 60 sec → annuleren zonder opslaan.

#### 10.2 In de webinterface (tab Climate)
- Schuifregelaars + nummerveld + `Apply`-knop per setpoint
- Velden: T max day, RH max day, RH min day, T max night, RH max night, RH min night
- Tooltip-uitleg bij elk veld (mouse-over) — verwijst naar `design/webGuiMouseOver.md` voor de exacte teksten

#### 10.3 Wat zijn goede setpoints?
- Tabel met richtwaarden per teelt (placeholder — door beheerder/teler in te vullen)
- Algemene tip: nacht-T iets lager dan dag-T; RH boven 85% leidt tot schimmelrisico

### 11. Wifi en webinterface gebruiken
- **De beheerder regelt de wifi-configuratie van de controller** — zowel het tijdelijke Access Point (AP-modus, dat alleen voor onderhoud aangezet wordt) als de verbinding met het kas-/thuisnetwerk (client-modus). De boer hoeft hier niets aan in te stellen.
- **Wat de boer wél doet**: zodra de beheerder de controller met het wifi-netwerk heeft verbonden, kan de boer de webinterface gebruiken:
  1. Lees het IP-adres af van de LCD (scherm WiFi toont SSID en IP)
  2. Open op een laptop, tablet of smartphone die op hetzelfde wifi-netwerk zit een browser
  3. Typ het IP-adres in (bijvoorbeeld `http://192.168.1.100`)
  4. De Status-tab is direct zichtbaar zonder login
  5. Voor klimaatinstellingen: klik **Farmer**, voer 4-cijferige PIN in
- **Tip**: bookmark het IP-adres in de browser zodat je niet steeds hoeft te kijken op de LCD
- **Als het IP-adres verandert** (bijvoorbeeld na een wijziging in het thuisnetwerk): kijk opnieuw op de LCD WiFi-pagina, of vraag je beheerder om een vast IP-adres te configureren
- **Geen wifi-verbinding meer?** De LCD geeft dit aan op het WiFi-scherm. Eerst zelf de wifi-router controleren; als die werkt en de controller komt nog steeds niet online: bel de beheerder.

### 12. Alarmen en bedrijfsmodi — wat betekenen ze, wat te doen
Gebaseerd op de daadwerkelijke firmware-implementatie (`firmware/src/types/app_types.h`, `firmware/src/ui_display/ui_display.cpp:686–714`).

#### 12.1 Bedrijfsmodi (Mode-regel op LCD)

| LCD-tekst | Betekenis | Wat doet de controller? | Wat moet je doen? |
|---|---|---|---|
| `Mode: AUTO` | Normale automatische werking | Regelt T en RH binnen setpoints | Niets |
| `Mode: WIND` | Wind-override actief — wind te hard | **Alle ramen dicht; klimaatregeling onderdrukt** | Wachten tot de wind afneemt; controleer of windbeveiliging aan staat |
| `Mode: ALARM` | Motor-alarm actief (RRK-3 noodstop) | **Alle relais uit; motoren staan stil** | **Bel de beheerder onmiddellijk**; pas niet zelf aan motoren of bedrading |
| `Mode:Window Cal.` | Kalibratie van de ramen — alle ramen worden gesloten om de uitgangspositie te bepalen | Sluit M1, M2, M3 gelijktijdig; duurt tot ~3 minuten | Wachten; niet ingrijpen, niet handmatig aan de ramen werken |

#### 12.2 Sensor- en status-indicaties

| LCD-tekst | Betekenis | Wat moet je doen? |
|---|---|---|
| `** SENSOR FAULT` (regel 2) | T/RH-sensor reageert niet | **Bel de beheerder**; controleer kort of sensorkabel zichtbaar beschadigd is |
| Geen wind-meting / `--` op windscherm | Wind-sensor reageert niet | Bel de beheerder; controleer of de wind-sensor buiten niet bedekt of beschadigd is |

#### 12.3 RGB-LED kleuren samengevat

- **Groen** — alles in orde, `Mode: AUTO`
- **Oranje** — waarschuwing: wind-override actief, sensor-fout, of windbeveiliging staat uit
- **Rood** — kritiek: motor-alarm; ramen worden niet meer aangestuurd

#### 12.4 Tijdens kalibratie

Bij iedere opstart (of na een power-cycle) voert de controller automatisch een **CLOSE_ALL kalibratie** uit: alle drie de ramen worden gelijktijdig gesloten zodat de controller weet wat de uitgangspositie is. Dit duurt:
- **M1 en M2 (dakramen)**: ~26 seconden
- **M3 (zijwand)**: ~176 seconden (totale duur)

Tijdens deze ~3 minuten staat de mode op `Mode:Window Cal.`. Daarna gaat de controller automatisch naar `Mode: AUTO`. **Als bij het opstarten al een motor-alarm actief was**, wordt de kalibratie overgeslagen en gaat de controller direct naar `Mode: ALARM`. In dat geval: bel de beheerder.

### 13. Inschakelen na stroomuitval
Na elke power-cycle (stroomuitval, beheerder heeft de stekker eruit getrokken, etc.) doorloopt de controller automatisch dezelfde startsequentie:

1. **Voeding terug** — de LCD licht op binnen enkele seconden; de heartbeat-LED begint 1× per seconde te knipperen
2. **Mode-regel toont** `Mode:Window Cal.` — kalibratie van de ramen begint automatisch
3. **Alle ramen sluiten gelijktijdig**:
   - M1 en M2 (dak) zijn na ~26 seconden dicht
   - M3 (zijwand) is na ~176 seconden dicht
   - Totaal: ongeveer 3 minuten
4. **Niet ingrijpen tijdens deze fase**: niet handmatig aan ramen werken, niet inloggen, niet rebooten
5. **Na kalibratie** schakelt de controller naar `Mode: AUTO` — RGB-LED wordt groen — de klimaatregeling is weer actief
6. **Controleer**: zijn de eerder ingestelde setpoints nog correct? (Setpoints worden in geheugen bewaard en zouden dus nog moeten staan.)
7. **Als bij opstart een motor-alarm actief is** (RRK-3 noodstop niet vrijgegeven): de kalibratie wordt overgeslagen, mode toont `Mode: ALARM`, RGB-LED gaat rood. Bel in dat geval de beheerder.

> **Tip**: noteer de tijd waarop de stroom uitviel en hoe lang de uitval duurde — dit kan voor de beheerder waardevol zijn bij het opsporen van een probleem.

### 14. Onderhoud — wat de boer zelf doet
- LCD-scherm schoonvegen met droge doek (geen reinigingsmiddel)
- Sensoren periodiek visueel controleren — niet bedekt door spinrag, plantenresten, condens-druppels
- Ramen handmatig **nooit** verplaatsen tijdens motor-actie
- RTC-batterij (CR2032) — vervanging is taak van de beheerder; symptoom van lege batterij: tijd loopt niet meer of dag/nacht-omschakeling klopt niet meer
- **Power-cycle uitvoeren** (volledige reset van de controller):
  - Voedingstekker eruit, 10 seconden wachten, weer erin — dit start de controller volledig opnieuw op (inclusief raam-kalibratie van ~3 minuten)
  - `[FOTO: voedingstekker en stopcontact bij de kascontroller-kast]`
  - Alternatief: druk de **RESET-knop op het microprocessorboard** (in de kast) — dezelfde werking als kort de stekker eruit halen
  - `[FOTO: microprocessorboard in kast, met RESET-knop en BOOT-knop gemarkeerd]`

### 15. Probleemoplossing (FAQ)
- **LCD blank** → voeding controleren; is heartbeat-LED zichtbaar?
- **Heartbeat-LED knippert niet** → controller bevroren; doe een power-cycle (zie §14)
- **RGB-LED rood** → kritiek alarm (`Mode: ALARM`); bel beheerder
- **RGB-LED oranje** → waarschuwing: lees mode-regel en eventuele `** SENSOR FAULT` op de LCD
- **PIN vergeten (Farmer)** → beheerder kan resetten via Admin-menu, of via de fysieke reset-procedure (zie §17)
- **PIN vergeten (Admin)** → fysieke reset-procedure op het microprocessorboard (zie §17)
- **Webinterface niet bereikbaar** → IP-adres juist gelezen op LCD? Apparaat op hetzelfde wifi-netwerk? Anders: bel beheerder
- **Ramen reageren niet** → controleer mode-regel: bij `Mode: WIND` zit de wind-override aan; bij `Mode: ALARM` motor-alarm; bel beheerder
- **Setpoint accepteert mijn waarde niet** → controleer bereik (15–30 °C dag, 10–25 °C nacht; nacht-T moet ≤ dag-T; RH 40–90% in stappen van 5%)
- **Kalibratie duurt lang** → ~3 minuten is normaal (M3 zijwand-raam heeft ~176 sec nodig); pas na die tijd actie ondernemen

### 16. Verklarende woordenlijst
Korte tabel:
- **Setpoint** — gewenste waarde
- **Hysterese** — bandbreedte rondom setpoint waarbij niet wordt geschakeld
- **Sliding average** — glijdend gemiddelde over X minuten
- **Wind override** — automatisch sluiten van ramen bij te harde wind
- **AP / Access Point** — controller is zelf een wifi-netwerk
- **Modbus / RS485** — communicatieprotocol naar sensoren
- **RTC** — real-time klok in de controller
- **Conflict-prioriteit** — keuze welke regelactie voorrang krijgt als T en RH tegelijk om actie vragen
- **Beaufort** — eenheid van windkracht (0–12)
- Engels↔Nederlands LCD-termen overzicht (OPERATIONAL = normale werking, AUTO = automatisch, MANUAL = handmatig, LOCKED = vergrendeld, etc.)

### 17. Reset-procedure (BOOT-knop op microprocessorboard)
Voor het geval een PIN vergeten is, of de controller moet volledig terug naar fabrieksinstellingen, kan de boer (of bij voorkeur de beheerder) een fysieke reset uitvoeren via de **BOOT-knop** op het microprocessorboard in de kast.

`[FOTO: microprocessorboard met BOOT-knop en RESET-knop duidelijk aangewezen]`

**Procedure** (gebaseerd op `firmware/src/ui_display/ui_display.cpp:606–655`):

1. Open de kast en lokaliseer de **BOOT-knop** op het microprocessorboard (LOLIN S3). Dit is een andere knop dan de RESET-knop — let goed op.
2. Druk de BOOT-knop in en houd deze ingedrukt
3. Op de LCD verschijnt na enkele seconden een melding die aangeeft welk reset-niveau actief wordt
4. Laat de knop los op het gewenste niveau:

| Inhouden | LCD-melding | Effect |
|---:|---|---|
| **0–5 sec** | (geen melding) | Geen actie — los gelaten zonder gevolgen |
| **5–10 sec** | `Reset PIN?` | **PIN-codes** terug naar fabrieksstandaard. Andere instellingen blijven behouden. Geen reboot. |
| **10–15 sec** | `Reset settings?` | **Alle instellingen** (klimaat, wind, motor, wifi, MQTT, systeem) terug naar fabrieksstandaard. PIN's ook gereset. Geen reboot. |
| **15–20 sec** | `Restart!` / `Restarting...` | Volledige reset + automatische herstart van de controller |

5. Bij **20 seconden continu vasthouden** voert de controller automatisch de volledige reset + herstart uit (niveau 3) — het is dus niet nodig om langer dan 20 sec vast te houden.
6. **Na een reset op niveau 2 of 3**: alle instellingen die de beheerder had geconfigureerd zijn weg. Bel de beheerder om wifi en motor-tijden opnieuw in te stellen.
7. **Na een reset op niveau 1**: PIN's staan weer op fabrieksstandaard. Login met de fabrieks-Farmer-PIN, en wijzig direct.

> **Waarschuwing**: gebruik niveau 2 en 3 alleen wanneer écht nodig — alle door de beheerder ingestelde wifi-, motor- en locatie-parameters gaan verloren.

### 18. Bijlage A — contactgegevens beheerder
- Naam: \[invullen]
- Telefoon: \[invullen]
- E-mail: \[invullen]
- Bij motor-, wifi-, of sensor-storingen: eerst de beheerder bellen voordat zelf gerepareerd wordt

### 19. Versie en wijzigingshistorie
- Versie 1.0 — eerste uitgave (datum)
- Gebaseerd op firmware 1.16.34

## Bronnen die in het document gebruikt/gerefereerd worden
- `design/LCD_GUI_Design.md` — schermlay-outs, menu-structuur, foutmeldingen
- `design/lcd_gui_state_diagram.png` — kan optioneel als afbeelding meegenomen worden
- `design/webGuiMouseOver.md` — tooltip-teksten voor webinterface (referentie voor uitleg per veld)
- `design/functionalRequirementsSpecification.md` — kasdimensies, regelstrategie
- `documentation/Sensors/sensors.md` — sensor-typen
- `firmware/src/auth/pin_auth.h` — **authoritatieve** PIN-lengtes (4/8) en lockout-gedrag
- `firmware/data/index.html`, `firmware/data/app.js` — webinterface tabs en velden
- `README.md` — kasdimensies en algemene context

## Verificatie
Omdat dit een tekst-document is en geen code, gebeurt verificatie handmatig:
1. **Inhoudelijke review door gebruiker** (Remko) — check of alle uitleg klopt met huidige firmware (1.16.34) en hardware
2. **Termen-check**: vergelijk de Engelse termen in het document met wat daadwerkelijk op het LCD verschijnt en in `firmware/data/index.html` staat
3. **PIN-lengtes**: zorg dat 4 (Farmer) en 8 (Admin) overal consistent zijn (niet 6 zoals in oude design-doc)
4. **Boer-test**: laat een boer (niet-techneut) de handleiding doorlezen en zien of hij setpoints kan wijzigen en wifi kan instellen alleen met de tekst als hulpmiddel
5. **Markdown render check**: open `manual/handleiding.md` in een markdown-viewer; controleer dat tabellen, ASCII-schermblokken en TOC correct renderen
6. **Foto-placeholders**: lijst alle `[FOTO: ...]`-markeringen op zodat duidelijk is welke foto's nog gemaakt moeten worden