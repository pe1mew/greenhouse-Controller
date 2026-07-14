# Handleiding Kascontroller — voor de boer

**Versie:** 1.17
**Datum:** 2026-06-27
**Firmware:** 2.1.1

---

> **Voor wie is deze handleiding?**
> Dit document is geschreven voor de boer / kasgebruiker die de kascontroller dagelijks bedient. Het legt uit wat je op het apparaat ziet, hoe je inlogt, hoe je het klimaat instelt, en wat alarmen betekenen. Technische installatie- en configuratiezaken staan hier niet in, die zijn voor de beheerder.

> **Veiligheid**
> Werk **nooit handmatig** aan een raam terwijl een motor actief is. Wacht tot de motor stilstaat. Bij twijfel: schakel eerst de voeding uit (stekker eruit) van de **Motorbox (Hotraco RRK-3)** of bel de beheerder.

---

## Inhoudsopgave

1. [Over deze handleiding](#1-over-deze-handleiding)
2. [Wat doet de kascontroller?](#2-wat-doet-de-kascontroller)
3. [De kas en het systeem](#3-de-kas-en-het-systeem)
4. [Hoe regelt de controller het klimaat?](#4-hoe-regelt-de-controller-het-klimaat)
5. [De controller (fysiek)](#5-de-controller-fysiek)
6. [De webinterface (via wifi)](#6-de-webinterface-via-wifi)
7. [De twee gebruikersrollen](#7-de-twee-gebruikersrollen)
8. [Gebruik zonder inloggen — informatiemenu](#8-gebruik-zonder-inloggen--informatiemenu)
9. [Inloggen als boer](#9-inloggen-als-boer)
10. [Klimaat instellen](#10-klimaat-instellen)
11. [Wifi en webinterface gebruiken](#11-wifi-en-webinterface-gebruiken)
12. [Alarmen en bedrijfsmodi](#12-alarmen-en-bedrijfsmodi--wat-betekenen-ze-wat-te-doen)
13. [Inschakelen na stroomuitval](#13-inschakelen-na-stroomuitval)
14. [Onderhoud — wat de boer zelf doet](#14-onderhoud--wat-de-boer-zelf-doet)
15. [Handmatige overname via de motorbox](#15-handmatige-overname-via-de-motorbox)
16. [Probleemoplossing (FAQ)](#16-probleemoplossing-faq)
17. [Verklarende woordenlijst](#17-verklarende-woordenlijst)
18. [Reset-procedure (BOOT-knop)](#18-reset-procedure-boot-knop-op-microprocessorboard)
19. [Bijlage A — contactgegevens beheerder](#19-bijlage-a--contactgegevens-beheerder)
20. [Bijlage B — Aanbevolen startinstellingen per gewas](#20-bijlage-b--aanbevolen-startinstellingen-per-gewas)
21. [Versie en wijzigingshistorie](#21-versie-en-wijzigingshistorie)

---

## 1. Over deze handleiding

Deze handleiding is bedoeld voor de **boer / kasgebruiker** die de kascontroller in dagelijks gebruik bedient.

**Wat staat er in deze handleiding?**
- Wat de kascontroller doet en hoe hij werkt
- Hoe je het LCD-scherm en de webinterface afleest
- Hoe je inlogt en het klimaat instelt
- Hoe je alarmen herkent en wat je dan doet

**Wat staat er niet in deze handleiding?**
- Hardware-installatie en bedrading
- Netwerkconfiguratie en wifi-instellingen door de beheerder
- Motor-tijden, sensor-poll-intervallen en andere technische parameters van de kascontroller
- Reparaties en vervanging van onderdelen

Voor al die onderwerpen: neem contact op met de **beheerder** (zie [Bijlage A](#19-bijlage-a--contactgegevens-beheerder)).

**Bij twijfel of een storing**: probeer niet zelf de hardware te openen of aan te passen. Bel eerst de beheerder.

---

## 2. Wat doet de kascontroller?

De kascontroller is een geautomatiseerd systeem dat het **klimaat in één kas** beheert door drie motorgestuurde ramen op de juiste momenten te openen en te sluiten. Hij meet voortdurend de temperatuur en de relatieve luchtvochtigheid binnen, en de windkracht buiten, en bepaalt op basis van door jou ingestelde grenswaarden of er geventileerd moet worden.

**Wat doet de kascontroller wél:**
- Meet **temperatuur (T)** en **relatieve luchtvochtigheid (RH)** in de kas
- Meet **windsnelheid en -richting** buiten de kas
- Opent en sluit drie ramen automatisch om T en RH binnen de gewenste grenzen te houden
- Schakelt automatisch tussen **dag-** en **nachtinstellingen** op basis van zonsopkomst en zonsondergang
- Sluit alle ramen automatisch wanneer de wind te hard wordt (windbeveiliging)
- Kan zichzelf **'s nachts automatisch bijwerken** met nieuwe software van de fabrikant — je hoeft hier niets voor te doen; na afloop herstart hij kort

**Wat doet de kascontroller niet:**
- Geen verwarming
- Geen koeling
- Geen klimaatschermen
- Geen besproeiing of CO₂-dosering
- Hij weet niet hoeveel een raam open staat — hij stuurt alleen volledige open- of sluit-commando's. Een raam is in de praktijk **OPEN** of **DICHT**, niet "30%". De controller varieert het ventilatie-oppervlak door verschillende ramen op verschillende momenten te openen.

---

## 3. De kas en het systeem

![FOTO: bovenaanzicht / plattegrond van de kas met M1, M2 en M3 aangegeven](images\KasRaamlocaties.png)

*Figuur 1: bovenaanzicht / plattegrond van de kas met M1, M2 en M3 aangegeven*

**Kas:**
- Lengte (oost-west): ongeveer 40 m
- Breedte (noord-zuid): ongeveer 16 m
- Dak: gevelvormig (puntdak), nok in oost-west richting

**De drie motorgestuurde ramen:**

| ID | Naam (zoals op de webinterface) | Locatie | Oppervlak | Open- en sluit-tijd |
|---|---|---|---|---|
| **M1** | Dakbeluchting Zuid | Zuidelijke dakhelft | ca. 8 m² | ca. 21 sec. |
| **M2** | Dakbeluchting Noord | Noordelijke dakhelft | ca. 8 m² | ca. 21 sec. |
| **M3** | Zijwandbeluchting Noord | Noordelijke zijwand | ca. 80 m² | ca. 171 sec. (≈ 3 min.) |

**Sensoren:**
- **T/RH-sensor** (temperatuur en luchtvochtigheid) — type FG6485A — gemonteerd binnen in de kas, op een representatieve plek (geen direct zonlicht, geen druipwater)
- **Wind-sensor** (snelheid en richting) — type SenseCAP S200 — buiten gemonteerd, op een open plek zonder afscherming door objecten

**Motor-relaisbox Hotraco RRK-3:**
De motorbox bedient de drie raammotoren. Hij is **in de kas gemonteerd**, **rechts bij de ingang**, op dezelfde plek als de kascontroller. De relaisbox heeft eindschakelaars voor elk raam — zodra een raam volledig open of dicht is, stopt de bijbehorende motor automatisch. De relaisbox heeft ook een eigen **alarmuitgang**: bij motorstoring (bijvoorbeeld een vastgelopen raam of overbelasting) wordt de kascontroller hiervan op de hoogte gesteld.

**Kascontroller:**
De kascontroller is de elektronische besturing van het hele systeem: een microprocessor met een LCD-scherm, een toetsenbord, sensor-interfaces en een wifi-module. De kascontroller leest de sensoren uit, vergelijkt de meetwaarden met de door jou ingestelde setpoints, en stuurt op basis daarvan de Hotraco RRK-3 aan om ramen te openen of te sluiten.

**Schematisch overzicht:**

![Schematisch overzicht kas besturing](images\SchematischOverzicht.png)

*Figuur 2: Schematisch overzicht kas besturing*

### 3.1 Unieke ID van de kascontroller

Elke kascontroller een **unieke identificatie van vier letters** (bijvoorbeeld `5C88` of `12F0`).

De ID wordt op vier plaatsen gebruikt:

| Waar | Hoe het eruit ziet |
|---|---|
| **AP-SSID** (LCD-scherm 4 wanneer de AP actief is) | `Greenhouse-5C88` |
| **LCD-scherm 7** (Firmware/Uptime) | rechts op regel 1, naast het versienummer: `FW: 2.1.1  5C88` |
| **Webinterface, voettekst** | onderaan de pagina: `Greenhouse Controller – v2.1.1 · 5C88` |
| **SD-logbestand (bestandsnaam)** | eerste vier tekens van de bestandsnaam: `5C88_20260507143022.csv` |

---

## 4. Hoe regelt de controller het klimaat?

De controller probeert continu de temperatuur en luchtvochtigheid binnen de door jou ingestelde grenzen te houden, door op het juiste moment de juiste ramen te openen en te sluiten.

**Setpoints (gewenste waarden):**
Je stelt vier soorten grenswaarden in, apart voor **dag** en **nacht**:
- Maximum temperatuur (boven deze waarde: ramen openen om af te koelen, onder deze waarde: ramen dicht houden om warmte vast te houden)
- Maximum vochtigheid (boven deze waarde: ramen openen om vocht af te voeren)
- Minimum vochtigheid (onder deze waarde: ramen dicht houden om vocht vast te houden)

**Dag/nacht-omschakeling:**
Dit is automatisch op basis van zonsopkomst en zonsondergang. De geografische locatie wordt automatisch bepaald of door de beheerder ingesteld; de controller berekent zelf wanneer de zon op- en ondergaat.

**Hysterese:**
Een kleine bandbreedte rondom elke setpoint, zodat de ramen niet om de seconde openen en sluiten als de meetwaarde precies op het setpoint zit. Hysterese-instellingen staan in de webinterface (en zijn alleen door de beheerder aan te passen).

**Glijdend gemiddelde (Sliding average):**
De controller middelt de meetwaarden over een instelbaar venster (1–60 minuten) voordat hij een beslissing neemt. Dit voorkomt dat één afwijkende meting (bijvoorbeeld een korte zonnestraal op de sensor) onmiddellijk tot ramen openen leidt.

**Stapsgewijs ventileren:**
De controller opent de ramen in stappen, afhankelijk van de gewenste afkoeling of vochtafvoer:
1. Eerst alleen **M1** (klein dakraam zuid)
2. Dan **M1 + M2** (beide dakramen)
3. Dan **M1 + M2 + M3** (alles, inclusief de grote zijwand)

Dit voorkomt dat het klimaat binnen plotseling sterk verandert. ook draagt dit bij aan een "rustig" gedrag van de ramen. 

**Conflict-prioriteit:**
Soms vraagt de temperatuur om ramen open (te warm) en de vochtigheid om ramen dicht (te droog), of andersom. In dat geval volgt de controller je gekozen prioriteit:
- **Temperature first** — temperatuur krijgt voorrang
- **Humidity first** — vochtigheid krijgt voorrang
- **Auto** — de regeling kijkt naar welke afwijking het grootst is en geeft daaraan voorrang

**Windbeveiliging:**
Bij te harde wind sluit de controller **alle ramen automatisch**, ongeacht wat het klimaat vraagt. Dit beschermt de motoren en de raamconstructie. De windgrenswaarden worden door de beheerder ingesteld.

---

## 5. De controller (fysiek)

![FOTO: vooraanzicht van de kascontroller-kast met LCD, toetsenbord en zichtbare LEDs](images\kasControllerFrontView.png)

*Figuur 3: vooraanzicht van de kascontroller-kast met LCD, toetsenbord en zichtbare LEDs*


De kascontroller bevindt zich in een afgesloten kast bij de ingang van de kas, naast de Hotraco RRK-3. Op de voorkant zie je:
- Een **LCD-scherm** met statusinformatie, die kleurt bij wind alarm
- Een **toetsenbord** voor bediening
- Een **RGB-LED** (zichtbaar door de doorzichtige kap) die de globale status aangeeft
- Een **heartbeat-LED** (kleine groene LED rechts) die aangeeft dat de firmware loopt
- Een **voeding-LED** (kleine groene LED links) die aangeeft dat de controller spanning heeft

### 5.1 LCD-display

Het LCD-display toont alle statusinformatie in compacte tekstschermen. Wanneer er geen gebruiker is ingelogd en er geen toetsen worden ingedrukt, **wisselen de schermen automatisch elke 5 seconden** door zes informatieschermen. Dit heet *auto-rotatie*.

Met de toets `D` (STEP) kun je **versneld naar het volgende scherm stappen** — handig om snel een specifiek scherm te bereiken zonder te wachten tot de auto-rotatie er aan toe is.

> **Let op**: elke andere toets (1, 2, 3, 4, A, B, C, *, # behalve in specifieke uitzonderingen op het Network- en Time-scherm) opent het hoofdmenu in plaats van naar het volgende statusscherm te gaan.

#### Het mode-veld

Op het derde statusscherm staat een **Mode-regel**. Deze geeft aan in welke bedrijfsmodus de controller zich bevindt. Mogelijke waarden zijn:

| LCD-tekst | Betekenis |
|---|---|
| `Mode: AUTO` | Normale automatische werking van de kascontroller |
| `Mode: WIND` | Wind-override actief — alle ramen dicht door te harde wind. de Kascontroller is gestopt met besturen |
| `Mode: ALARM` | Motor-alarm — de Hotraco RRK-3 heeft een fout gemeld. de Kascontroller is gestopt met besturen |
| `Mode:Window Cal.` | Kalibratie van de ramen (alle ramen worden gesloten om de uitgangspositie te bepalen) |

Zie [hoofdstuk 12](#12-alarmen-en-bedrijfsmodi--wat-betekenen-ze-wat-te-doen) voor uitgebreide uitleg.

#### Sensor-foutindicatie

Wanneer de T/RH-sensor niet (meer) reageert, verschijnt op het temperatuur-scherm op de tweede regel `** SENSOR FAULT` in plaats van de RH-meting:

```
   +----------------+
   |Temp: 23 °C     |
   |** SENSOR FAULT |
   +----------------+
```

Daarnaast kleurt de LCD-achtergrond rood. De klimaatregeling is verstoord. Bel in dat geval de beheerder.

#### De zeven statusschermen (auto-rotatie)

De controller doorloopt zeven schermen in vaste volgorde, elk 5 seconden zichtbaar.

**Scherm 1 — Temperatuur en luchtvochtigheid:** De actuele temperatuur en relative-luchtvochtigheid

```
   +----------------+
   |Temp: 23 °C     |
   |  RH: 65 %      |
   +----------------+
```

 - Druk `#` op dit scherm om direct naar het Climate-menu te gaan voor het instellen van setpoints (vraagt om de Farmer-PIN als je nog niet bent ingelogd).
 - Bij sensoruitval toont regel 2 `** SENSOR FAULT`. 
 - Bij ongeldige meting: `Temp: --- °C` en `  RH: ---  %    `.

**Scherm 2 — Wind:** Toont de gemiddelde windsnelheid in m/s, en op rij 2 de richting in graden met de kompasrichting tussen haakjes (N, NE, E, SE, S, SW, W, NW).

```
   +----------------+
   |Wind: 2.3 m/s   |
   | Dir: 180 ° (S )|
   +----------------+
```

 - Druk `#` op dit scherm om direct naar het Wind-menu te gaan (vraagt om de Farmer-PIN als je nog niet bent ingelogd). 
 - Bij ongeldige meting: `Wind: -- m/s` en ` Dir: --- °     `.

**Scherm 3 — Bedrijfsmodus en sessie:** mode en sessie status

```
   +----------------+
   |Mode: AUTO      |
   |Sess: NONE      |
   +----------------+
```

 - Regel 1 toont de bedrijfsmodus (AUTO / WIND / ALARM / STANDBY / Window Cal.). 
 - Regel 2 toont de actieve sessie, is er ingelogd op de controller en door wie:
	- `Sess: NONE` — niemand ingelogd
	- `Sess: Farmer` — Boer ingelogd
	- `Sess: Admin` — Beheerder ingelogd
	- Als er een over-the-air firmware-update (OTA) bezig is, verschijnt `OTA` aan het eind van regel 2
 - Druk `#` op dit scherm om de **Stand-by-modus** aan of uit te zetten — de boer en de beheerder mogen dit allebei (zie [§10.4](#104-de-controller-tijdelijk-pauzeren--stand-by)). Als je nog niet ingelogd bent vraagt de controller eerst je PIN; voer dan je 4-cijferige Boer-PIN in en druk `#`.

**Scherm 4 — Wifi-status:**

Drie mogelijke weergaven:

```
   +----------------+      +----------------+      +----------------+
   |WiFi: connected |      |WiFi: AP active |      |WiFi: --------  |
   |192.168.1.100   |      |Greenhouse-XXXX |      |                |
   +----------------+      +----------------+      +----------------+
```

 - **Connected**: de kascontroller is verbonden met een wifi-netwerk; regel 2 toont het IP-adres
 - **AP active**: de tijdelijke Access Point staat aan; regel 2 toont de SSID `Greenhouse-XXXX` (waar `XXXX` de unieke ID van de kascontroller is — vier letters afgeleid van de chip; zie [§3.1](#31-unieke-id-van-de-kascontroller))
 - **Disconnected**: geen verbinding; druk `#` om de AP in te schakelen (vraagt Beheerder-PIN)

> **Sinds firmware 1.18.0** kan op regel 1 rechts de tekst `BK` verschijnen:
>
> ```
>    +----------------+
>    |WiFi: conn    BK|
>    |192.168.1.100   |
>    +----------------+
> ```
>
> `BK` (afkorting van *backoff*) betekent dat de kascontroller tijdelijk **gestopt** is met rapporteren naar het externe webdashboard, omdat de verbinding daarmee meerdere keren mislukte. **Dit is geen storing** — het klimaatregelsysteem blijft normaal werken (RGB-LED blijft groen), alleen de online status-rapportage staat pauze. De controller probeert het later automatisch opnieuw (vanzelf, na 60 sec → 5 min → 30 min → 1 uur, afhankelijk van hoe lang de uitval al duurt). Geen actie nodig van de boer; als `BK` permanent blijft staan, meld het bij de beheerder.

**Scherm 5 — Datum en tijd:**

```
   +----------------+
   |06-05-2026 14:30|
   |Src:NTP      Day|
   +----------------+
```

 - Regel 1 toont datum en tijd. 
 - Regel 2 toont links de tijdsbron (`NTP` = gesynchroniseerd via internet, of `RTC` = alleen interne klok) en rechts of de controller op dit moment in de **dag**- of **nacht**-zone zit (`Day` / `Night`). 
	- De omschakeling dag/nacht gebeurt automatisch op basis van zonsopkomst en zonsondergang voor de ingestelde locatie en is dezelfde grens die ook bepaalt welke dag- of nacht-setpoints actief zijn. 
 - Druk `#` op dit scherm om datum en tijd handmatig in te stellen (vraagt om Beheerder-PIN).

**Scherm 6 — Raamposities:**

```
   +----------------+
   |M1    M2    M3  |
   |OPEN  CLOS  MOV>|
   +----------------+
```

 - Rij 1 is een vaste kop. Rij 2 toont per raam de toestand:

	| Code | Betekenis |
	|---|---|
	| `OPEN` | Raam is volledig open |
	| `CLOS` | raam is volledig dicht |
	| `MOV>` | Raam wordt geopend |
	| `MOV<` | Raam wordt gesloten |
	| `UNK ` | Raamopening onbekend (treedt op kort na opstart vóór de kalibratie) |

 - Druk `#` op dit scherm om een raam handmatig open of dicht te zetten. Deze functie is **alleen voor de Beheerder** beschikbaar — als je als Boer op `#` drukt, vraagt de controller om de 8-cijferige Beheerder-PIN; je kunt met `*` annuleren en terug naar de auto-rotatie.

**Scherm 7 — Firmware versie, unit-ID en bedrijfsduur:**

```
   +----------------+
   |FW: 1.20.0  5C88|
   |Up: 1d 4h 23m   |
   +----------------+
```

 - Regel 1 toont links het firmware-versienummer (`FW: 1.20.0`) en rechts de **unieke ID van deze kascontroller** (in dit voorbeeld `5C88` — zie [§3.1](#31-unieke-id-van-de-kascontroller) voor de uitleg). Sinds firmware 1.20.0 staat de ID hier.
 - Regel 2 toont de bedrijfsduur sinds de laatste start.

Een onverwachte herstart valt op doordat Uptime bedrijfsduur naar `0 minuten` en daarna weer oploopt. Handig om te zien of de controller stabiel draait. Geef de beheerder het firmware-nummer als je een storing meldt — dat helpt bij diagnose.

### 5.2 Toetsenbord (4 × 4)

```
   +-----+-----+-----+-----+
   |  1  |  2  |  3  |  A  |
   +-----+-----+-----+-----+
   |  4  |  5  |  6  |  B  |
   +-----+-----+-----+-----+
   |  7  |  8  |  9  |  C  |
   +-----+-----+-----+-----+
   |  *  |  0  |  #  |  D  |
   +-----+-----+-----+-----+
```

De functie van een toets hangt af van het **scherm** waar je je bevindt. Hieronder een overzicht van de toesfunctie per situatie:

#### Op een statusscherm (auto-rotatie)

| Toets | Functie |
|:---:|:---|
| **D** | Volgende statusscherm (versneld door auto-rotatie schermen stappen) |
| **#** | Quick-jump naar een functie — afhankelijk van welk scherm wordt getoond (zie tabel hieronder) |
| **alle andere toetsen** | Open het hoofdmenu |

#### In een menu of bewerk-scherm

| Toets | Functie |
|:---:|:---|
| **D** | **Direct terug naar de auto-rotatie statusschermen** — werkt vanuit elk menu, bladermenu, PIN-invoer en bewerk-scherm. Eén druk en je staat weer op het roterende statusscherm. Handig om snel weg te komen als je per ongeluk in een menu bent beland. |

> **Automatisch terug naar de auto-rotatie**: blijft het LCD 5 minuten op een menu of invoerscherm staan zonder dat er een toets wordt ingedrukt, dan keert de controller automatisch terug naar de roterende statusschermen. Je hoeft dus niet bang te zijn dat de display vast blijft staan op een halve invoer.

De `#`-quick-jump werkt op zes statusschermen. Op het LCD zelf staat geen zichtbare hint; onthoud gewoon dat `#` op een statusscherm met instellingen direct het bijhorende menu opent:

| Scherm | `#` opent | Vraagt PIN? |
|---|---|---|
| 1 — Temperatuur/luchtvochtigheid | Klimaat-menu (setpoints) | Boer-PIN |
| 2 — Wind | Wind-menu (Wnd-max, Wnd-prot) | Boer-PIN |
| 3 — Mode/Sess | Stand-by-modus aan/uit (zie [§10.4](#104-de-controller-tijdelijk-pauzeren--stand-by)) | Boer- óf Beheerder-PIN |
| 4 — WiFi | System-menu (AP aan/uit) | Beheerder-PIN |
| 5 — Datum/tijd | Datum/tijd-invoer | Beheerder-PIN |
| 6 — Raamposities | Handmatige raambediening | Beheerder-PIN — niet voor de Boer |

Alleen op scherm 7 (Firmware/uptime) heeft `#` geen aparte functie; daar opent het, net als andere toetsen, het hoofdmenu.

#### In een menu (root, climate, wind, access, system)

| Toets | Functie |
|:---:|:---|
| **1, 2, 3, 4** | Selecteer de menu-optie met dat nummer |
| **\*** | Eén niveau terug |

#### In het bladermenu voor klimaat-setpoints (Day / Night / CR-priority)

| Toets | Functie |
|:---:|:---|
| **A** | Vorige setpoint (← op het scherm) — alleen Day/Night |
| **B** | Volgende setpoint (→ op het scherm) — alleen Day/Night |
| **#** | Bewerk de huidige setpoint (vraagt Farmer-PIN als nog niet ingelogd) |
| **\*** | Terug naar Climate-menu |

#### In de PIN-invoer

| Toets | Functie |
|:---:|:---|
| **0–9** | Voer cijfer in |
| **#** | Bevestig PIN |
| **\*** | Wis het laatst ingevoerd cijfer; bij lege invoer: annuleer je de invoer en ga terug naar het vorige scherm |

#### Bij het invoeren / bewerken van een waarde

| Toets | Functie |
|:---:|:---|
| **0–9** | Voer cijfer in |
| **B** | Plus/min-teken omdraaien (alleen bij waarden die negatief mogen zijn) |
| **#** | Bevestig en sla op |
| **\*** | Wis het laatst ingevoerd cijfer; bij lege invoer: annuleer je de invoer en ga terug naar het vorige scherm  |

### 5.3 LED-indicatoren

#### RGB-LED

Een meerkleurige LED is zichtbaar door de deksel van de kast. De kleur geeft de globale toestand van de kascontroller aan:

| Kleur | Betekenis |
|---|---|
| **Groen** | Normale werking — `Mode: AUTO`, geen waarschuwingen |
| **Oranje (amber)** | Waarschuwing — wind-override actief, sensor-fout, windbeveiliging staat uit, vochtregeling staat uit, of operator-Stand-by actief |
| **Rood** | Kritiek alarm — motor-noodstop (`Mode: ALARM`); de motoren worden niet meer aangestuurd |

De LED dimt 's nachts automatisch.

#### Heartbeat-LED

Een kleine groene LED die **1× per seconde** aan/uit knippert. Zolang deze knippert, draait de software normaal. **Knippert hij niet?** Dan is de controller bevroren of uitgeschakeld — voer een power-cycle uit (zie [§14](#14-onderhoud--wat-de-boer-zelf-doet)) of bel de beheerder.

---

## 6. De webinterface (via wifi)

Naast het LCD-scherm op de kast is de kascontroller ook bereikbaar via een **webinterface**: een webpagina die je kunt openen in de browser op een laptop, tablet of smartphone en die op hetzelfde wifi-netwerk zit als de controller. De webinterface biedt **meer overzicht en mogelijkheden** dan de bediening controller: live informatie, sensorhistorie en alle setpoints op één scherm.

![SCHERMAFBEELDING: hoofdpagina van de webinterface](images\kasControllerWebGUIHoofdpagina.png)

*Figuur 4: hoofdpagina van de webinterface*

### Voorwaarde

De beheerder moet de kascontroller eerst hebben verbonden met een wifi-netwerk. Zonder die instelling heeft je laptop of telefoon geen verbinding met de controller. De aparte AP-modus (Access Point) van de controller is een hulpmiddel voor de beheerder bij installatie of onderhoud, en hoef je als boer niet zelf te gebruiken.

### Bereiken van de webinterface

1. Meld je aan op hetzelfde WiFi netwerk als waar de kascontroller op is aangemeld.
2. Lees het IP-adres af van het LCD-scherm (auto-rotatie, of door klikken met de D-toets, laat het WiFi-scherm vanzelf zien — daar staat **SSID + IP-adres**)
3. Open een browser (Chrome, Firefox, Edge, Safari) op een apparaat dat op hetzelfde wifi-netwerk zit
4. Typ het IP-adres in de adresbalk, bijvoorbeeld `http://192.168.1.100`
5. De **Status**-pagina opent direct, zonder dat je hoeft in te loggen

### Hoofdtabs

| Tab | Wie ziet het? | Wat staat er? |
|---|---|---|
| **Status** | Iedereen (zonder inloggen) | temperatuur, luchtvochtigheid, wind, raamposities, mode, alarmen, klok, wifi, SD-kaart |
| **Climate** | Boer + Beheerder | Setpoints voor dag en nacht, vochtregeling aan/uit, conflict-prioriteit |
| **Wind** | Boer + Beheerder | Windbeveiliging aan/uit; windgrenzen en windgemiddelde window instellen is alleen voor de Beheerder |


### Sensorhistorie

Een lijst van de meetwaarden over de afgelopen periode met de laatst gemeten waarde bovenaan de lijst. Toegankelijk **zonder login**. De tabel ververst zichzelf elke ~2 minuten.

### Sessie

Wanneer je bent ingelogd en 5 minuten lang geen actie onderneemt, word je automatisch uitgelogd. Je moet dan opnieuw je PIN invoeren om wijzigingen door te voeren.

---

## 7. De twee gebruikersrollen

De kascontroller kent twee gebruikersrollen, elk met een eigen PIN-code:

### Farmer (boer / kasgebruiker)

- **PIN**: 4 cijfers
  - Bij eerste levering staat deze op fabrieksstandaard `1234`
  - **Wijzig deze direct na ingebruikname** — laat hem niet op de fabrieksstandaard staan. PIN-wijziging gaan alleen via de **webinterface** (Access-tab)en kan niet op de controller zelf.
- **Mag op de kas controller (LCD-menu)**:
  - Klimaat-setpoints instellen — T-max en RH-min/max voor dag en nacht
  - Conflict-prioriteit kiezen (Temperatuur eerst / Luchtvochtigheid eerst / Automatisch)
  - Windbeveiliging (wind protection) aan- of uitzetten en windgrens (Wnd-max) aanpassen — **deze actie wordt gelogd**
- **Mag aanvullend in de webinterface**:
  - Vochtregeling (humidity control) aan- of uitzetten

> **Opmerking:** De PIN-code van de boer kan alleen door de beheerder worden ingesteld. 

### Admin / Technisch beheerder

- **PIN**: 8 cijfers (door de beheerder ingesteld)
- **Mag**:
  - Alle systeeminstellingen wijzigen
  - Wifi configureren (AP en client mode)
  - Motor-tijden en sensor-poll-intervallen aanpassen
  - Geografische locatie instellen
  - PIN-codes van de boer en zichzelf wijzigen

Voor jou als boer relevant: bel de beheerder als motor-instellingen, wifi of sensoren niet kloppen. Probeer niet zelf in admin-instellingen te duiken.

### Lockout

Als je 5 keer achter elkaar een verkeerde PIN invoert, wordt de invoer voor die rol **5 minuten geblokkeerd**. Dit geldt zowel op de bediening op de controller als in de webinterface, en beide rollen hebben hun eigen lockout-teller.

### PIN-opslag

PIN's worden versleuteld opgeslagen (gehasht). Ze kunnen niet worden teruggelezen — alleen vervangen. Als je je PIN bent vergeten:
- **Farmer-PIN vergeten**: de beheerder kan deze openieu instellen via het Beheerder-menu
- **Admin-PIN vergeten**: gebruik de fysieke reset-procedure (zie [§18](#18-reset-procedure-boot-knop-op-microprocessorboard))

---

## 8. Gebruik zonder inloggen — informatiemenu

Zonder in te loggen kun je alle **statusinformatie** van het systeem aflezen. Je kunt geen instellingen wijzigen.

### Op de controller

Wanneer er geen gebruiker is ingelogd, rouleren de schermen op het LCD-display automatisch elke 5 seconden in deze volgorde (zeven schermen, daarna weer scherm 1):

1. **Temperatuur en luchtvochtigheid** — Temperatuur `Temp:` en luchtvochtigheid `RH:`
2. **Wind** — windsnelheid (`Wind:`) en windrichting (`Dir:` met kompasletter)
3. **Bedrijfsmodus en sessie** — `Mode:` en `Sess:`
4. **Wifi-status** — `WiFi:` en SSID of IP-adres
5. **Datum en tijd** — datum en tijd, met tijdsbron (`NTP` of `RTC`); of het dag `Day` of nacht `Night` is
6. **Raamposities** — Staus van de ramen `M1`, `M2`, `M3` met `OPEN` / `CLOS` / `MOV>` / `MOV<`
7. **Systeem status** - Firmware `FW` unit-ID, en uptime `Up`.

Volledige beschrijving van elk scherm staat in [§5.1](#51-lcd-display-16--2-tekens).

**Handmatig navigeren** (zonder inloggen):
- `D` — direct naar het volgende statusscherm
- Op het temperatuur-/luchtvochtigheid-scherm (1): `#` opent het Climate-menu (vraagt Boer-PIN)
- Op het wind-scherm (2): `#` opent het Wind-menu (vraagt Boer-PIN)
- Op het wifi-scherm (4): `#` opent de wifi-AP-functie (vraagt Beheerder-PIN)
- Op het tijdscherm (5): `#` opent de datum-/tijd-instelling (vraagt Beheerder-PIN)
- Elke andere toets opent het hoofdmenu

### In de webinterface

De **Status**-tab is direct zichtbaar in de browser, zonder inloggen. Hier zie je dezelfde informatie als op de LCD, maar dan overzichtelijk gepresenteerd, plus een tabel met de metingen van de afgelopen tijd.

### Wat zichtbaar na login?

- Setpoints wijzigen
- Windbeveiliging aan/uit zetten
- Vochtregeling toggelen
- PIN's wijzigen
- Wifi-instellingen
- Motor-tijden

Voor al deze handelingen moet je inloggen als Boer of Beheerder.

---

## 9. Inloggen als boer

### Op de controller

Inloggen gaat via het hoofdmenu. Je kunt direct vanuit elk statusscherm naar het hoofdmenu door bijvoorbeeld op een cijfertoets te drukken (de toets `D` werkt niet — die stapt door de statusschermen).

1. Druk vanaf elk statusscherm een willekeurige toets behalve `D` — bijvoorbeeld `1`, `2`, `A` of `B` — om het hoofdmenu te openen:

```
   +----------------+
   |1:Clim  2:Wind  |
   |3:Access 4:Sys *|
   +----------------+
```

2. Druk `3` om het Access-menu te openen:

```
   +----------------+
   |1:Farmer 2:Admin|
   |          *:Back|
   +----------------+
```

3. Druk `1` om als boer `Farmer` in te loggen. Het PIN-invoerscherm verschijnt:

```
   +----------------+
   |PIN (4 dig) *=Bk|
   |____            |
   +----------------+
```

4. Voer je **4-cijferige Farmer-PIN** in via de cijfertoetsen `0`–`9`. De ingevoerde cijfers verschijnen als `*` op het scherm
5. Druk `#` om te bevestigen
6. **Bij correcte PIN**: het scherm toont kort `Access granted` / `Welcome!`, daarna verandert `Sess: NONE` op het modus-scherm in `Sess: Farmer`
7. **Bij foute PIN**: melding `Wrong PIN!` / `Try again` — probeer opnieuw
8. **Bij niet-volledige invoer**: melding `Need all digits` / `then press #`
9. **Na 5 mislukte pogingen**: melding `Locked out!` / `Try in <n> s` — invoer is 5 minuten geblokkeerd

**Backspace en annuleren in het PIN-scherm**:
- `*` — wist het laatst ingevoerde cijfer
- `*` met lege invoer — annuleer en ga terug naar het Access-menu

> **Tip — quick-jump naar setpoint**: je kunt ook direct in het Climate-menu beginnen (zie [§10](#10-klimaat-instellen)). Wanneer je een setpoint probeert te bewerken zonder ingelogd te zijn, vraagt het systeem dan vanzelf om je PIN en zet je daarna meteen in de bewerk-modus.

### In de webinterface

1. Open de webinterface (zie [§6](#6-de-webinterface-via-wifi))
2. Klik op de Login-knop
3. Klik op **Farmer**
4. Voer je 4-cijferige PIN in
5. Klik **Login**

### Uitloggen

- **LCD**: hoofdmenu → `3:Access` → `3:Logout`. Het bericht `Logged out` verschijnt en de controller keert terug naar de statusschermen
- **Webinterface**: klik op **Logout** knop rechts boven
- **Automatisch**: na de ingestelde sessie-time-out (standaard ~5 min) zonder activiteit word je uitgelogd

---

## 10. Klimaat instellen

Allereerst: log in als Boer (zie [§9](#9-inloggen-als-boer)).

### 10.1 Op de kas controller

Het LCD-menu volgt een eenvoudig pad: **hoofdmenu → Climate → Day of Night → blader door setpoints → bewerk waarde**. Inloggen kan vooraf (zie [§9](#9-inloggen-als-boer)) of komt automatisch op het moment dat je `#` indrukt om te bewerken.

#### Stap 1 — Open het hoofdmenu

Druk vanaf elk statusscherm een willekeurige toets (behalve `D`) om het hoofdmenu te openen:

```
   +----------------+
   |1:Clim  2:Wind  |
   |3:Access 4:Sys *|
   +----------------+
```

#### Stap 2 — Open het Climate-menu

Druk `1`:

```
   +----------------+
   |Climate menu    |
   |1Day 2Ngt 3CR  *|
   +----------------+
```

- `1` — bewerk Day-setpoints (3 setpoints voor de dag)
- `2` — bewerk Night-setpoints (3 setpoints voor de nacht)
- `3` — bewerk Conflict-prioriteit (T/RH-prio)
- `*` — terug naar hoofdmenu

#### Stap 3 — Blader door de setpoints (Day of Night)

Na `1` (Day) of `2` (Night) verschijnt een bladerscherm:

```
   +----------------+
   |T-max day (C)   |
   |25  1/3 ←A→B↩#^*|
   +----------------+
```

Rij 1 toont de naam van de huidige setpoint. Rij 2 toont:
- de huidige waarde (hier `25`)
- de positie in de groep (`1/3`)
- toetshints: `←A` (vorige), `→B` (volgende), `↩#` (bewerk), `^*` (terug)

**Setpoints in de dag-groep (Day):**

| Volgorde | Naam op LCD | Wat regelt het? | Bereik |
|:---:|---|---|---|
| 1/3 | `T-max day (C)` | Maximum dagtemperatuur — boven deze waarde gaan ramen open | 15–45 °C |
| 2/3 | `RH-max day (%)` | Maximum dagvochtigheid — boven deze waarde gaan ramen open | 40–98 % |
| 3/3 | `RH-min day (%)` | Minimum dagvochtigheid — onder deze waarde blijven ramen dicht | 20–90 % |

**Setpoints in de Nacht-groep (Night):**

| Volgorde | Naam op LCD | Wat regelt het? | Bereik |
|:---:|---|---|---|
| 1/3 | `T-max ngt (C)` | Maximum nachttemperatuur — boven deze waarde gaan ramen open | 10–35 °C |
| 2/3 | `RH-max ngt (%)` | Maximum nachtvochtigheid — boven deze waarde gaan ramen open | 40–98 % |
| 3/3 | `RH-min ngt (%)` | Minimum nachtvochtigheid — onder deze waarde blijven ramen dicht | 20–90 % |

#### Stap 4 — Bewerk een setpoint

Druk `#` op het bladerscherm om de huidige setpoint te bewerken. Als je nog niet ingelogd was als Boer, vraagt de controller eerst je PIN; daarna ga je automatisch naar het bewerk-scherm.

```
   +----------------+
   |T-max day (C)   |
   |_____           |
   +----------------+
```

- Voer de gewenste waarde in via `0`–`9`. Maximaal 5 cijfers
- `*` wist het laatst ingevoerde cijfer
- `*` zonder cijfers — annuleer en ga terug
- `#` — bevestig en sla op

**Bij opslaan**:
- Aanpassing is succesvol: melding `Saved: <nieuwe waarde>`
- Waarde is deelfde: melding `No change`
- Waarde ligt buiten bereik: de waarde wordt automatisch geknepen tot het toegestane minimum of maximum

#### Conflict-prioriteit (Climate-menu, optie 3)

Druk in het Climate-menu op `3` om de prioriteit aan te passen. Het bewerkscherm toont:

```
   +----------------+
   |T/RH prio (0-2) |
   |_               |
   +----------------+
```

| Waarde | Betekenis |
|:---:|---|
| `0` | Temperatuur eerst — temperatuur krijgt voorrang |
| `1` | luchtvochtigheid eerst — luchtvochtigheid krijgt voorrang |
| `2` | Auto — de regeling kijkt naar welke afwijking het grootst is en kiest die |

Voer 0, 1 of 2 in en bevestig met `#`.

#### Wind-instellingen (hoofdmenu → 2 Wind)

Vanuit het hoofdmenu druk je `2` om in het Wind-menu te komen:

```
   +----------------+
   |1 Wnd-max     12|
   |2 Wnd-prot     1|
   +----------------+
```

- `1` — bewerk **Wind max** (maximum windsnelheid in m/s, 1–30); boven deze waarde sluiten alle ramen automatisch
- `2` — bewerk **Wind protection** (`0` = uit, `1` = aan)
- `*` — terug naar hoofdmenu

> **Waarschuwing**: zet `Wnd-prot` alleen op `0` als je een goede reden hebt. Bij harde wind kunnen open ramen, motoren of de raamconstructie beschadigd raken. Iedere wijziging van de windbeveiliging wordt door de controller gelogd.

#### PIN wijzigen op de kas controller — niet beschikbaar

Er is op de kascontroller geen menu om je PIN te wijzigen. Je PIN wijzigen kan alleen via de **webinterface** (Access-tab) door de Beheerder. Bij verlies van een PIN-code: zie de fysieke reset-procedure in [§18](#18-reset-procedure-boot-knop-op-microprocessorboard).

#### Time-out

Wanneer je in een menu of bewerk-scherm de ingestelde sessie-time-out (standaard ~5 min) lang geen toets indrukt, word je automatisch uitgelogd en keer je terug naar de statusschermen.

### 10.2 In de webinterface (tab Climate)

![SCHERMAFBEELDING: tab Climate met sliders, velden en de keuzelijst voor T vs RH conflict-prioriteit](.\images\kasControllerWebGUIClimateTab.png)

*Figuur 5: Climate tab met sliders, velden en de keuzelijst voor T vs RH conflict-prioriteit*

Bovenaan de tab staat een aparte "Mode"-keuzelijst met **Normal** (Automatisch) of **Stand-by** (Pauze). Hiermee zet je de automatische klimaatregeling tijdelijk uit zonder andere instellingen aan te raken — handig tijdens onderhoud of wanneer je zelf de ramen wilt openen of sluiten. Lees [§10.4](#104-de-controller-tijdelijk-pauzeren--stand-by) voor wat de Standby-modus precies doet en wanneer je hem gebruikt. De keuzelijst is grijs gemaakt als wind-override, motor-alarm of kalibratie actief is — de controller laat veiligheidsregels altijd voorgaan op een operator-pauze.

Per setpoint (daaronder) heb je een schuifregelaar + nummerveld + **Apply**-knop.

| Veld op de webinterface | Betekenis |
|---|---|
| T max day | Maximum dagtemperatuur — boven deze waarde gaan ramen open |
| T min day | Minimum dagtemperatuur — beneden deze waarde sluiten ramen om warmte vast te houden |
| RH max day | Maximum dagvochtigheid |
| RH min day | Minimum dagvochtigheid |
| T max night | Maximum nachttemperatuur |
| T min night | Minimum nachttemperatuur — beneden deze waarde sluiten ramen |
| RH max night | Maximum nachtvochtigheid |
| RH min night | Minimum nachtvochtigheid |
| **T vs RH conflict priority** | Keuzelijst met drie opties: *Temperature first* (default), *Humidity first*, *Largest deviation* — bepaalt welke regelactie voorrang krijgt als T en RH tegelijk om actie vragen |

Bij elk veld vind je een tooltip (mouse-over) met uitleg over wat het veld doet.

> **Tip**: na het wijzigen van een waarde moet je op **Apply** klikken om hem op te slaan. Anders wordt de wijziging genegeerd.

### 10.3 Wat zijn goede setpoints?

Setpoints zijn afhankelijk van de teelt in de kas. Deze setpoints moeten door de boer wordt opgegeven.

Algemene vuistregels:
- **Nacht-T** mag iets lager zijn dan dag-T (planten besparen energie 's nachts)
- **RH boven 85%** voor langere tijd vergroot het risico op schimmelziekten — houd RH-max liever onder 85%
- **RH onder 50%** kan groei remmen en mijten in de hand werken
- **Bij twijfel**: zie [Bijlage B — Aanbevolen startinstellingen per gewas](#20-bijlage-b--aanbevolen-startinstellingen-per-gewas) voor een tabel met richtwaarden voor de meest voorkomende gewassen, of vraag je teler / leverancier van de planten om aanbevolen klimaatzones

### 10.4 De controller tijdelijk pauzeren — Stand-by

Stand-by zet de **automatische klimaatregeling tijdelijk uit** zonder dat je instellingen hoeft te wijzigen. De ramen blijven staan waar ze stonden op het moment dat je Stand-by aanzet — de controller stuurt simpelweg geen nieuwe open- of dicht-commando's meer.

#### Wanneer gebruik je Stand-by?

- Tijdens **onderhoud aan de motoren of de ramen** — voorkomt dat de controller midden in jouw werk een raam in beweging zet
- Tijdens **een rondleiding of demonstratie** waarbij de ramen tijdelijk in een bepaalde stand moeten blijven
- Wanneer je **even zelf de ramen wilt bedienen** via de Hand-schakelaars op de motorbox (de controller probeert dan niet steeds tegen je in te werken)
- **Tijdelijk uitschakelen** wanneer je weet dat de buitenomstandigheden bijzonder zijn (een schoonmaakdag waarbij de ramen open moeten staan, een korte test, …)

#### Wat doet Stand-by NIET?

- Stand-by **schakelt de veiligheidsregels niet uit**. Bij harde wind sluit de controller alle ramen alsnog automatisch (`Mode: WIND` blijft voorrang krijgen). Bij motor-alarm (`Mode: ALARM`) blijft de motor-noodstop actief.
- Stand-by **doet de ramen niet automatisch dicht** wanneer je hem aanzet. Wil je dat alles dicht is voor onderhoud, doe dat dan eerst handmatig op de motorbox.

#### Stand-by aanzetten — drie manieren

**Via het LCD-scherm:**
1. Druk `D` tot je op **scherm 3** (`Mode/Sess`) bent
2. Druk `#`
3. Als je nog niet ingelogd bent: voer je 4-cijferige Boer-PIN in, druk `#`
4. Het menu toont `Now:AUTO` (of `STANDBY`) en de keuzes `1=Auto 2=Stby *B`
5. Druk `2` om naar Stand-by te schakelen — bevestiging `Mode: STANDBY`, controller pauzeert. Het LCD springt terug naar de auto-rotatie en scherm 3 toont `Mode: STANDBY`

**Via de webinterface (Climate-tab):**
1. Log in als Boer
2. Open tab **Climate**
3. Bovenaan staat een "Mode"-keuzelijst — kies **Standby (paused)**
4. Klik **Apply** — binnen een paar seconden verschijnt op scherm 3 `Mode: STANDBY`

#### Stand-by uitzetten

Dezelfde route, maar kies **Auto** (LCD: toets `1` op het menu, web: kies "Normal" en klik Apply).

**Belangrijk — kort kalibratie-moment bij uitschakelen:** Zodra je Stand-by verlaat, voert de controller automatisch een **kalibratiecyclus** uit: alle drie de ramen sluiten zich gelijktijdig om een betrouwbare uitgangspositie te bepalen. Dit duurt **tot ~3 minuten** (zo lang als M3 nodig heeft om volledig dicht te zijn) en je ziet `Mode: Window Cal.` op scherm 3. Daarna gaat de controller automatisch verder met `Mode: AUTO` en past hij de ramen aan op basis van de actuele klimaatwaarden.

> **Stand-by blijft staan bij stroomuitval.** Heb je Stand-by aangezet en gaat tussendoor de stroom uit? Na opstarten staat de controller weer in `Mode: STANDBY` — zodat een korte stroomonderbreking je onderhoudswerk niet onbedoeld onderbreekt. Vergeet niet om Stand-by weer uit te zetten als je klaar bent met je werk!

#### Visuele bevestiging dat Stand-by actief is

- **Op de controller (LCD)**: scherm 3 toont `Mode: STANDBY`
- **In de webinterface**:
  - Op de **Status**-pagina verschijnt een gele **Stand-by**-badge in de Alarms-tegel, naast de andere actieve waarschuwingen
  - Op de **Climate**-tab toont de Mode-keuzelijst `Standby (paused)` en de Apply-knop is grijs (geen wijziging mogelijk tot je via dezelfde keuzelijst weer naar Normal stelt)
  - Tijdens de korte kalibratiecyclus die direct ná het uitzetten van Stand-by loopt, toont de Mode-keuzelijst de transient tekst `Calibrating windows...` — wacht ~3 min, daarna keert hij vanzelf terug naar `Normal (autonomous)`

---

## 11. Wifi en webinterface gebruiken

**De beheerder regelt de wifi-configuratie van de kascontroller.** Zowel het tijdelijke Access Point (AP-modus, dat alleen voor onderhoud aangezet wordt) als de verbinding met het kas-/thuisnetwerk (client-modus) zijn taken van de beheerder. **Als boer hoef je hier niets aan in te stellen.**

### Wat doe jij als boer?

Zodra de beheerder de controller met het wifi-netwerk heeft verbonden, kun jij de webinterface gebruiken:

1. **Lees het IP-adres af van de LCD** (auto-rotatie toont het WiFi-scherm vanzelf — daar staan SSID en IP)
2. Open op een laptop, tablet of smartphone die op **hetzelfde wifi-netwerk** zit een browser
3. Typ het IP-adres in (bijvoorbeeld `http://192.168.1.100`)
4. De Status-tab is direct zichtbaar zonder login
5. Voor klimaat-instellingen: klik **Farmer**, voer je 4-cijferige PIN in

### Tips

- **Bookmark het IP-adres** in je browser, zodat je niet steeds op de LCD hoeft te kijken
- Als je een vast (statisch) IP-adres wenst — zodat het IP nooit verandert — vraag de beheerder om er één te configureren

### Geen wifi-verbinding meer?

De LCD geeft op het WiFi-scherm aan of er verbinding is. Als de melding `DISCONNECTED` of `Connecting...` blijft staan:

1. Controleer eerst je eigen wifi-router — werkt internet thuis nog?
2. Werkt de router en is het wifi-netwerk gewoon aanwezig, maar krijgt de controller toch geen verbinding? **Bel de beheerder.**

### IP-adres veranderd

Het IP-adres kan veranderen als de wifi-router opnieuw is opgestart of als de controller een nieuw IP toegewezen krijgt. Kijk in dat geval opnieuw op de LCD WiFi-pagina voor het actuele adres.

### Korte automatische herstart na wifi-wijziging door de beheerder

De kascontroller herstart **automatisch** wanneer de beheerder via de webinterface de wifi-instellingen wijzigt (nieuwe SSID, nieuwe WiFi-wachtwoord, of nieuw AP-wachtwoord). Je ziet:

- Het LCD springt kortstondig naar de boot-rotatie en daarna weer naar `Mode: AUTO` (totale onderbreking ~2 sec voor de klimaatregeling, dankzij de kalibratie-overslaan-logica — zie [§13](#13-inschakelen-na-stroomuitval)).
- De webinterface zou een herlaad-melding kunnen tonen.

**Dit is normaal en zo bedoeld**: de oude wifi-configuratie blijft anders actief tot de volgende fysieke power-cycle. Geen actie nodig.

---

## 12. Alarmen en bedrijfsmodi — wat betekenen ze, wat te doen

Dit hoofdstuk legt uit wat de mode-regel op het LCD betekent en wat je in elke situatie doet.

### 12.1 Bedrijfsmodi (Mode-regel op LCD)

| LCD-tekst | Betekenis | Wat doet de controller? | Wat moet je doen? |
|---|---|---|---|
| `Mode: AUTO` | Normale automatische werking | Regelt Temperatuur en Luchtvochtigheid binnen de setpoints | Niets — alles werkt zoals het hoort |
| `Mode: STANDBY` | Stand-by-modus — door operator gepauzeerd | **Geen klimaatcommando's; ramen blijven waar ze zijn** | Niets — dit is een bewuste pauze (zie [§10.4](#104-de-controller-tijdelijk-pauzeren--stand-by)). Vergeet niet om Stand-by uit te zetten als je klaar bent. |
| `Mode: WIND` | Wind-override actief — wind te hard | **Alle ramen dicht; klimaatregeling onderdrukt** | Wachten tot de wind afneemt; zie [§12.5](#125-windbeveiliging-in-detail) |
| `Mode: ALARM` | Motor-alarm (Hotraco RRK-3) | **Alle relais uit; motoren staan stil** | **Bel de beheerder onmiddellijk**; zie [§12.6](#126-motor-alarm-in-detail) |
| `Mode:Window Cal.` | Kalibratie van de ramen — alle ramen sluiten om de uitgangspositie te bepalen | Sluit M1, M2, M3 gelijktijdig; duurt tot ~3 minuten | Wachten; niet ingrijpen, niet handmatig aan de ramen werken |

### 12.2 Sensor- en status-indicaties

| LCD-tekst | Betekenis | Wat moet je doen? |
|---|---|---|
| `** SENSOR FAULT` (regel 2) | T/RH-sensor reageert niet | **Bel de beheerder**; controleer kort of de sensorkabel zichtbaar beschadigd is of of de sensor losgekoppeld lijkt |
| Geen wind-meting / `--` op windscherm | Wind-sensor reageert niet | Bel de beheerder; controleer of de wind-sensor buiten niet bedekt is door bladeren of beschadigd lijkt |

> **Web-badge "Update pending".** Ziet u in de webinterface (kaartje *Alarms*) een blauwe badge **Update pending**? Dan heeft de beheerder automatische internet-updates (ROTA) aangezet en staat er een nieuwe firmwareversie klaar. Die installeert zichzelf 's nachts automatisch — **u hoeft niets te doen**. De kascontroller herstart daarbij kort, net als bij een gewone update. Het is dus géén storing.

### 12.3 RGB-LED kleuren samengevat

| LED-kleur | Betekenis |
|---|---|
| **Groen** | Alles in orde, `Mode: AUTO` |
| **Oranje** | Waarschuwing — wind-override actief, sensor-fout, of windbeveiliging staat uit |
| **Rood** | Kritiek — motor-alarm; ramen worden niet meer aangestuurd |

### 12.4 Tijdens kalibratie

Na een opstart kan de controller een **CLOSE_ALL kalibratie** uitvoeren: alle drie de ramen worden gelijktijdig gesloten zodat de kascontroller weet wat de uitgangspositie is. Tijdens deze procedure staat de mode op `Mode:Window Cal.` Daarna gaat de controller automatisch naar `Mode: AUTO`.

| Raam | Tijd om dicht te zijn |
|---|---|
| M1 (dak zuid) | ~26 sec. |
| M2 (dak noord) | ~26 sec. |
| M3 (zijwand noord) | ~176 sec. (≈ 3 min.) |

**Wanneer wordt de kalibratie wél uitgevoerd?**

- Bij élke opstart waarin de ramen vóór de uitschakeling **niet allemaal dicht** waren (bv. M3 stond open op het moment van stroomuitval, OTA-update of een geplande herstart van de controller).
- Na een **motor-alarm-clearance** (60 sec. wachttijd + ~3 min kalibratie — zie §12.6).
- Na een **fabrieksreset** (BOOT-knop): omdat het permanente geheugen leeg is, geldt de raampositie als "onbekend" en wordt altijd gekalibreerd.

**Wanneer wordt de kalibratie overgeslagen?**

- Als bij de uitschakeling **alle drie de ramen volledig dicht stonden** én de motor-controller geen alarm meldt, slaat de controller de kalibratie over. Hersteltijd dan: ~2 sec. in plaats van ~3 min. Op het LCD verschijnt niet `Mode:Window Cal.` maar direct `Mode: AUTO`.
- Als bij de opstart al een motor-alarm actief is op de motorbox, slaat de controller de kalibratie eveneens over en gaat hij direct naar `Mode: ALARM`. In dat geval: bel de beheerder.

**Wat betekent dit in de praktijk?**

Of een opstart wel of geen kalibratie krijgt hangt **niet** af van wat de oorzaak van de opstart was (power-cycle, OTA-update, fabrieksreset, etc.), maar uitsluitend van **de raamposities op het moment van uitschakeling**. Een power-cycle 's avonds met alle ramen dicht slaat de kalibratie over; een power-cycle midden op de dag terwijl M3 open staat voert de volledige kalibratie uit.

### 12.5 Windbeveiliging in detail

De windbeveiliging beschermt de kas tegen schade door wind. Dit gedeelte beschrijft hoe deze precies werkt.

#### Wat triggert het wind-alarm?

Bij elke sensor-cyclus controleert de kascontroller drie zaken:

1. **Gemiddelde windsnelheid** — ligt deze op of boven de ingestelde grens **`Wnd-max`** (in m/s)?
2. **Windrichting** — valt de gemiddelde windrichting binnen een eventueel door de Beheerder ingestelde **uitsluitings-zone** (een openingshoek waarin de wind extra gevaarlijk is, bijvoorbeeld omdat ramen er direct op staan.
3. **Wind-sensor storing** — levert de wind-sensor geen geldige meting meer? In dat geval gaat de controller "veilig falen" en gedraagt zich alsof het hard waait.

Als één of meer van deze drie waar zijn, gaat de controller in **wind-override** modus.

#### Wat kan de boer instellen?

| Instelling | Op LCD | In webinterface | Wie? |
|---|---|---|---|
| Windbeveiliging aan/uit | Wind-menu, item 2 (`Wnd-prot`) | Wind-tab | Boer |
| Windgrens in m/s | Wind-menu, item 1 (`Wnd-max`) | Wind-tab | Boer |

Het **gemiddeld windvenster** bepaalt over hoeveel minuten de windmetingen worden gemiddeld voordat ze met de grens worden vergeleken. Een langer venster reageert minder snel op een rukwind maar wel betrouwbaarder op aanhoudend stevige wind. Dit venster is onafhankelijk van het temperatuurvenster en wordt uitsluitend door de beheerder ingesteld.

#### Wat gebeurt er als wind-alarm actief wordt?

1. **Mode-regel** op de LCD verandert naar `Mode: WIND`l het LCD kleurt *Rood* 
2. **RGB-LED** wordt oranje; LCD-achtergrond wordt rood
3. **Alle drie de ramen worden onmiddellijk gesloten** — gelijktijdig, zonder vertraging
4. **Klimaatregeling wordt onderdrukt** — de controller stopt met afwegen of de ramen open zouden moeten op basis van temperatuur of vochtigheid, zolang het alarm actief is
5. Eventuele lopende open-acties worden afgebroken; de ramen sluiten met voorrang
6. De gebeurtenis wordt gelogd

#### Wanneer valt het wind-alarm af?

Het alarm valt **direct** af zodra **alle** onderstaande voorwaarden tegelijk waar zijn:

- De gemiddelde windsnelheid is **onder** de grens `Wnd-max`
- De gemiddelde windrichting valt **buiten** de uitsluitings-zone (indien ingesteld)
- De wind-sensor levert weer geldige metingen

Er is **geen extra wachttijd of hysteresis** — zodra de wind weer binnen de grenzen is, wordt het alarm onmiddellijk gewist en gaat de mode terug naar `Mode: AUTO`. De ramen blijven dicht; de klimaatregeling beslist daarna zelf op basis van temperatuur en Luchtvochtigheid of er weer geopend moet worden.

> **Praktische tip**: doordat er geen hysteresis is, kan bij onstabiel weer (windvlagen rond de grens) het alarm meermaals snel achter elkaar in- en uitgaan. Een **langer gemiddeld windvenster** (door de Beheerder in te stellen) dempt dit, omdat korte rukwinden dan minder snel de gemiddelde meetwaarde over de grens duwen.

#### Bij een wind-sensor storing

Als de wind-sensor geen geldige meting meer levert (twee opeenvolgende mislukte uitlezingen via Modbus), kiest de controller voor **veilig falen**: hij gedraagt zich alsof het hard waait, zet wind-override actief, sluit alle ramen, en houdt deze toestand vast totdat de sensor weer reageert.

> **Bel de beheerder bij een wind-sensor storing.** Zolang de fout niet is verholpen, blijven de ramen dicht en kan de klimaatregeling niet werken.

#### Windbeveiliging uitschakelen — risico's

Als je `Wnd-prot` op `0` (uit) zet:
- Hoge wind heeft **geen effect** meer op de ramen
- Een eventueel actief wind-alarm wordt onmiddellijk gewist
- Ramen kunnen open blijven of openen op basis van klimaatregeling, ongeacht de windkracht
- Een wind-sensor storing leidt niet meer tot automatisch sluiten

Iedere wijziging van `Wnd-prot` wordt gelogd. **Zet windbeveiliging alleen uit als je een hele goede reden hebt** — bijvoorbeeld voor een gerichte controle bij rustig weer — en zet hem **direct na de actie** weer aan.

### 12.6 Motor-alarm in detail

De **Hotraco RRK-3 motorbox** heeft een eigen alarm-uitgang die de kascontroller waarschuwt zodra een raam-motor in storing is gekomen (vastloper, overbelasting, ingedrukte noodstop, eindschakelaar-fout, etc.). Dit is een **hardware-mechanisme**: de RRK-3 beslist zelfstandig wanneer hij alarm geeft, los van de software in de kascontroller.

#### Wat triggert het motor-alarm?

De alarm-uitgang van de RRK-3 is met de kascontroller verbonden. Zodra de RRK-3 zijn alarm-contact sluit:

- De kascontroller detecteert dit binnen ~75 milliseconden
- `Mode: ALARM` verschijnt op het LCD
- De RGB-LED gaat **rood**, het LCD-scherm kleurt rood

Mogelijke oorzaken — bepaald door de RRK-3 zelf:
- Een motor blijft te lang draaien zonder dat de eindschakelaar wordt bereikt (vastloper of eindschakelaar-fout)
- Een motor heeft te veel stroom getrokken (overbelasting)
- Een externe noodstop is ingedrukt
- De RRK-3 heeft een interne fout

#### Wat gebeurt er als motor-alarm actief wordt?

1. **Mode-regel** verandert naar `Mode: ALARM`
2. **RGB-LED** en **LCD-scherm** wordt rood
3. **Alle relais naar de motoren worden onmiddellijk uitgeschakeld** — alle drie de motoren stoppen direct met bewegen
4. **De raamposities worden als "onbekend" gemarkeerd** (`UNK` op het raamposities-scherm) — na een noodstop weet de controller niet meer in welke positie de ramen staan
5. **Klimaatregeling wordt onderdrukt** — geen evaluatie van temperatuur en Luchtvochtigheid zolang het alarm actief is
6. **Alle nieuwe commando's worden genegeerd** — zelfs als je in de webinterface op iets klikt of een setpoint wijzigt, wordt er niets met de ramen gedaan zolang het alarm actief is
7. De gebeurtenis wordt gelogd

#### Wanneer valt het motor-alarm af?

Stap voor stap:

1. De **beheerder lost de oorzaak op** en reset de RRK-3 (handmatig, op de motorbox zelf — een externe reset-procedure die buiten de kascontroller om gaat)
2. De alarm-uitgang van de RRK-3 valt af; de kascontroller detecteert dit
3. **Het alarm in de kascontroller wordt gewist** — `Mode: ALARM` zou direct kunnen verdwijnen, maar:
4. er volgt een **60 seconden veiligheids-wachttijd** ("guard period"): de controller wacht **één volle minuut** zonder iets met de ramen te doen. Reden: een motor die net gestopt is na een noodstop kan nog enige tijd uitlopen of nog onder spanning staan. Direct opnieuw aansturen zou schade veroorzaken. Ook geeft het de een Beheerder de tijd om rekening te houden met de aanstaande kalibratie van de ramen
5. Tijdens deze 60 seconden controleert de controller elke 5 seconden of het alarm misschien terugkomt. Zo ja → onmiddellijk terug naar `Mode: ALARM`, en de hele procedure begint van voren af aan
6. Na 60 seconden stabiele veiligheid start de controller automatisch een **CLOSE_ALL re-kalibratie** (~3 minuten — `Mode:Window Cal.`) om alle ramen weer in een bekende uitgangspositie (volledig dicht) te brengen
7. Na de her-kalibratie keert de controller automatisch terug naar `Mode: AUTO` en hervat de klimaatregeling

**Totale duur vanaf alarm-clear tot weer normaal werkend**: ongeveer **3 tot 4 minuten**.

#### Wat moet de boer doen?

- **Tijdens een motor-alarm**: niet ingrijpen. Niet handmatig aan ramen werken, niet aan motoren of bedrading komen. **Bel de beheerder onmiddellijk** — alleen die mag de RRK-3 nazien, de fout vaststellen en de alarm-relay resetten
- **Na het oplossen door de beheerder**: laat de automatische 60-seconden wachttijd + re-kalibratie zijn werk doen. Dit gebeurt geheel zelfstandig. De boer hoeft niets te doen

#### Geen handmatige bypass

Een motor-alarm kan **niet** door de boer of vanuit de webinterface worden uitgeschakeld of overruled. Dat is bewust zo: een actief motor-alarm wijst op een veiligheidsprobleem dat eerst opgelost moet worden voordat de motoren weer mogen draaien. Het alarm valt alleen af als de RRK-3 zelf zijn alarmsignaal intrekt.

### 12.7 Wat doe je bij een onverwachte mode?

Vuistregel:
- **Mode: AUTO + groene LED** → niets doen
- **Mode: WIND + oranje LED** → wachten op rustiger weer (zie [§12.5](#125-windbeveiliging-in-detail))
- **Mode:Window Cal.** → wachten (max. 3 minuten — bij re-kalibratie na motor-alarm zelfs 60 sec wachttijd + 3 min kalibratie)
- **Mode: ALARM + rode LED** → bel de beheerder (zie [§12.6](#126-motor-alarm-in-detail))
- **`** SENSOR FAULT`** → bel de beheerder

---

## 13. Inschakelen na stroomuitval

Na elke power-cycle (stroomuitval, beheerder heeft de stekker eruit getrokken, of een power-cycle door jezelf uitgevoerd) doorloopt de controller automatisch dezelfde startsequentie:

1. **Voeding terug** — de LCD licht op binnen enkele seconden; de heartbeat-LED begint 1× per seconde te knipperen.
2. **Controleer-de-raamposities**: de controller leest uit zijn permanente geheugen wat de laatst bekende posities waren (zie [§12.4](#124-tijdens-kalibratie)).
   - **Stonden alle drie ramen volledig dicht?** → kalibratie wordt **overgeslagen**, mode springt binnen ~2 sec. op `Mode: AUTO`.
   - **Stond ten minste één raam open of onbekend?** → mode toont `Mode:Window Cal.` en kalibratie loopt (~3 minuten).
3. **Tijdens kalibratie**: alle drie de ramen sluiten gelijktijdig (M1/M2 ~26 sec., M3 ~176 sec.). Niet ingrijpen, niet handmatig aan ramen werken, niet inloggen, niet rebooten.
4. **Na kalibratie (of direct als die werd overgeslagen)** schakelt de controller naar `Mode: AUTO` — RGB-LED wordt groen — de klimaatregeling is weer actief.
5. **Controleer**: zijn de eerder ingestelde setpoints nog correct? (Setpoints worden in het permanente geheugen bewaard en zouden dus nog moeten staan.)
6. **Bij opstart met motor-alarm actief**: zowel kalibratie als skip worden overgeslagen, mode toont direct `Mode: ALARM`, RGB-LED gaat rood. Bel in dat geval de beheerder.

> **Tip**: noteer de tijd waarop de stroom uitviel en hoelang de uitval duurde. Dit kan voor de beheerder waardevol zijn bij het opsporen van een onderliggend probleem.

---

## 14. Onderhoud — wat de boer zelf doet

Dit zijn de onderhoudsacties die je als boer zelf kunt en mag doen. Voor alles daarbuiten: bel de beheerder.

### Schoonhouden

- **LCD-scherm**: schoonvegen met een **droge** doek (geen reinigingsmiddel, geen vochtig doekje — vocht kan in de elektronica dringen)
- **Toetsenbord**: stof verwijderen met droge doek
- **Sensoren** periodiek visueel controleren:
  - **T/RH-sensor binnen** — niet bedekt door spinrag, plantenresten of condens-druppels
  - **Wind-sensor buiten** — niet bedekt door bladeren of vuil; vrij om te draaien

### Wat je nooit doet

- **Ramen handmatig verplaatsen tijdens motor-actie** — wacht tot de motor stilstaat, anders kan de motor of de constructie beschadigen
- **De kast of het microprocessorboard openen** — alleen voor de beheerder, en alleen na power-cycle (voeding eruit)
- **Bedrading aanpassen** — alleen voor de beheerder

### klok-batterij (RTC)

In de controller zit een kleine knoopcel-batterij (CR2032) die de interne klok ook tijdens stroomuitval laat doorlopen. Vervanging is een taak van de **beheerder**. Symptomen van een lege RTC-batterij:
- De tijd is na een stroomuitval niet meer correct
- Dag/nacht-omschakeling klopt niet meer met zonsopkomst en zonsondergang

Bij deze symptomen: bel de beheerder.

### Power-cycle uitvoeren (volledige reset van de controller)

Soms helpt het om de controller volledig opnieuw op te starten — bijvoorbeeld als de heartbeat-LED is gestopt met knipperen of als de LCD bevroren lijkt.

**Optie A — voedingstekker**:
1. Trek de voedingstekker van de kascontroller eruit
2. Wacht **10 seconden**
3. Steek de stekker er weer in
4. De controller doorloopt automatisch de startsequentie (zie [§13](#13-inschakelen-na-stroomuitval))

`[FOTO: voedingstekker en stopcontact bij de kascontroller-kast]`

**Optie B — RESET-knop op het microprocessorboard**:
1. Open de kast (alleen als je daar door de beheerder toegang voor hebt)
2. Vind de **RESET-knop** op het microprocessorboard (LOLIN S3) — dit is een andere knop dan de IO0-knop is met **1** gemarkeerd op onderstaande figuur. 
3. Druk de RESET-knop kort in en laat los
4. De controller herstart hetzelfde als bij een power-cycle

![FOTO: microprocessorboard in kast met RESET-knop en BOOT-knop duidelijk gemarkeerd](images\LolinS3Reset.png)

*Figuur 6: Reset-knop op het microprocessorboard*

> **Waarschuwing**: druk niet op de **IO0-knop** in plaats van de RESET-knop, tenzij je bewust de fysieke reset-procedure uitvoert (zie [§18](#18-reset-procedure-boot-knop-op-microprocessorboard)). De IO0-knop start een fabrieksreset wanneer je hem te lang ingedrukt houdt.

---

## 15. Handmatige overname via de motorbox

In sommige situaties wil je de **automatische besturing door de kascontroller volledig uitschakelen** en zelf de regie over de ramen overnemen — bijvoorbeeld:

- Tijdens onderhoud aan een raammotor of aan de kasconstructie
- Bij een storing waarbij je niet meteen de oorzaak weet
- Wanneer een teelt-handeling vereist dat de ramen handmatig in een specifieke stand staan
- Als je twijfelt of de kascontroller iets vreemds doet en je tijdelijk de controle wilt loskoppelen

Op de **Hotraco RRK-3 motorbox** zit voor elk van de drie ramen een eigen schakelaar. Door alle drie de schakelaars **uit de automatische stand** te zetten, **negeert de motorbox alle commando's vanaf de kascontroller** — de kascontroller is dan effectief losgekoppeld van de raamaansturing.

![vooraanzicht van de Hotraco RRK-3 motorbox met de drie schakelaars per kanaal duidelijk in beeld](images\RBMotorControllerKnoppenstand.png)

*Figuur 7: vooraanzicht van de Hotraco RRK-3 motorbox met de drie schakelaars*

### Effect op de kascontroller

Zolang één of meer schakelaars op de motorbox **niet in de automatische stand** staan, geldt voor de betreffende ramen:

- **De kascontroller kan ze niet aansturen** — geen openen, geen sluiten, ongeacht wat T, RH of wind doen
- **Klimaatregeling werkt niet meer** voor het deel van de ramen dat handmatig staat
- **Windbeveiliging werkt niet meer** voor de uitgeschakelde ramen — dit is **een belangrijk veiligheidsrisico**: bij harde wind kunnen open ramen schade oplopen omdat de controller ze niet meer dicht kan sturen
- **Motor-alarm-detectie blijft wel werken** zolang de RRK-3 stroom heeft, maar de controller kan niet meer ingrijpen op de ramen

Wat **wel** blijft werken:
- De kascontroller blijft **temperatuur, vochtigheid en wind meten**
- Alle metingen blijven zichtbaar op het LCD en in de webinterface

### De kascontroller weet niet dat hij is uitgeschakeld

> **Lees dit aandachtig vóór je een schakelaar verzet.** Dit punt is essentieel om te begrijpen wat je op het scherm ziet zolang de schakelaars handmatig staan.

De kascontroller heeft **geen enkele terugmelding** uit de RRK-3 over de stand van de schakelaars. Hij **gaat ervan uit dat hij gewoon de baas is over de ramen**, ook wanneer dat fysiek niet meer zo is. Op het LCD en in de webinterface zie je dat ook niet aan het systeem af — er is **geen aparte melding "handmatig actief"**.

#### Wat de controller doet alsof er niets is gebeurd

- **Mode-regel blijft `Mode: AUTO`** — er verschijnt geen waarschuwing dat de uitvoering is onderbroken
- De controller blijft setpoints evalueren en **commando's naar de motorbox sturen** — alleen worden die commando's door de RRK-3 in de hand-stand genegeerd
- De **raamposities** op het LCD en in de webinterface (`OPEN`, `CLOS`, `MOV>`, `MOV<`) zijn de **interne aanname** van de controller op basis van zijn eigen verzonden commando's. Wordt een raam handmatig in een andere stand gezet, dan **weet de controller daar niets van** — wat het scherm toont kan dan flink afwijken van de werkelijke positie
- **Wind-override blijft binnen de controller werken**: bij harde wind stuurt hij `CMD_CLOSE_ALL` naar de motorbox. De motorbox negeert dit. Op het LCD zie je dan keurig `Mode: WIND` en de controller "denkt" dat alle ramen dicht zijn — fysiek kunnen ze nog volledig open staan. Dit is precies waarom **windbeveiliging effectief uitstaat** zolang ook maar één schakelaar handmatig staat
- Het systeemlog blijft normale events vastleggen — er staat **geen waarschuwing** in het log dat de uitvoering richting de motoren is onderbroken

#### Consequenties voor de boer

- **Vertrouw niet blind op het LCD of de webinterface** zolang de schakelaars handmatig staan. Wat je ziet kan afwijken van de werkelijke raamposities
- **Een motorbox-schakelaar die "tijdelijk" handmatig staat is gevaarlijk** — de controller waarschuwt je niet als je dat vergeet. Maak er een vaste gewoonte van om bij elke schakelaarwissel beide standen te noteren of een collega te attenderen
- **Bij terugschakelen naar automatisch** denkt de controller dat de ramen al staan zoals hij dat had bedacht. Klopt dat niet met de werkelijkheid (bijvoorbeeld: M3 staat fysiek open, maar de controller "weet" van een eerder zelf gestuurd CLOSE_ALL dat M3 dicht is), dan kan de eerstvolgende klimaat- of wind-actie tot een **onverwachte raambeweging** leiden
- **Doe daarom altijd een power-cycle** bij het terugschakelen (zie [§14](#14-onderhoud--wat-de-boer-zelf-doet)) **én zorg ervoor dat ten minste één raam fysiek open staat op het moment van de power-cycle**. Sinds firmware 1.17.36 slaat de controller de kalibratie over als hij denkt dat alle drie de ramen al dicht zijn — wat bij handmatige overname zonder waarschuwing kan kloppen met zijn interne aanname maar niet met de fysiek zichtbare werkelijkheid. Door één raam handmatig open te zetten vóór de power-cycle dwing je de controller tot een volledige CLOSE_ALL kalibratie, waarna hij weer met zekerheid weet waar alle ramen staan

> **Belangrijke gevolgtrekking**: zolang ook maar één schakelaar op de motorbox handmatig staat, is **alles wat je op het LCD en in de webinterface ziet over raamposities en bedrijfsmodus mogelijk niet representatief** voor de fysieke werkelijkheid. Vertrouw in die situatie op wat je met eigen ogen aan de ramen ziet, niet op het scherm.

### Wanneer toepassen?

- **Voor onderhoud**: zet alle drie de schakelaars op handbediening voordat iemand aan ramen of motoren werkt. Dit voorkomt dat motoren onverwacht starten op een commando van de kascontroller — een belangrijk veiligheidsuitgangspunt
- **Bij een storing waarbij ramen niet meer correct werken**: tijdelijke uitschakeling kan voorkomen dat een defect raam herhaaldelijk geforceerd wordt aangestuurd, totdat de beheerder ter plaatse is

### Wanneer terug naar automatisch?

Zet de schakelaars **pas terug op automatisch** als:

1. Het onderhoud of de werkzaamheden klaar zijn en niemand meer aan de ramen werkt
2. Alle drie de ramen in een veilige uitgangspositie (bij voorkeur **dicht**) staan
3. De beheerder akkoord is (bij twijfel)

Direct na het terugschakelen naar automatisch:

- De kascontroller kan op elk moment commando's gaan sturen — bij verschil tussen actuele klimaat en setpoints kunnen ramen direct gaan bewegen
- Bij twijfel over de raamposities: voer een **power-cycle** uit (zie [§14](#14-onderhoud--wat-de-boer-zelf-doet)) **terwijl ten minste één raam fysiek open staat**. De controller voert dan een volledige CLOSE_ALL kalibratie uit en weet daarna weer zeker waar de ramen staan. Power-cyclen terwijl alle drie de ramen dicht staan slaat de kalibratie over (~2 sec. herstart) — handig in normale situaties maar geen oplossing voor "ik weet niet of de controller-aanname klopt"

> **Veiligheid**: vergeet **nooit** om na onderhoud de schakelaars terug op automatisch te zetten — anders staat de windbeveiliging effectief uit en kan een onverwachte windvlaag schade veroorzaken aan open ramen.

---

## 16. Probleemoplossing (FAQ)

| Probleem | Mogelijke oorzaak / oplossing |
|---|---|
| **LCD blank / leeg** | Voeding controleren; is heartbeat-LED zichtbaar? Zo niet → power-cycle |
| **Heartbeat-LED knippert niet** | Controller is bevroren; doe een power-cycle (zie [§14](#14-onderhoud--wat-de-boer-zelf-doet)) |
| **RGB-LED is rood** | Kritiek alarm (`Mode: ALARM`); bel de beheerder |
| **RGB-LED is oranje** | Waarschuwing; lees mode-regel en eventuele `** SENSOR FAULT` op de LCD |
| **PIN vergeten (Farmer)** | Beheerder kan resetten via Admin-menu, of via fysieke reset-procedure ([§18](#18-reset-procedure-boot-knop-op-microprocessorboard)) |
| **PIN vergeten (Admin)** | Fysieke reset-procedure op het microprocessorboard ([§18](#18-reset-procedure-boot-knop-op-microprocessorboard)) |
| **Webinterface niet bereikbaar** | IP-adres juist gelezen op LCD? Apparaat op hetzelfde wifi-netwerk? Anders: bel beheerder |
| **Ramen reageren niet** | Controleer mode-regel: bij `Mode: WIND` zit de wind-override aan; bij `Mode: ALARM` motor-alarm. Controleer ook of de schakelaars op de motorbox in de automatische stand staan (zie [§15](#15-handmatige-overname-via-de-motorbox)). Bel beheerder bij ALARM |
| **Setpoint accepteert mijn waarde niet** | Controleer bereik: T-max day 15–45 °C, T-max ngt 10–35 °C, RH-max 40–98 %, RH-min 20–90 %. Waarden buiten bereik worden automatisch tot het minimum of maximum geknepen |
| **Kalibratie duurt lang** | ~3 minuten is normaal (M3 zijwand-raam heeft ~176 sec. nodig); pas na die tijd actie ondernemen |
| **Tijd / dag-nacht klopt niet meer** | Mogelijk RTC-batterij leeg; bel beheerder voor vervanging |
| **`** SENSOR FAULT` blijft staan** | T/RH-sensor reageert niet; bel beheerder. Controleer zelf alleen of sensor zichtbaar beschadigd is |
| **Webinterface logt me steeds uit** | Sessie verloopt na 5 min inactiviteit — log opnieuw in |

---

## 17. Verklarende woordenlijst

### Algemene termen

| Term | Betekenis |
|---|---|
| **Setpoint** | De gewenste waarde die de controller probeert te bereiken/handhaven |
| **Hysterese** | De bandbreedte rondom een setpoint waarbinnen de controller niet schakelt — voorkomt continu aan/uit |
| **Sliding average** | Glijdend gemiddelde over een tijdvenster (1–60 min); voorkomt overreactie op piekmetingen |
| **Wind override** | Automatisch sluiten van alle ramen wanneer de wind de veiligheidsgrens overschrijdt |
| **AP / Access Point** | Modus waarin de controller zelf een wifi-netwerk uitzendt (alleen voor beheerder) |
| **Client mode** | Modus waarin de controller verbindt met een bestaand wifi-netwerk |
| **Modbus / RS485** | Communicatieprotocol waarover de controller met de sensoren praat |
| **RTC** | Real-Time Clock — interne klok die de tijd onthoudt, ook bij stroomuitval |
| **Conflict-prioriteit** | Keuze welke regelactie voorrang krijgt als T en RH tegelijk om actie vragen |
| **Beaufort** | Eenheid van windkracht (0–12, 0 = stil, 12 = orkaan) |
| **CLOSE_ALL kalibratie** | Procedure bij opstart waarbij alle ramen worden gesloten om de uitgangspositie te kennen |
| **Power-cycle** | Voeding uit, kort wachten, voeding weer aan — volledige herstart |

### Engels-Nederlands LCD-termen

Onderstaande termen verschijnen op het LCD-scherm. Ze zijn gegroepeerd per functie.

**Bedrijfsmodus (Mode-regel):**

| Op LCD | Nederlands |
|---|---|
| `Mode: AUTO` | Normale automatische werking |
| `Mode: WIND` | Wind-override actief — alle ramen dicht |
| `Mode: ALARM` | Motor-alarm — Hotraco RRK-3 noodstop |
| `Mode:Window Cal.` | Raamkalibratie bezig |

**Sessiestatus (Sess-regel):**

| Op LCD | Nederlands |
|---|---|
| `Sess: NONE` | Niemand ingelogd |
| `Sess: Farmer` | Boer ingelogd |
| `Sess: Admin` | Beheerder ingelogd |
| `OTA` (achteraan) | Firmware-update bezig |

**Sensor- en metingweergaven:**

| Op LCD | Nederlands |
|---|---|
| `** SENSOR FAULT` | T/RH-sensor reageert niet |
| `Temp: --- °C` / `RH: ---` | Geen geldige meting |

**Raamposities:**

| Op LCD | Nederlands |
|---|---|
| `OPEN` | Raam is volledig open |
| `CLOS` | Raam is volledig dicht |
| `MOV>` | Raam is aan het openen |
| `MOV<` | Raam is aan het sluiten |
| `UNK` | Raam is in onbekende positie (kort na opstart) |

**Wifi:**

| Op LCD | Nederlands |
|---|---|
| `WiFi: connected` | Verbonden met netwerk |
| `WiFi: conn    BK` | Verbonden, online status-rapportage in **backoff** (tijdelijk uit; klimaat ongewijzigd) — sinds 1.18.0 |
| `WiFi: AP active` | Eigen AP actief |
| `WiFi: --------` | Geen verbinding |
| `Greenhouse-XXXX` | SSID van de eigen AP met unieke code op `XXXX` |

**Tijd:**

| Op LCD | Nederlands |
|---|---|
| `Src:NTP` | Tijd via internet (NTP) |
| `Src:RTC` | Tijd uit interne klok |

**Menu's:**

| Op LCD | Nederlands |
|---|---|
| `1:Clim 2:Wind 3:Access 4:Sys` | Hoofdmenu |
| `Climate menu` / `1Day 2Ngt 3CR` | Climate-menu (Day, Night, Conflict-prioriteit) |
| `1:Farmer 2:Admin 3:Logout` | Access-menu |
| `System settings` / `1=WiFi AP` | Systeem-menu (alleen Admin) |
| `T-max day (C)` / `T-max ngt (C)` | Maximum temperatuur dag/nacht |
| `RH-max day (%)` / `RH-max ngt (%)` | Maximum vochtigheid dag/nacht |
| `RH-min day (%)` / `RH-min ngt (%)` | Minimum vochtigheid dag/nacht |
| `T/RH prio (0-2)` | Conflict-prioriteit (0/1/2) |
| `Wnd-max` / `Wnd-prot` | Wind-maximum / windbeveiliging aan/uit |

**Login en bewerking:**

| Op LCD | Nederlands |
|---|---|
| `PIN (4 dig) *=Bk` | Voer 4-cijferige PIN in; `*` is backspace |
| `PIN (8 dig) *=Bk` | Voer 8-cijferige Admin-PIN in |
| `Access granted` / `Welcome!` | Ingelogd |
| `Wrong PIN!` / `Try again` | Foute PIN, probeer opnieuw |
| `Need all digits` / `then press #` | Voer eerst alle cijfers in |
| `Locked out!` / `Try in <n> s` | Geblokkeerd na te veel pogingen |
| `Logged out` | Uitgelogd |
| `Saved: <waarde>` | Wijziging opgeslagen |
| `No change` | Geen wijziging doorgevoerd |
| `Already logged in` | Je bent al ingelogd op dit niveau |

**Reset (IO0-knop):**

| Op LCD | Nederlands |
|---|---|
| `Reset PIN?` | Niveau 1 — alleen PIN's resetten |
| `Reset settings?` | Niveau 2 — alle instellingen resetten |
| `Restarting?` (tijdens vasthouden) | Niveau 3 — bezig naar volledige reset |
| `Restart!` / `Restarting...` | Volledige reset wordt uitgevoerd |
| `PIN Reset!` / `Default PINs set` | PIN's teruggezet naar fabrieksstandaard |
| `Settings Reset!` / `Defaults loaded` | Alle instellingen teruggezet |

---

## 18. Reset-procedure (IO0-knop op microprocessorboard)

Voor het geval de Beheerder-PIN vergeten is, of de controller moet volledig terug naar fabrieksinstellingen, kan de een fysieke reset uitgevoert worden via de **IO0-knop** op het microprocessorboard in de kast.

![FOTO: microprocessorboard met IO0-knop en RESET-knop duidelijk aangewezen](.\images\kasControllerLOLINRebootButton.png)

*Figuur 8: microprocessorboard met RESET-knop*

> **LET OP**: gebruik deze procedure alleen bewust. Op niveau 2 en 3 verlies je álle door de beheerder ingestelde wifi-, motor- en locatie-parameters.

### Procedure

1. Open de kast en lokaliseer de **IO0-knop** op het microprocessorboard (LOLIN S3). Dit is een andere knop dan de RST-knop — let goed op welke knop je indrukt
2. Druk de **IO0-knop** in en houd deze ingedrukt
3. Op de LCD verschijnt na enkele seconden een melding die aangeeft welk reset-niveau actief wordt
4. Laat de knop los op het gewenste niveau:

| Inhouden | LCD-melding | Effect |
|---:|---|---|
| **0–5 sec.** | (geen melding) | Geen actie — los gelaten zonder gevolgen |
| **5–10 sec.** | `Reset PIN?` | **Niveau 1 — PIN's resetten**: alleen de PIN-codes worden teruggezet naar fabrieksstandaard. Andere instellingen (klimaat, wifi, motor) blijven behouden. Geen reboot. |
| **10–15 sec.** | `Reset settings?` | **Niveau 2 — alle instellingen resetten**: klimaat, wind, motor, wifi, MQTT en systeem-instellingen worden allemaal teruggezet. PIN's ook gereset. Geen reboot. |
| **15–20 sec.** | `Restart!` / `Restarting...` | **Niveau 3 — volledige reset + herstart**: alles wordt gereset en de controller start opnieuw op |

5. Bij **20 seconden continu vasthouden** voert de controller automatisch een niveau 3 reset uit (volledige reset + herstart). Het is dus niet nodig om langer dan 20 seconden vast te houden
6. **Na een reset op niveau 2 of 3**: alle instellingen die de beheerder had geconfigureerd zijn weg. Bel de beheerder om wifi-, motor- en locatie-instellingen opnieuw te configureren
7. **Na een reset op niveau 1**: PIN's staan weer op fabrieksstandaard. Login met de fabrieks-Boer-PIN op de webinterface, en wijzig deze direct

### Welke reset wanneer?

| Situatie | Welk niveau? |
|---|---|
| Boer-PIN vergeten | Niveau 1 (Reset PIN) — daarna weer met `1234` inloggen en direct wijzigen |
| Beheerde-PIN vergeten | Niveau 1 — daarna kan de beheerder met de fabrieks-Admin-PIN inloggen |
| Controller helemaal vastgelopen, eenvoudige reboot helpt niet | Eerst proberen met power-cycle of RESET-knop ([§14](#14-onderhoud--wat-de-boer-zelf-doet)). Pas als dat niet werkt: niveau 3 |
| Controller compleet terug naar fabriek (bijv. bij verhuizing of overname) | Niveau 2 of 3 — daarna alles opnieuw laten configureren door beheerder |

> **Waarschuwing**: gebruik niveau 2 en 3 alleen wanneer écht nodig — alle door de beheerder ingestelde wifi-, motor- en locatie-parameters gaan verloren.

---

## 19. Bijlage A — contactgegevens beheerder

Voor alle vragen of problemen waar deze handleiding geen antwoord op geeft:

- **Naam beheerder**: \[invullen]
- **Telefoon**: \[invullen]
- **E-mail**: \[invullen]
- **Bereikbaarheid (uren)**: \[invullen]

**Bel de beheerder direct bij:**
- `Mode: ALARM` op de LCD (motor-noodstop)
- `** SENSOR FAULT` dat blijft staan
- Heartbeat-LED die ook na een power-cycle niet knippert
- Beschadigde sensoren of bedrading
- Wifi-verbinding die niet meer terugkomt na een routerherstart
- Onverklaarbaar gedrag dat niet in deze handleiding besproken wordt

**Voordat je belt, noteer:**
- Wat staat er op de mode-regel van het LCD?
- Welke kleur heeft de RGB-LED?
- Knippert de heartbeat-LED?
- Wanneer is het probleem begonnen?
- Was er net daarvoor een stroomuitval, onweer, of een handmatige actie?

> Maak eventueel foto's met een smartphone en stuur die naar de beheerder

---

## 20. Bijlage B — Aanbevolen startinstellingen per gewas

> **Belangrijk** — deze waarden zijn **startpunten**, geen absolute regels. De ideale instelling voor jouw kas hangt af van locatie, seizoen, gewas-variëteit, groeistadium en persoonlijke ervaring. Stel de waarden in zoals hieronder, observeer een paar dagen, en bij twijfel leuter een paar °C in de juiste richting tot het klopt met wat je in de kas ziet. De getallen zijn afgerond op hele graden / procenten — de kascontroller werkt sowieso met gehele getallen (zie [§4 Hoe regelt de controller het klimaat?](#4-hoe-regelt-de-controller-het-klimaat)).

### Klimaat-startinstellingen per gewas

De tabel is geordend per gewas-familie zodat verwante gewassen bij elkaar staan: vruchtgewassen, peulvruchten, bladgroenten, koolgewassen, wortelgewassen, kruiden en bloemen.

| Gewas | T dag<br/>min–max | T nacht<br/>min–max | RH dag<br/>min–max | RH nacht<br/>min–max | RH-regeling | CR-prio | Opmerkingen |
|---|:---:|:---:|:---:|:---:|:---:|:---:|---|
| **Tomaat** | 18–26 °C | 16–18 °C | 60–75 % | 65–80 % | aan | RH | Vruchtfase: houd RH onder 80 % 's nachts om botrytis te voorkomen. Hoge RH bij bloei → slechte vruchtzetting. |
| **Komkommer** | 22–28 °C | 18–21 °C | 70–85 % | 75–85 % | aan | T | Houdt van vocht. Bij T > 30 °C wordt de plant gestrest; bij T < 12 °C 's nachts groei-onderbreking. |
| **Paprika / peper** | 21–27 °C | 18–20 °C | 60–75 % | 65–75 % | aan | T | Bloemval bij T > 32 °C of T < 15 °C 's nachts. Stabiele temperatuur belangrijker dan exacte waarde. |
| **Aubergine** | 22–28 °C | 18–22 °C | 55–70 % | 60–75 % | aan | T | Warmtegevoelig — bij T > 30 °C ramen open houden. |
| **Meloen** | 22–30 °C | 18–22 °C | 60–75 % | 65–75 % | aan | T | Warmtegevoelig; nachten boven 16 °C, anders vruchtval. Tolereert T tot ~32 °C bij goede ventilatie. |
| **Ananaskers (Physalis)** | 18–26 °C | 15–20 °C | 55–70 % | 60–75 % | aan | T | Familie van tomaat; vergelijkbare aanpak maar iets droger en koeler verdraagbaar. Lange teelt, oogst pas na augustus. |
| **Aardbei** | 18–24 °C | 12–16 °C | 60–70 % | 60–75 % | aan | RH | Hoge RH = botrytis-risico op de vruchten. Liever wat koeler en droger dan warm en vochtig. |
| **Courgette / pompoen** | 20–28 °C | 16–20 °C | 60–75 % | 65–80 % | aan | T | Bij hoge RH bloeit de plant goed maar krijgt meeldauw. Ventilatie belangrijk. |
| **Bonen (stam / stok)** | 18–24 °C | 15–18 °C | 60–75 % | 65–80 % | uit | T | Vrij tolerant. Bij T > 30 °C wordt bloei en zetting onbetrouwbaar. |
| **Spaghettiboon (asperge-boon)** | 20–28 °C | 18–22 °C | 60–75 % | 65–80 % | uit | T | Warmer-minnend dan gewone bonen. Zetting matig onder 18 °C 's nachts. Lange peulen, lange oogstperiode. |
| **Peulen / erwten** | 15–22 °C | 10–16 °C | 60–75 % | 65–75 % | uit | T | Koel-seizoen-peulvrucht. Bloemval bij T > 25 °C. Vroege voorjaars- of late herfst-teelt. |
| **Sla / kropsla** | 15–22 °C | 10–15 °C | 50–70 % | 55–75 % | uit | T | Schiet door bij T > 24 °C. Vochtregeling meestal niet nodig — temperatuur is de hoofdsturing. |
| **Andijvie / radicchio** | 15–22 °C | 10–16 °C | 55–70 % | 60–75 % | uit | T | Vergelijkbaar met sla; iets warmer ondergrens dan spinazie. |
| **Spinazie** | 14–20 °C | 8–14 °C | 50–70 % | 55–75 % | uit | T | Koel-seizoen-gewas. Bij T > 22 °C schiet snel door (in bloei). |
| **Rucola** | 12–22 °C | 8–16 °C | 50–70 % | 55–75 % | uit | T | Koel-seizoen-bladgroente. Pikanter bij koel groeien; bitter of doorschieten bij T > 22 °C. |
| **Paksoi** | 14–22 °C | 10–16 °C | 55–75 % | 60–80 % | uit | T | Aziatische bladkool. Schiet snel door bij T > 24 °C 's middags. Vooral voor- en najaars-teelt. |
| **Snijbiet** | 14–24 °C | 10–16 °C | 55–75 % | 60–75 % | uit | T | Tolerant voor zowel koele als warmere periodes; minder schietgevoelig dan sla. |
| **Raapsteel** | 12–22 °C | 6–14 °C | 50–70 % | 55–75 % | uit | T | Koel-seizoen-blad; vorsttolerant tot ~ −2 °C. Snelle teelt (~4 weken). |
| **Palmkool (cavolo nero)** | 12–22 °C | 6–14 °C | 50–70 % | 55–75 % | uit | T | Winter-/voorjaars-teelt. Vorsttolerant; smaak verbetert zelfs na lichte vorst. |
| **Bladgroenten (boerenkool, andijvie-mix)** | 12–22 °C | 6–14 °C | 50–70 % | 55–75 % | uit | T | Winter-/voorjaars-teelt; verdraagt nachtvorst tot ~ −2 °C zonder schade. |
| **Koolrabi** | 15–22 °C | 10–16 °C | 55–75 % | 60–75 % | uit | T | Koel-seizoen-kool. Hoge T → houterige knol; vermijd T > 25 °C. |
| **Bospeen (peen)** | 12–22 °C | 8–15 °C | 50–70 % | 55–75 % | uit | T | Wortelgewas. Stabiele bodemvocht belangrijker dan luchtvocht; de controller stuurt ramen, niet irrigatie. |
| **Bosbiet (rode biet)** | 12–22 °C | 8–15 °C | 50–70 % | 55–75 % | uit | T | Wortelgewas vergelijkbaar met bospeen. Goed bij koele tot middentemperaturen. |
| **Radijs (radys)** | 12–22 °C | 6–14 °C | 50–70 % | 55–75 % | uit | T | Snelste gewas in de tabel (~3–4 weken). Bij T > 25 °C wordt de knol scherp en houterig. |
| **Groene selderij** | 15–22 °C | 10–15 °C | 70–85 % | 75–85 % | aan | T | Houdt van vocht zowel in lucht als bodem. Bij droge lucht bittere stengels. Lange teelt (~4–5 maanden). |
| **Kruiden (basilicum)** | 20–26 °C | 16–20 °C | 50–65 % | 55–70 % | uit | T | Houdt **niet** van koude voeten — nachttemperatuur niet onder 15 °C. |
| **Kruiden (peterselie, dille)** | 16–22 °C | 12–16 °C | 50–65 % | 55–70 % | uit | T | Koeltolerant. Hoge T → bittere smaak / vroege bloei. |
| **Bloemen (snijbloemen)** | 15–24 °C | 12–18 °C | 60–75 % | 65–80 % | aan | T | Sterk variëteit-afhankelijk; deze waarden zijn een algemeen midden-bereik. Raadpleeg de leverancier per variëteit (zonnebloem, zinnia, statice, cosmea hebben ieder eigen optima). |

### Wat de kolommen betekenen

- **T dag min–max** / **T nacht min–max**: temperatuurband. **Min**imum is de waarde waaronder de controller de ramen sluit (`T_min`); **max** is de waarde waarboven de controller de ramen opent (`T_max`). Tussen min en max gebeurt er niets (regelhysteresis — zie [§4](#4-hoe-regelt-de-controller-het-klimaat)).
- **RH dag min–max** / **RH nacht min–max**: vochtigheidsband, alleen actief wanneer **RH-regeling** aan staat.
- **RH-regeling**: of de controller mag reageren op vochtigheid. **Aan** = vochtigheid stuurt mee in de raam-beslissing; **uit** = alleen temperatuur stuurt (handig voor gewassen waar vocht niet de beperkende factor is).
- **CR-prio** (Conflict Resolution-prioriteit): wat doet de controller als T en RH tegelijk om tegengestelde acties vragen? **T** = temperatuur wint (gebruikelijk bij koel-seizoen-gewassen en warme zomers); **RH** = vochtigheid wint (gebruikelijk wanneer een gewas vochtigheids-gevoelig is — schimmelziektes, botrytis, meeldauw).
- **Opmerkingen**: gewas-specifieke aandachtspunten waar de getallen alleen niet voldoende zijn.

### Windbeveiliging — geldt voor alle gewassen

De **windsnelheid-drempel** (`Wnd-max`) staat standaard op **6 m/s** en is **niet gewas-afhankelijk** maar **kas-constructie-afhankelijk**. Pas hem alleen aan wanneer:

- Je merkt dat de ramen frequent dichtgaan bij wind die je intuïtief nog "rustig" zou noemen → verhoog naar 7–8 m/s.
- Je merkt dat de wind ramen schade aanricht voordat de override inslaat → verlaag naar 4–5 m/s.

De **windbeveiliging zelf** (aan/uit-schakelaar) moet **altijd AAN** staan tijdens de teelt. Alleen tijdelijk uitschakelen wanneer een beheerder lokaal aanwezig is en bewust met de ramen werkt. Zie ook [§12.5 Windbeveiliging in detail](#125-windbeveiliging-in-detail).

### Hoe te gebruiken

1. Zoek je gewas op in de tabel (of het meest vergelijkbare).
2. Log in als boer op de webinterface of LCD (zie [§9](#9-inloggen-als-boer)).
3. Stel achtereenvolgens in: **Climate-tab → T min/max dag en nacht → RH min/max dag en nacht → RH-regeling aan/uit → CR-prio**.
4. **Observeer 2–3 dagen** voordat je nog iets aanpast. De sliding-average uitmiddeling (6 min standaard) zorgt voor stabiel gedrag, maar de cumulatieve invloed van een instelling op de plant zie je pas na een paar dagen.
5. Stel **één parameter tegelijk** bij als iets niet klopt. Twee tegelijk verandert maakt het onmogelijk te zien welke aanpassing welk effect had.

### Wat de tabel niet vervangt

- **Bodemvochtigheid** — de controller stuurt ramen, geen irrigatie. Vochtig blad bij droge wortels lost de controller niet op.
- **CO₂-bemesting** — niet gemeten, niet gestuurd. Bij gesloten ramen op een zonnige ochtend kan CO₂ snel dalen tot een groei-beperking; bewust ventileren is dan nodig ongeacht wat T en RH zeggen.
- **Lichtniveau** — alleen indirect (dag/nacht-schakeling via zonsopkomst/zonsondergang). Schermdoeken, schaduwverf, en aanvullend assimilatielicht zijn buiten het bereik van deze controller.
- **Gewas-specifieke groeistadia** — bovenstaande waarden zijn voor de **vegetatieve / vruchtdragende hoofdfase**. Bij zaailingen, oogstpiek, of einde-seizoen kunnen optimale waarden afwijken (bv. iets koeler in de uitloopfase om houdbaarheid te verbeteren).

### Wanneer twijfel?

- Lees de tabel als **eerste schatting** en pas aan op wat je in de kas ziet.
- Bij ziekte/schade: noteer de instellingen die actief waren (Climate-tab) plus T-gemiddelde en RH-gemiddelde van de laatste 24 uur (Status-tab → Sensorhistorie). Stuur die naar de teeltvoorlichter of de Herenboeren-kennisgroep voor advies.
- De **fabrieksinstellingen** van de kascontroller (Dag T = 18 / 28 °C, Nacht T = 16 / 25 °C, RH = 50 / 75 %) zijn een redelijke "tomaat-achtig" middelweg. Voor andere gewassen begin je beter direct vanaf de tabel-waarden.

---

## 21. Versie en wijzigingshistorie

Inhoudelijke wijzigingen aan de firmware staan beschreven in het bestand `changelog.md` in de git-repository. Deze tabel houdt alleen bij welke firmware-versie door welke handleiding-versie wordt afgedekt.

| Versie | Datum | Firmware |
|---|---|---|
| 1.0 | Er was eens... | 1.16.34 |
| 1.1 | 2026-05-09 | 1.16.35–1.16.38 |
| 1.2 | 2026-05-10 | 1.16.39 |
| 1.3 | 2026-05-11 | 1.17.0–1.17.25 |
| 1.4 | 2026-05-12 | 1.17.25 (alleen kop-/voettekst en figuur-nummering) |
| 1.5 | 2026-05-12 | 1.17.26 |
| 1.6 | 2026-05-14 | 1.18.0–1.18.2 |
| 1.7 | 2026-05-14 | 1.18.3 |
| 1.8 | 2026-05-15 | 1.19.0–1.20.0 |
| 1.9 | 2026-05-16 | 1.20.1 |
| 1.10 | 2026-05-16 | 1.20.2 |
| 1.11 | 2026-05-16 | 1.20.2 (alleen documentatie — Bijlage B *Aanbevolen startinstellingen per gewas* toegevoegd) |
| 1.12 | 2026-05-16 | 1.20.2 (alleen documentatie — Bijlage B uitgebreid van 13 naar 28 gewassen passend bij de teelt in Wenumseveld: Meloen, Ananaskers, Spaghettiboon, Peulen, Rucola, Paksoi, Snijbiet, Raapsteel, Palmkool, Koolrabi, Bospeen, Bosbiet, Radijs, Groene selderij, Bloemen — geordend per gewas-familie) |
| 1.13 | 2026-05-17 | 1.20.3 |
| 1.14 | 2026-05-26 | 2.0.0-rc.1.5.0 |
| 1.15 | 2026-05-26 | 2.0.0-rc.1.5.1–rc.1.5.2 |
| 1.16 | 2026-06-26 | 2.0.0 t/m 2.1.1 — T min dag/nacht gedocumenteerd (webinterface); SD-logbestand bestandsnaam eenheid-ID prefix (gh#30, 2.0.1); windgemiddelde onafhankelijk venster (gh#35, 2.1.0); standaard uitmiddelvenster gecorrigeerd naar 6 min; bugfix HTTP-statuscode in auditlog (gh#34, 2.1.1) |
| 1.17 | 2026-06-27 | 2.1.1 — figuurcaptions toegevoegd (Figuur 2 schematisch overzicht, Figuur 6 reset-knop); figuurcaptions hernummerd (Figuur 1–8); "Standby" → "Stand-by" consistent; "calibratie" → "kalibratie"; "in- en uitgaan"; "hoelang"; "foto's"/"smartphone" |

---

*Einde van de handleiding.*
