# Handleiding Kascontroller — voor de beheerder

**Versie:** 1.28
**Datum:** 2026-09-25
**Firmware:** 2.14.0

---

> **Voor wie is deze handleiding?**
> Dit document is geschreven voor de **technisch beheerder / installateur** die verantwoordelijk is voor de installatie, configuratie, onderhoud en het oplossen van storingen aan de kascontroller. De boer \/ kasgebruiker bedient het systeem dagelijks; daarvoor is een aparte [boer-handleiding](boerHandleiding.md).

> **Veiligheid**
> Binnen in de kast van de kascontroller bevinden zich onderdelen onder **netspanning (230 V)**. Werk uitsluitend aan de bedrading of aan motoren met de **voeding afgekoppeld**. Een aansluiting op het lichtnet vereist kennis van elektrische installaties. Let bovendien op draaiende motoren: handmatige werkzaamheden aan ramen mogen alleen wanneer de motoren stilstaan en bij voorkeur met de motorbox-schakelaars in handbediening (zie [§15](#15-handmatige-overname-via-de-motorbox)).

> **Verwijzing naar boer-handleiding**
> Voor algemene uitleg over dagelijks gebruik, schermen op de LCD en alarm-meldingen verwijst dit document regelmatig naar de [boer-handleiding](boerHandleiding.md). Onderwerpen die specifiek voor de beheerder zijn worden hier volledig behandeld.

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
11. [Eerste-installatie WiFi-verbinding](#11-eerste-installatie-wifi-verbinding)
12. [Alarmen en bedrijfsmodi — diagnose en herstel](#12-alarmen-en-bedrijfsmodi--diagnose-en-herstel)
13. [Inschakelen na stroomuitval](#13-inschakelen-na-stroomuitval)
14. [Onderhoud — wat de beheerder doet](#14-onderhoud--wat-de-beheerder-doet)
15. [Handmatige overname via de motorbox](#15-handmatige-overname-via-de-motorbox)
16. [Probleemoplossing — Beheerder-niveau](#16-probleemoplossing--Beheerder-niveau)
17. [Verklarende woordenlijst](#17-verklarende-woordenlijst)
18. [Reset-procedure (BOOT-knop)](#18-reset-procedure-io0-knop-op-microprocessorboard)
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
- Eerste-installatie en in bedrijf stelling 
- Alle beheersfuncties op het LCD/toetsenbord én in de webinterface
- Configuratie van WiFi, locatie, NTP, motor-tijden, sensor-instellingen, hysteresis, glijdend gemiddelde
- PIN-management voor zowel Boer als Beheerder
- Diagnose en oplossing van alarmen, sensor-fouten en motor-storingen
- Firmware-updates (OTA), SD-kaart beheer, downloaden van logs
- Reset-procedures en herstel naar fabrieksinstellingen

### Wat staat er niet in?
- **Firmware-broncode-niveau** detail → zie `design/technicalSoftwareDesignSpecification.md`
- **Hardware-ontwerp / schema's / printontwerp** → zie de `hardware/` map in de repository
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
- Automatische CLOSE_ALL kalibratie bij opstart wanneer ten minste één raam niet als CLOSED in het geheugen staat opgeslagen, en na motor-alarm-clearance
- Permanente opslag in het geheugen (Non Volatile Memory - NVS) van alle setpoints en configuratie instellingen
- Logging van de activiteiten op de kascontroller in het geheugen en op SD-kaart

### Wat doet de controller niet
- Geen verwarming of koeling aansturen
- Geen klimaatschermen aansturen
- Geen besproeiing of CO₂-dosering aansturen
- Ramen die gedeeltelijk dicht gestuurd worden. De controller stuurt op tijd, met commando's via de RRK-3; de motoren geven geen positie terug. Met een raamstandsensor op M3 controleert hij sinds 2.10.0 wél of M3 na elke beweging zijn eindstand haalde, en meldt hij het als dat niet zo is (zie *M3 raamstandsensor — controle na elke beweging*), maar ook M3 blijft hij op tijd sturen

---

## 3. De kas en het systeem

![FOTO: bovenaanzicht / plattegrond van de kas met M1, M2 en M3 aangegeven](images\KasRaamlocaties.png)

*Figuur 1: bovenaanzicht / plattegrond van de kas met M1, M2 en M3 aangegeven* 

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
|---|---|---|
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

![Kast met bedrading en bordjes met etiketten, GPIO-pinout zichtbaar](images\kasControllerFrontInternalView.png)

*Figuur 2: Kas controller interieur.*

### Schematisch overzicht

![Schematisch overzicht](images\SchematischOverzicht.png)

*Figuur 3: Schematisch overzicht kas controller.*

---

## 4. Hoe regelt de controller het klimaat?

### Setpoints (door de boer bewerkbaar via Climate-tab of LCD-menu)

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

- **Hysteresis**: Dit is de bandbreedte rondom een setpoint waarbinnen niet wordt geschakeld. Voorbeeld bij `hyst_t = 5`: bij `t_max_dag = 28 °C` opent een raam wanneer T **boven** 28 °C komt — het gemiddelde wordt op hele graden afgerond, dus vanaf 29 °C — en sluit het pas weer wanneer T ≤ 23 °C. Voorkomt continu in/uit-schakelen rond een setpoint.
- **Glijdend gemiddelde**: meetwaarden worden over de venstertijd in minuten gemiddeld voordat ze met een setpoint worden vergeleken. Een groter venster maakt de regeling rustiger en minder gevoelig voor pieken (een korte zonnestraal op de sensor); een kleiner venster reageert sneller. De live waarden op het LCD display toont de **ruwe** (laatste) meetwaarde zodat de gebruiker altijd het actuele resultaat ziet. Windsnelheid en -richting gebruiken een eigen venster (`avg_win_wind`, alleen instelbaar door de Beheerder in het Wind-tabblad).
- **Vochtregeling aan/uit**: vochtregeling uit; alleen de temperatuur wordt geregeld. Dit kan zinvol zijn bij teelten waarbij luchtvochtigheid niet relevant is of wanneer de luchtvochtigheid-sensor defect is.
- **Conflict-prioriteit**: wat er gebeurt wanneer de twee regel-assen tegengestelde acties vragen.
  - `0` — Temperature first: de temperatuur-stap wint (default)
  - `1` — Humidity first: de luchtvochtigheid-stap wint
  - `2` — Auto: de hoogste van de twee stappen wint

  Dit geldt **ook wanneer de temperatuur-as stap 0 vraagt**: stap 0 is een actieve wens om te sluiten, geen onthouding. Met de default `0` opent de vochtregeling daardoor nooit uit zichzelf een raam. Zie *Hoe de twee regel-assen worden gecombineerd* hieronder.

### Dwelltime (wachttijd) per motor (Beheerder-only, Motors-tab)

Minimum tijd dat een raam in een stand moet blijven voordat de controller hem opnieuw mag schakelen. Deze instelling dempt oscillaties, met name bij het raam in de zijwand (M3) met een groot oppervlak en lange klimaat-respons.

| Motor | Dwell-open default | Dwell-close default | Bereik |
|---|---|---|---|
| M1 (dak zuid) | 300 s (5 min) | 300 s (5 min) | 0–1500 s |
| M2 (dak noord) | 300 s (5 min) | 300 s (5 min) | 0–1500 s |
| M3 (zijwand noord) | **1500 s (25 min)** | **600 s (10 min)** | 0–1500 s |

> De M3 default-waarden zijn op kas gekalibreerd voor eem lange responstijd. Andere kassen kunnen andere waarden vergen.

### Motors-tab: per raam gegroepeerd

Sinds 2.8.0 staan de instellingen **per motor** bij elkaar (M1, M2, M3) in plaats van per soort instelling. Bij M3 staan ze bovendien in twee groepen:

| Groep | Wat het stuurt |
|---|---|
| **Time control** · *in gebruik* | Loopttijd en dwelltijden — dit is wat de ramen vandaag aanstuurt |
| **Linear control** · *raamstandsensor* | Instellingen voor de raamstandsensor en voor de **lineaire besturing** van M3 (sinds 2.12.0) |

De groep *Linear control* begint met **Position sensor fitted**: heeft M3 een raamstandsensor, ja of nee (sinds 2.9.0, zie hieronder). Staat die op **No**, dan is alles daaronder **grijs**, met erboven waarom.

De **Dodezone** is verder een gewone instelling en werkt op elke build. Het blok **Commissioning** daaronder (raamgrootte, kalibratie-oordeel, teach) werkt **sinds 2.11.0 op elke build**, zolang de sensor als gemonteerd is ingesteld — je hoeft er dus geen speciale build meer voor op de controller te zetten. *(Tot 2.11.0 kon leren alleen met een commissioning-build; een sensor die toen geleerd is houdt zijn kalibratie, daar verandert niets aan.)* Staat het blok grijs terwijl de sensor gemonteerd is, dan is bij het opstarten een route niet geregistreerd: **dat is een storing**, noteer de versie en meld het. Dat grijs-met-reden is bewust: een instelling die simpelweg verdwijnt is niet te onderscheiden van een verkeerd tabblad of een storing.

### M3 raamstandsensor — gemonteerd of niet (sinds 2.9.0)

Met **Position sensor fitted** (tab **Motors**, M3 → *Linear control*) vertel je de controller of M3 een raamstandsensor heeft: de trekdraadsensor op Modbus-adres 40. De standaard is **No**.

| Instelling | Wat de controller doet |
|---|---|
| **No** | Spreekt adres 40 helemaal niet aan. Het kaartje *Modbus bus* en het logboek tonen geen regel voor adres 40, M3 toont geen openingspercentage, en er komt nooit een sensorstoring. Zo hoort een kas zonder sensor eruit te zien |
| **Yes** | Leest de sensor. Reageert die niet (meer), dan is dat een **storing**: de badge *Window sensor fault* en een regel in het logboek, binnen ongeveer een minuut |

In beide gevallen loopt M3 op zijn **looptijd**: de instelling verandert niets aan hoe het raam bewogen wordt.

Vóór 2.9.0 kon de controller "geen sensor" niet onderscheiden van "sensor reageert niet". Een kas zonder sensor vroeg adres 40 elke 30 s tevergeefs, en het kaartje *Modbus bus* liet dat zien als een falende sensor. Een gemonteerde sensor die uitviel, gaf juist geen melding.

> **Na een update naar 2.9.0 staat de instelling op No, ook bij een kas die wél een sensor heeft.** Zet hem daar **eenmalig** op **Yes**. Hetzelfde geldt na een reset op niveau 2 of 3 met de BOOT-knop (§18), want die zet alle instellingen terug. Wie een sensor monteert, zet de instelling daarbij op **Yes**. Hij staat in het logboek als *wpos_fitted_m3*.

### M3 raamstandsensor — controle na elke beweging (sinds 2.10.0)

Met *Position sensor fitted* op **Yes** beoordeelt de controller elke beweging van M3 op het moment dat de motor stopt:

| Oordeel | Wanneer | Wat je ziet |
|---|---|---|
| **Bevestigd** | De eindsensor aan de kant waar M3 heen ging schakelde, nadat het raam van de andere kant was weggelopen, en de gemeten stand klopte daarmee. Ook een beweging naar de stand waar M3 al stond (een kalibratie van een dicht raam) is bevestigd | Alleen een regel in het logboek |
| **Niet bereikt** | De motor liep zijn volle looptijd, maar de sensor zag de eindstand niet | De badge *M3 not confirmed* in de tegel *Alarms*, tot de volgende bevestigde beweging, en een regel in het logboek met de stand waar het raam stopte |
| **Niet beoordeeld** | De beweging werd afgebroken (een omkering, een kalibratie, een motoralarm), of de sensor viel weg of meldde een storing | Alleen een regel in het logboek |

- **Alleen een melding.** De controller stuurt M3 nog steeds op zijn looptijd, en noteert daarna OPEN of CLOSED, ook als de sensor iets anders zag. De ventilatie werkt gewoon door.
- **Bij *M3 not confirmed* controleer je twee dingen.**
  - **De looptijd van M3** (tab **Motors**): is die korter dan de beweging werkelijk duurt, dan stopt de motor voordat het raam zijn eindstand haalt. Het ergste geval is de looptijd van de testopstelling (13 s) in de kas (171 s): M3 gaat dan maar een tiende open terwijl het logboek OPEN noteert. Deze controle is er juist om dat zichtbaar te maken.
  - **Het mechaniek**: een raam dat zwaar loopt, of een draad die slipt.

**De looptijdcontrole.** Bij elke **volledige** beweging, van eindstand tot eindstand, meet de controller per richting hoe lang het duurt tot de eindsensor schakelt, en vergelijkt dat met de ingestelde looptijd:

| Badge | Betekent | Wat te doen |
|---|---|---|
| *M3 travel time too short* | De eindsensor schakelde later dan de looptijd: alleen de vaste marge van 5 s brengt het raam nog in zijn eindstand | Verhoog de looptijd tot boven de gemeten tijd; die staat in het logboek |
| *M3 travel time too long* | De eindsensor schakelde al binnen de helft van de looptijd | Verlaag de looptijd. Voor het raam onschuldig, want de eindschakelaar stopt de motor, maar de controller rekent het meetritme van de sensor uit de looptijd, en dat klopt dan niet |

- **De controller past de looptijd nooit zelf aan.** Een badge verdwijnt vanzelf bij de eerste volledige beweging die weer binnen de looptijd valt.
- **Beide eindsensoren tegelijk actief** is een bedradingsfout. De sensor geldt dan als storing: de badge *Window sensor fault*, M3 op zijn looptijd, en bewegingen worden niet beoordeeld tot het verholpen is.
- **In het logboek** staan het oordeel onder parameter 251 en de looptijdcontrole onder 252 (`ALARM`, kanaal 6); `log/logparser.md` beschrijft ze.


### M3 raamstandsensor — kalibratie (commissioning)

#### Twee afstanden, en ze zijn niet hetzelfde

Dit is de belangrijkste val in dit scherm. De motor stuurt het raam **voorbij** de eindsensoren door tot zijn eigen eindschakelaars hem stoppen — die extra afstand heet de *blinde overlap*.

| Instelling | Meet van | Waar hij heen gaat |
|---|---|---|
| **Loopttijd (s)** | **eindschakelaar tot eindschakelaar** — de volledige motorloop, inclusief de blinde overlap | `travel_m3` in de controller; dit is wat de relais aanstuurt |
| **Raamgrootte (mm)** | **eindsensor tot eindsensor** — het bereik dat 0 tot 100 % heet | register `40004` in de sensor zelf |

> **De loopttijd is dus altijd langer dan de raamgrootte suggereert.** Verwissel je ze, dan is de kalibratie stil verkeerd: de sensor rekent dan met een afstand die het raam nooit aflegt.

De raamgrootte meet je met een rolmaat, in millimeters, tussen de twee eindsensoren. Vul die in **vóór** je een teach start — het is de bekende afstand waartegen de sensor zijn eigen meetwaarde ijkt.

#### Wat de teach doet

De sensor meet intern een weerstandswaarde. Op zichzelf is dat een getal zonder betekenis. De **teach koppelt dat getal aan de raamgrootte**: de controller rijdt M3 naar **beide** eindsensoren, en op het moment dat een eindsensor schakelt legt de sensor vast welke meetwaarde bij "helemaal dicht" en welke bij "helemaal open" hoort. Daarna weet hij millimeters.

**Daarom hoef je de uitkomst niet te beoordelen.** Jij gaf de afstand, de sensor nam de twee eindpunten waar, de rest volgt. Het scherm toont in plaats daarvan een **oordeel**, en alleen als dat oordeel iets meldt is er reden om opnieuw te teachen.

> **De knop Teach laat het raam bewegen: twee volledige raamgangen, soms drie.** Het maakt niet uit waar M3 staat — open, dicht of halverwege. De controller rijdt eerst weg van de stand waarin hij M3 denkt te weten en daarna weer terug, zodat **beide** eindsensoren schakelen. Een raam dat al tegen een eindsensor staat kan die kant pas vastleggen nadat het er eerst vanaf is geweest; daarom is één raamgang nooit genoeg. Klopt de stand die de controller denkt te weten niet, dan loopt de eerste gang tegen de eindschakelaar en volgt er een derde. Er volgt eerst een bevestigingsvraag.
>
> Tijdens de teach toont het scherm welke gang loopt en hoeveel eindsensoren al bereikt zijn, bijvoorbeeld *"leg 2 of up to 3 — M3 CLOSING"* met *"1 of 2 end sensors reached"*. Op het productieraam duurt één gang ongeveer drie minuten: **laat hem lopen** tot er *teach complete* staat. *Abort* stopt de teach; het raam maakt de gang waarin het zit wel af.

> **Tijdens de teach staat de automatische klimaatregeling stil.** Zodra de teach start, zet de controller zichzelf in **Stand-by**, net als bij handmatige raambediening op het LCD. Die Stand-by blijft staan tot je uitlogt of je sessie verloopt (standaard na 5 minuten zonder activiteit), ook als de teach al klaar is. Daarna sluiten alle ramen één keer om te kalibreren en neemt de regeling het weer over. Zolang de pauze duurt, staat dat onder de teach-knop.
>
> Er zijn drie uitzonderingen:
>
> - **Een Stand-by die al aan stond blijft staan.** Stond de controller al in Stand-by omdat iemand dat zo had ingesteld, dan raakt de teach die niet aan; hij verdwijnt dus ook niet als je uitlogt.
> - **Een herstart beëindigt de pauze.** Deze Stand-by wordt niet opgeslagen. Wordt de controller tijdens of na de teach herstart, dan regelt hij na het opstarten gewoon weer automatisch.
> - **Een eigen keuze gaat voor.** Kies je tussendoor zelf *Stand-by* of *Normal* in de Mode-keuzelijst, dan geldt jouw keuze, en een gekozen Stand-by blijft dan ook na het uitloggen staan.

#### Als de teach mislukt

Naast *teach failed* staat de reden. Een mislukte teach laat de bestaande kalibratie ongemoeid.

| Reden (op het scherm) | Betekenis | Wat te doen |
|---|---|---|
| *M3 stopped between the end sensors* | Een gang eindigde voordat er een eindsensor schakelde | **Loopttijd M3** controleren: die moet de hele weg tot voorbij de eindsensoren dekken |
| *M3 did not leave its end sensor in either direction* | Het raam bewoog in geen van beide richtingen | Motor, relais en eindschakelaars nakijken |
| *the sensor refused the result* | Beide eindsensoren schakelden, maar de twee meetwaarden lagen vrijwel gelijk | Trekdraad controleren (los, of loopt niet mee) en opnieuw teachen |
| *M3 was moving* | M3 bewoog al bij de start, of iets anders stuurde M3 tijdens de teach | Wachten tot M3 stilstaat en opnieuw starten |
| *the sensor dropped the teach* | De sensor liet de teach los voordat beide eindsensoren bereikt waren — meestal een herstart van de sensor | Opnieuw teachen |
| *a wind override intervened* / *the motor alarm fired* | Een veiligheidsfunctie greep in | Na afloop opnieuw teachen |

> **Een teach die niemand meer volgt, wordt afgebroken.** Wordt de controller tijdens een teach herstart, dan blijft de sensor wachten op eindsensoren — en de eerstvolgende gewone raambewegingen zouden de teach dan onbewaakt afmaken. De controller breekt zo'n achtergebleven teach daarom zelf af. In het logboek staat dat als *teach ORPHAN found (armed, no teach running) - ABORTED*; dat is geen storing.

#### Het kalibratie-oordeel

| Melding | Betekenis | Wat te doen |
|---|---|---|
| **VALID** | Alle controles in orde. Het scherm toont erbij wat er geleerd is, bijvoorbeeld *"taught 0…858 (84 % of range), window 1500 mm"* | Niets |
| geen raamgrootte | `40004` staat op nul — er is nooit geteacht | Raamgrootte invullen, dan teachen |
| nooit geteacht | De twee vastgelegde waarden zijn gelijk | Teachen |
| bereik te smal | De twee waarden liggen te dicht bij elkaar: de meetdraad heeft de beweging niet volledig gevolgd | Trekdraad controleren en opnieuw teachen |
| teach nog actief | Een teach is gestart maar niet afgerond | Afbreken of afronden |
| draad-circuit open | De meetdraad is onderbroken (bit 2) | Bekabeling naar de sensor controleren |
| buiten bereik | De meetwaarde valt buiten het geijkte gebied (bit 6) | Opnieuw teachen; blijft het terugkomen, dan is de montage verschoven |
| volgt niet | Het raam bewoog wel maar de meetwaarde niet (bit 7) | Mechanisch nakijken: trekdraad los, slippend of vastgelopen |

> **Eén blinde vlek, en die is het vermelden waard.** Bij een volledig gesloten raam staat de meetwaarde op nul. Een *kortgesloten* meetdraad geeft óók nul. Die twee zijn op deze installatie niet uit elkaar te houden, dus **VALID sluit die ene storing niet uit**. Zie het plan §2a.6.

#### Wanneer opnieuw teachen

Niet periodiek. Alleen wanneer:

- het kalibratie-oordeel iets anders dan VALID meldt;
- er aan de raamstandsensor of de trekdraad is gewerkt;
- de mechanica van M3 is aangepast, waardoor de afstand tussen de eindsensoren verandert — vul dan **eerst** de nieuwe raamgrootte in.

#### Dodezone (mm)

De kleinste afwijking waarvoor het raam nog bijgestuurd wordt. Te klein en het raam gaat op ruis heen en weer; nul zou het onafgebroken laten klapperen. Instelbaar in tab **Motors**, onder M3 → *Linear control*, op elke build (1–200 mm, standaard 20), zolang *Position sensor fitted* op **Yes** staat.

Sinds 2.12.0 is dit **ook de aankomsttolerantie van de lineaire besturing**: M3 stopt zodra hij binnen deze afstand van de gevraagde stand is, en de regeling vraagt geen beweging aan die kleiner is dan deze afstand. **Behalve bij de eindstanden (sinds 2.12.2, gh#83):** ligt de gevraagde stand binnen deze afstand van helemaal dicht of helemaal open, dan rijdt de controller door tot de eindsensor schakelt. Een eindstand is een schakelaar, geen getal — daarvoor kon een "dicht" een dodezone op een kier blijven staan, een hele nacht lang. Verder gebruiken twee dingen hem al langer:
- **de controle op een te vroeg stoppend raam**: een SLUIT-gang die binnen de dodezone van "dicht" eindigt zonder dat de eindsensor schakelt, wordt als storing gemeld;
- **het logboek**: in rust schrijft de controller alleen een positieregel als het raam verder bewogen is dan de dodezone (minstens 5 mm). Zo zie je ook een raam dat zonder opdracht van de controller bewoog, bijvoorbeeld via de handschakelaars in de motorbox.

#### M3-besturing: Tijdgestuurd of Lineair (2.12.0)

**Tijdgestuurd**: M3 wordt volledig geopend of gesloten op zijn looptijd, net als M1 en M2. Een gemonteerde sensor meet en meldt, maar stuurt niet. Dit was tot 2.13.0 de fabrieksinstelling.

> **Sinds 2.13.0 is *Lineair* de fabrieksinstelling** (besluit van de beheerder, 2026-09-25). Dat geldt alleen voor een **nieuwe of teruggezette** controller: een bestaande controller houdt bij een firmware-update gewoon wat er was ingesteld. En een controller zonder positiesensor blijft precies doen wat hij altijd deed — de instelling heeft dan niets om op te sturen, en de regeling valt vanzelf terug op tijdgestuurd.

**Lineair**: M3 wordt naar een gemeten stand gereden en daar gestopt. Dit vraagt één instelling **én** een positie die de controller vertrouwt — sensor gemonteerd, antwoordend, ingeleerd en zonder storing.

> **De instelling zegt wat je vraagt, niet wat er gebeurt.** Direct onder de keuze staat welke besturing op dit moment **werkelijk actief** is. Staat daar *Tijdgestuurd* terwijl je *Lineair* hebt gekozen, dan noemt de tekst **sinds 2.13.0 de precieze reden**: sensor niet gemonteerd, geeft geen antwoord, meldt zelf een storing, beide eindsensoren tegelijk actief, nog bezig met controleren, of de wachttijd van twee minuten na een terugval. Zodra dat in orde is, schakelt de controller vanzelf om — je hoeft niets opnieuw in te stellen.
>
> **Je hoeft nooit zelf een raam te bewegen om lineair te laten beginnen.** Tot 2.13.0 moest dat wel: de controller nam de lineaire besturing pas over aan het einde van een beweging, dus bleef hij tijdgestuurd tot M3 om een andere reden ging bewegen — op een milde dag urenlang. Sinds 2.13.0 schakelt hij ook **in rust** om, binnen ongeveer een halve minuut nadat de positie vertrouwd is. Staat er kort na een herstart nog *Tijdgestuurd*, dan is dat een moment, geen toestand.

Wat je verder moet weten:

- **Terugvallen gaat onmiddellijk, terugkomen niet.** Verliest de controller het vertrouwen in de positie, dan stuurt hij M3 direct weer op tijd. Terugschakelen naar lineair gebeurt pas na een afgeronde beweging én een wachttijd van twee minuten, zodat de besturing niet elke minuut wisselt.
- **De veiligheid verandert niet.** Windbeveiliging, motoralarm en de sluitronde bij opstarten werken precies zoals altijd en sluiten M3 volledig.
- **Na een terugval staat M3 mogelijk halfopen.** De tijdgestuurde regeling stuurt hem dan bij de eerstvolgende beslissing naar een eindstand — open of dicht, wat de regeling op dat moment vraagt.
- **Elke wisseling staat in het logboek**, met de reden, en is terug te lezen met `logparser.py`.

#### Minimale tussentijd (s)

Alleen in lineaire besturing: de kortste tijd tussen het einde van één beweging van M3 en het begin van de volgende. Instelbaar in tab **Motors**, onder M3 → *Linear control* (0–1500 s, standaard **600**).

> **Deze instelling VERVANGT de open- en sluit-rusttijd van M3 zolang lineair actief is.** Dat is opzet: een regeling die voortdurend bijstuurt zou anders na elke beweging 25 minuten stilstaan.

De standaard van 600 s (tien minuten) verandert met de huidige regeling **niets**: die wacht zelf al minstens zo lang tussen twee bewegingen. Het is een ondergrens voor het geval de regeling ooit vervangen wordt door één die sneller beslist — zonder die ondergrens zou M3 dan veel vaker gaan lopen. Tien minuten is ook ongeveer de traagheid van de kas zelf: de gemeten temperatuur loopt 3,5 tot 5,5 minuten achter op de lucht.

> **Zet deze waarde niet boven de 900 s.** Daarboven schommelt de temperatuur net zo veel als bij tijdgestuurde besturing, terwijl M3 nog steeds vaker loopt — lineaire besturing levert dan niets meer op. **0 betekent geen tussentijd** en is alleen bedoeld om bewust te testen wat de regeling in haar eentje doet.

### Stapsgewijs ventileren

De controller telt een interne ventilatie-stap-teller (0–3) per regel-as (Temperatuur en Luchtvochtigheid):

| Stap | Open ramen |
|---|---|
| 0 | Geen — alle ramen dicht |
| 1 | M1 |
| 2 | M1 + M2 |
| 3 | M1 + M2 + M3 |

Bij overschrijding van een setpoint stijgt de stap; binnen de hysteresis daalt hij. De Temperatuur-stap en luchtvochtigheid-stap worden via de conflict-prioriteit gecombineerd.

### Hoe de twee regel-assen worden gecombineerd

**Temperatuur-as.** Het gemiddelde wordt eerst op hele graden afgerond en daarna met `t_max` vergeleken. De stapbreedte is `hyst_t / 3`, minimaal 1 °C: bij `hyst_t = 5` telt elke hele graad boven het setpoint één stap, dus met `t_max_dag = 28 °C` geeft 29 °C stap 1, 30 °C stap 2 en 31 °C stap 3. Zakt de temperatuur weer, dan volgt de as direct terug — 3 → 2 → 1 — maar **stap 1 houdt ze vast**: terug naar stap 0 gaat deze as pas wanneer het gemiddelde een volle `hyst_t` onder het setpoint ligt (23 °C in dit voorbeeld). M1 blijft dus open tot ruim onder het setpoint; dat is de sluit-hysterese uit de parametertabel hierboven.

**Luchtvochtigheid-as.** Boven `rh_max` dezelfde ladder, maar met `hyst_rh` als stapbreedte: bij `hyst_rh = 12` is dat 4 % per stap, dus met `rh_max_dag = 75 %` geeft 76–79 % stap 1, 80–83 % stap 2 en vanaf 84 % stap 3. Onder `rh_min` geeft de as **stap 0**, een actieve wens om te sluiten. Daartussen doet de as **niet mee** en telt alleen de temperatuur.

> **De luchtvochtigheid-as heeft geen sluit-hysterese.** `hyst_rh` bepaalt uitsluitend de stapbreedte. Zodra het gemiddelde terug op of onder `rh_max` staat, vervalt de vochtstem onmiddellijk — anders dan bij de temperatuur-as, die op stap 1 blijft staan tot `t_max − hyst_t`.

**Combineren**, in deze volgorde:

1. doet de luchtvochtigheid-as niet mee (meetwaarde tussen `rh_min` en `rh_max`, of vochtregeling uit) → de temperatuur-stap geldt;
2. vragen beide om ventileren (allebei groter dan 0) → de **hoogste** van de twee, ongeacht de conflict-prioriteit;
3. vragen beide hetzelfde → die stap;
4. anders is het een echt conflict en beslist de **conflict-prioriteit**.

| Situatie | `0` Temperature first (default) | `1` Humidity first | `2` Auto |
|---|---|---|---|
| T boven setpoint, RH binnen de band | stap van de T-as | stap van de T-as | stap van de T-as |
| T boven setpoint, RH boven `rh_max` | hoogste van beide | hoogste van beide | hoogste van beide |
| T binnen de band, RH boven `rh_max` | **stap 0 — ramen dicht** | stap van de RH-as | stap van de RH-as |
| T boven setpoint, RH onder `rh_min` | stap van de T-as | **stap 0 — ramen dicht** | stap van de T-as |

De derde regel is de belangrijkste voor de praktijk: met de default opent een koele, klamme kas geen enkel raam, ook niet bij 90 % RV. Wie dat wel wil, zet de prioriteit op `1` of `2`. Bij `1` sluit een te droge kas de ramen ook wanneer het te warm is; bij `2` gebeurt dat niet, want daar wint de hoogste stap.

**In mode 2 — lineaire besturing van M3.** De vochtvraag werkt alles-of-niets op M3: bij stap 3 vraagt de vochtregeling M3 volledig open, daaronder volgt M3 de temperatuurvraag. De snelheidsbegrenzing van de lineaire regeling geldt ook voor die vochtvraag — hoogstens 25 % per beweging en tien minuten tussen twee bewegingen — zodat M3 pas na vier bewegingen, ruim een half uur, volledig open staat. In mode 1 opent M3 in één beweging.

### Dag/nacht-omschakeling

- **Berekening** van dag/nacht vindt automatisch op basis van de **datum en geografische locatie**: zonsopkomst en zonsondergang
- **Geografische locatie** (`latitude`, `longitude`) wordt **automatisch bepaald op basis van de internet-aansluiting** (geo-lookup via [ip-api.com](ip-api.com) eerste verbinding) en wordt daarna in het permanente geheugen bewaard. De beheerder kan deze handmatig overschrijven via System-tab → Location
- **Tijdzone** (`tz_str`) — POSIX-formaat, default voor Nederland: `CET-1CEST,M3.5.0,M10.5.0/3`*Wordt automatisch bepaald bij locatiebepaling.* De beheerder kan deze handmatig overschrijven via System-tab → NTP timezone → POSIX TZ string
- **Tijd** synchroniseert via NTP zodra WiFi-client is verbonden; bij geen verbinding gebruikt controller de interne klok

> *Opgelet:* Wanneer er wordt gebruik gemaakt van een mobiele internet verbinding kan de locatie flink afwijken. 

### Reboot-vereiste parameters

Niet alle parameters zijn direct na het aanpassen actief; sommige vereisen een power-cycle (Schakel het apparaat uit en weer in):

| Parameter | Reboot vereist? |
|---|---|
| Climate setpoints, hyst, avg_win, rh_ctrl_en, cr_priority | Nee — direct actief |
| Wind v_max, dir_excl_low/high, wind_hyst, Wind protection | Nee — direct actief |
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

![Kast vooraangezicht](images\kasControllerFrontView.png)

*Figuur 4: Kas controller vooraanzicht.*

### De kast
- Schroefverbinding aan de wand met M5/M6 schroeven (placeholder)
- Kabelinvoer onderzijde, met drukwartels voor sensoren, motor-bedrading, voeding
- IP-rating: IP67

### 5.1 LCD-display

Voor algemene uitleg over schermen, auto-rotatie en mode-regel: zie de [boer-handleiding §5.1](boerHandleiding.md#51-lcd-display).

**Aanvullend voor de beheerder**:

 - **Achtergrondkleur LCD-display** geeft de veiligheidsstatus van de controller weer:
 - **Blauw** — normale werking (geen alarmen actief)
 - **Rood** — kritiek: **motor-alarm** OF **wind-alarm** OF **sensor-fault Temperatuur/Luchtvochtigheid**

### 5.2 Toetsenbord
Zie [boer-handleiding §5.2](boerHandleiding.md#52-toetsenbord-4--4) voor het volledige overzicht van toetsfuncties per scherm. Geen Beheerder-specifieke uitbreidingen.

### 5.3 LED-indicatoren

#### RGB-LED kleuren
Zie [boer-handleiding §5.3](boerHandleiding.md#53-led-indicatoren). 

> Nachtmodus voorkomt een felle LED in een verduisterde kas of woonruimte naast de kas. De nachtstand loopt van 22:00 tot 06:00 en ligt vast; sinds 2.14.0 is hij niet meer in te stellen (gh#67).

#### Heartbeat-LED
groene LED Knippert 1× per seconde. **Knippert niet:**
- Controleer groene voedling LED; bij uit geen spanning.
- Probeer power-cycle of RESET-knop
- Bij blijvende uitval: Neem contact op met de leverancier (zie [§14](#14-onderhoud--wat-de-beheerder-doet))

---

## 6. De webinterface (via WiFi)

De webinterface is de primaire beheerdersinterface. Veel beheers-functies zijn alleen hier beschikbaar (niet via de bediening op de kast).

### Bereiken webinterface

Dezelfde route als voor de boer: lees IP-adres af van het LCD, het WiFi-scherm, open browser op laptop op hetzelfde WiFi-netwerk, typ het IP-adres in dat wordt getoond op het LCD-display.

> Voor eerste-installatie zonder bestaand WiFi-netwerk: gebruik de AP-modus — zie [§11 WiFi installatie](#11-WiFi-en-webinterface--installatie-en-beheer).

### Hoofdtabs en role-vereisten

| Tab | Rol | Functies |
|---|---|---|
| **Status** | Iedereen | Live Temparatuur, Luchtvochtigheid, wind snelheid en richting, raamposities, mode, alarmen, klok, WiFi, SD-kaart informatie |
| **Climate** | Boer + Beheerder | Setpoints; *Beheerder ziet ook hyst, avg_win, rh_ctrl_en* |
| **Wind** | Boer + Beheerder | wind_prot_en; *Beheerder ziet ook v_max, dir_excl_low/high, avg_win_wind, wind_hyst* |
| **Motors** | **Beheerder** | M1/M2/M3 travel + dwell open/close |
| **System** | **Beheerder** | Sessie-timeout, AP-config, WiFi-client, NTP/timezone, locatie coordinaten, Over the Air Update (OTA) |
| **Access** | **Beheerder** | PIN-management voor Farmer + Beheerder |
| **Log** | **Beheerder** | SD-kaart mount/unmount, log-bestanden downloaden |
| **Web** | **Beheerder** | Instellingen voor on-line status pagina |
| **Sensor history** | Iedereen | laatste sensormetingen en gemiddelde waarden |

![SCHERMAFBEELDING: webinterface met alle 7 tabs zichtbaar voor Beheerder](images\kasControllerWebGUIAllTabsBeheerder.png)

*Figuur 5: webinterface met alle tabs zichtbaar voor Beheerder*

### Status-tab — Klok-tegel

Op de Status-tab staat in de linker tegelrij de **Klok-tegel (Clock)**. Deze toont drie regels:

- **Tijd** — actuele lokale datum en tijd op de controller (format `YYYY-MM-DD HH:MM:SS`). De tijdzone wordt automatisch ingesteld na NTP-sync via geolocation, of handmatig via System-tab → NTP timezone.
- **NTP-badge** — `NTP synced` (groen) wanneer de klok deze sessie via NTP gesynchroniseerd is, `NTP pending` (rood) zolang dat nog niet gelukt is en de controller op de interne klok RTC met batterij-backup draait.
- **Uptime** — bedrijfsduur sinds de laatste start, ververst om de ~2 seconden. 

> Bij een herstart van de controller springt deze waarde naar `0s` en begint opnieuw — handig om te zien of de controller stabiel draait.

### Alarms-tegel — overzicht van alle badges

De Alarms-tegel verzamelt elke actieve waarschuwing of bedrijfsstaat als een gekleurde badge. De kleur geeft de ernst aan; de tekst zegt waarop het slaat. Geen actieve waarschuwingen → groene **OK**-badge.

De kleuren van de badg geven de urgentie weer van de melding: 
 - 🔴 Rood (alarm)
 - 🟡 Geel (waarschuwing)
 - 🔵 Blauw (informatie, herinnering)

| Kleur | Badge | Betekent | Operator-actie |
|---|---|---|---|
| 🔴 | **MOTOR ALARM** | Noodstop door RRK-3 — de besturing van de ramen is gestaakt | Diagnose: zie [§12.2](#122-motor-alarm--diagnose) |
| 🔴 | **WIND** | Wind-override actief — alle ramen dicht door harde wind | Wachten tot windsnelheid zakt; controleer `v_max` als de override te snel/vaak triggert |
| 🔴 | **MISMATCH** | Firmware-versie ≠ web-assets-versie (onvolledige OTA) | Harde browser-refresh; bij blijven: OTA opnieuw met beide pakketten |
| 🟡 | **T/RH fault** | Twee opeenvolgende mislukte uitlezingen van de Temperatuur/Luchtvochtigheid-sensor | Zie [§12.3](#123-sensor-fault--diagnose) |
| 🟡 | **Wind fault** | Twee opeenvolgende mislukte uitlezingen van de windsensor | Zie [§12.3](#123-sensor-fault--diagnose) |
| 🟡 | **OTA active** | OTA-update loopt nu | Wacht tot voltooid; geen handeling vereist |
| 🟡 | **Calibrating** | Window-Cal: ramen worden gesloten om positie vast te leggen | Wacht ~3 min; geen handeling vereist |
| 🟡 | **Standby** | Automatische klimaatregeling gepauzeerd | Zelf aangezet via Scherm 3 of de web GUI: blijft aan tot je hem uitzet, ook na een herstart. Aangezet door het handmatige raammenu op de LCD ([§10.11](#1011-handmatige-raambediening-via-de-lcd-beheerder)) of door een teach: gaat vanzelf uit zodra die beheerderssessie eindigt (uitloggen, 5 min zonder toetsdruk, of een herstart). Bij het uitgaan sluiten de ramen één keer om te kalibreren |
| 🟡 | **Net backoff** | Status-website onbereikbaar — Updates zij tijdelijk gestopt na herhaalde fouten | Controleer netwerk + URL; herstelt automatisch |
| 🟡 | **Wind protect off** | Boer/Beheerder heeft windbeveiliging uitgezet | Bewust — controleer of dit zo bedoeld is; ramen worden niet meer dichtgestuurd bij wind |
| 🔵 | **Humidity ctrl off** | Boer/Beheerder heeft de luchtvochtigheid-regeling uitgezet | Bewust — alleen temperatuur stuurt nu de ramen |
| 🔵 | **Coredump available** | De firmware is gecrashed *panic*; er staat een coredump flash die gedownload kan worden voor analyse | Tab Log → Diagnostics — zie [§14 Coredump ophalen na een panic](#coredump-ophalen-na-een-panic) |

### Status-tab — actieve setpoints op de tegels

Naast de Temperature-, Humidity- en Wind-tegels tonen naast de actuele meting ook de **op dit moment actieve setpoint(s)**. "Actief" wil zeggen: de dag- of nacht-waarde die op dit moment in werking is, geselecteerd op basis van zonsopkomst/zonsondergang.

| Tegel | Extra regel(s) |
|---|---|
| Temperature | **Setpoint** — actieve `T-max` (boven deze waarde gaan ramen open) |
| Humidity | **Setpoint max** + **Setpoint min** — actieve `RH-max` / `RH-min` |
| Wind | **Variation** — de hoekbreedte (in graden) waarbinnen alle recente windrichting-metingen liggen; klein getal = stabiele wind, groot getal = sterk wisselende richting |

**Humidity-tegel bij uitgeschakelde RH-regeling**: wanneer de onder Climate → Humidity-control op *Off* is gezet, worden de twee **Setpoint max / Setpoint min**-regels op de Humidity-tegel *grayed* — de waarden blijven leesbaar zodat de gebruiker nog kan zien wat geconfigureerd is, maar zijn duidelijk inactief. Het externe dashboard toont deze velden in dat geval niet. 

### Sensorhistorie-tabel onderaan de pagina

De sensorhistorie-tabel onder de Status-tegels toont acht kolommen: **Time · T · T-avg · RH · RH-avg · Wind · Wind Avg · Direction · Variation**. Elke rauwe meting staat naast zijn glijdend-gemiddelde.

### LCD-statusschermen — overzicht

Op het LCD-scherm roteren **zeven** statusschermen. Elk scherm staat 5 seconden, daarna volgt het volgende; na scherm 7 begint de cyclus opnieuw bij 1.

| # | Scherm | Inhoud rij 1 / rij 2 |
|---|---|---|
| 1 | Temp/RH | `Temp: 23 °C` / `  RH: 65 %` |
| 2 | Wind | `Wind: 2.3 m/s` / ` Dir: 180 ° (S )` |
| 3 | Mode/Sess | `Mode: AUTO` / `Sess: NONE` (mode kan ook STANDBY, WIND, ALARM of Window Cal. zijn) |
| 4 | WiFi | `WiFi: connected` / `192.168.20.150` |
| 5 | Tijd | `06-05-2026 14:30` / `Src:NTP      Day` |
| 6 | Raamposities | `M1    M2    M3 ` / `OPEN  CLOS  MOV>` |
| 7 | Firmware + Uptime | `FW: 2.1.1` / `Up: 1d 4h 23m` |

### LCD — D-toets als directe terugkeer + 5-minuten time-out

- **D-toets als Quick-jump**: vanaf elk menu, bladermenu, PIN-invoer of bewerk-scherm springt **één druk op `D`** terug naar de roterende statusschermen. 
- **5-minuten auto-terugkeer**: blijft het LCD 5 minuten lang op een menu of invoer staan zonder dat een toets wordt ingedrukt, dan keert het display automatisch terug naar de auto-rotatie schermen. Dit is onafhankelijk van de sessie-time-out en werkt ook zonder een ingelogde gebruiker, zodat de display niet permanent blijft hangen op een halve invoer als iemand wegloopt.

---

## 7. De twee gebruikersrollen

### Boer (kasgebruiker)
- 4-cijferige PIN
- Fabrieksstandaard: `1234` — moet bij eerste gebruik worden gewijzigd door de boer of door de beheerder namens de boer in de webinterface. 
- **Mag** klimaat-setpoints en windbeveiliging instellen, eigen PIN wijzigen (web)
- Kan niet bij motor-, WiFi-, systeem-, toegang- of log-instellingen

### Beheerer (Technisch beheerder van installatie)
- 8-cijferige PIN
- **Geen default in deze handleiding genoemd** — wordt door installateur ingesteld bij oplevering, de webinterface door de beheerder of via fysieke reset op fabrieksstandaard teruggezet (zie [§18](#18-reset-procedure-io0-knop-op-microprocessorboard))
- Volledige toegang tot alle beheers-functies

### Lockout
- 5 opeenvolgende foute PIN-pogingen → 5 minuten lockout voor die rol
- Lockout-teller en -duur per rol apart

### PIN-opslag
- PIN's worden opgeslagen als **salted SHA-256 hash** in permanent geheugen
- De salt is 16 bytes random getal, en wordt gegenereerd bij eerste boot
- Plaintext-PIN wordt nooit opgeslagen of gelogd

### PIN-management voor de Beheerder (webinterface, Access-tab)

![Access-tab met PIN-change formulieren voor Farmer en Beheerder](imagesBeheerder\kasControllerWebGUIAccessTab.png)

*Figuur 6: Access-tab met PIN-change formulieren voor Farmer en Beheerder* 

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
> **Beheerder is zijn PIN vergeten** → fysieke reset niveau 1 op de IO0-knop (zie [§18](#18-reset-procedure-io0-knop-op-microprocessorboard)).

### Sessie-timeout instellen
- Default: 5 min
- Bereik: 1–1440 min (1 dag)
- Webinterface: System-tab → veld → `session timeout (min)`
- Geldt voor zowel Boer- als Beheerder-sessies

---

## 8. Gebruik zonder inloggen — informatiemenu

Geen beheer-specifieke verschillen. Zie [boer-handleiding §8](boerHandleiding.md#8-gebruik-zonder-inloggen--informatiemenu) voor de volledige uitleg.

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

### Quick-jump via #-toets

Op **zes** statusschermen kan je direct vanuit de auto-rotatie naar een instellingen-menu springen, waarbij PIN-invoer eerst wordt gevraagd als je nog niet ingelogd bent. Alleen op scherm 7 (Firmware/uptime) heeft `#` geen aparte functie en opent het, net als andere toetsen, het hoofdmenu.

- **T/RH-status (scherm 1)** — `#` → vraagt **Boer-PIN** (4 cijfers) → daarna direct in Climate-menu (Day/Night setpoints + Conflict-prioriteit)
- **Wind-status (scherm 2)** — `#` → vraagt **Boer-PIN** → daarna direct in Wind-menu (`Wnd-max`, `Wnd-prot`)
- **Mode/Sess (scherm 3)** — `#` → vraagt **Boer- óf Beheerder-PIN** (de PIN-lengte bepaalt welke rol wordt geverifieerd: 4 cijfers = Boer, 8 cijfers = Beheerder) → daarna direct in Standby-toggle (`1=Auto 2=Stby *Bk`). Zie [§10.10](#1010-standby-modus--controller-tijdelijk-pauzeren) voor wanneer en hoe Standby gebruikt wordt.
- **WiFi-status (scherm 4)** — `#` → vraagt **Beheerder-PIN** (8 cijfers) → daarna direct in System-menu (waar je AP kunt aan/uit zetten)
- **Time-status (scherm 5)** — `#` → vraagt **Beheerder-PIN** → daarna direct in datum/tijd-invoer
- **Raamposities (scherm 6)** — `#` → vraagt **alléén Beheerder-PIN** (8 cijfers) → daarna in een twee-trapsmenu om M1/M2/M3 handmatig open of dicht te zetten. Zie [§10.11](#1011-handmatige-raambediening-via-de-lcd-beheerder) voor de volledige procedure, veiligheidsregels en wat er met de autonome regeling gebeurt tijdens en na de sessie.

> **Let op**: ben je al ingelogd als boer, dan zal `#` op de WiFi-, Time- of Raamposities-status alsnog om de Beheerder-PIN vragen (deze schermen zijn admin-only). Op scherm 3 (Mode/Sess) wordt de boer-sessie direct geaccepteerd — Standby is een normale operationele toggle die de boer ook mag bedienen. Andersom werkt voor een Beheerder elk van de zes sneltoetsen direct zonder extra PIN-invoer.

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

De LCD-route voor de door de **boer-bewerkbare** instellingen (T-max dag/ngt, RH-max/min dag/ngt, T vs RH conflict priority) staat in de [boer-handleiding §10.1](boerHandleiding.md#101-op-de-kas-controller).

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

![SCHERMAFBEELDING: tab Climate, ingelogd als Beheerder, met Beheerder-only sectie zichtbaar](imagesBeheerder\kasControllerWebGUIClimateTab.png)

*Figuur 7: Climate-tab, ingelogd als Beheerder, met Beheerder-only sectie zichtbaar*

#### Mode-keuzelijst (bovenaan, Boer + Beheerder)

Bovenaan de Climate-tab staat een aparte "Mode"-keuzelijst met **Normal (autonomous)** of **Standby (paused)**. Hiermee pauzeer je de autonome klimaatregeling zonder andere setpoints aan te raken. Zie [§10.10](#1010-standby-modus--controller-tijdelijk-pauzeren) voor wanneer/hoe je dit gebruikt. De keuzelijst is automatisch grijs gemaakt (uitgeschakeld) wanneer **Wind-override**, **Motor-alarm** of **Window Cal.** actief is: in die situaties domineert het veiligheids-/early-boot-mechanisme over operator-intentie.

#### Boer-bewerkbare velden (Boer + Beheerder)

Per setpoint: schuifregelaar + nummerveld + **Apply**-knop.

| Label | Default | Bereik |
|---|---|---|
| Temperatuur max dag (°C) | 28 | 15–45 |
| Temperatuur min dag (°C) | 16 | -20–60 |
| Luchtvochtigheid (RH) max dag (%) | 75 | 40–98 |
| Luchtvochtigheid (RH) min dag (%) | 50 | 20–90 |
| Temperatuur max nacht (°C) | 20 | 10–35 |
| Temperatuur min nacht (°C) | 14 | -20–60 |
| Luchtvochtigheid (RH) max nacht (%) | — | 40–98 |
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

![SCHERMAFBEELDING: tab Wind, ingelogd als Beheerder](imagesBeheerder\kasControllerWebGUIWindTabBeheerder.png)

*Figuur 8: Wind-tab, ingelogd als Beheerder*

#### Boer en Beheerder
|  Label |  Default | Bereik |
|---|---|---|
| Wind protection | Aan | Uit/Aan |

#### Alleen Beheerder

| Label |  Default | Bereik | Eenheid |
|---|---|---|---|
| Wind speed max | 6 | 1–30 | m/s |
| Dir excl. zone low | — | 0–359 | ° |
| Dir excl. zone high | — | 0–359 | ° |
| Wind avg window | 6 | 1–30 | min. |
| Wind hysteresis | 1 | 0–5 | m/s |

> **Wind gemiddelde window (Wind avg window)**: het aantal minuten waarover de windsnelheid en -richting worden gemiddeld voordat ze worden vergeleken met `v_max`. Een korter venster reageert sneller op windstoten; een langer venster dempt toevallige pieken. Standaard 6 min. (12 metingen bij 30 s poll-interval). Onafhankelijk van het klimaat-gemiddelde (`avg_win_t`).

> **Wind hysteresis** *(sinds 2.3.0)*: dode band onder `v_max` voordat een actieve wind-override wordt vrijgegeven. De ramen sluiten zodra het windgemiddelde `v_max` bereikt, maar gaan pas weer open als het gemiddelde onder `v_max − wind_hyst` zakt. Dit voorkomt herhaald sluiten/openen (motorslijtage — M3 doet 171 s over een volledige slag) wanneer de wind rond de grens blijft hangen. Standaard 1 m/s; `0` = direct vrijgeven onder `v_max` (oud gedrag).

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

![SCHERMAFBEELDING: tab Motors met M1, M2, M3 instellingen](imagesBeheerder\kasControllerWebGUIMotorTabBeheerder.png)

*Figuur 9: Motors-tab met M1, M2, M3 instellingen*

| Veld | Default M1 | Default M2 | Default M3 | Bereik |
|---|---|---|---|---|
| Travel time |  21 s | 21 s | 171 s | 5–300 s |
| Dwell open | 300 s | 300 s | 1500 s | 0–1500 s |
| Dwell close | 300 s | 300 s | 600 s | 0–1500 s |

> **Travel-time afstemming**: meet de werkelijke open- of sluit-tijd van een raam met een stopwatch. Stel die waarde in als *travel-time.* De firmware voegt zelf een veiligheidsmarge toe van 5 sec. De controller gebruikt deze waarde als time-out voor het OPEN/CLOSE-relais.
>
> **Dwell-tijden aanpassen**: bij oscillatie (raam gaat steeds open/dicht in een korte cyclus) → dwell-tijd verhogen. Bij trage reactie op klimaat-veranderingen → dwell verlagen. Begin met de standaar instellingen; pas deze waarden alleen aan na minimaal 1 dag observeren.

### 10.5 System-tab (alleen Beheerder)

![SCHERMAFBEELDING: tab System, ingelogd als Beheerder](imagesBeheerder\kasControllerWebGUISystemTabBeheerder.png)

*Figuur 10: System-tab, ingelogd als Beheerder — alle systeem-instellingen op één plek*

De System-tab bundelt alle systeem-instellingen die niet direct aan de klimaatregeling raken: netwerk, klok, locatie, sessies en firmware-updates. Eerste-installatie van WiFi loopt via een combinatie van de LCD en deze tab (zie [§11 Eerste-installatie WiFi-verbinding](#11-eerste-installatie-wifi-verbinding) voor de stap-voor-stap procedure).

#### WiFi AP (Access Point op de controller)

De controller kan een eigen tijdelijk WiFi-netwerk uitzenden zodat de beheerder ook zonder bestaand WiFi-netwerk de webinterface kan bereiken. Aanbevolen alleen voor installatie en onderhoud — niet voor dagelijks gebruik.

| Veld | Beschrijving | Default | Bereik / regels |
|---|---|---|---|
| `AP password` | WPA2-wachtwoord voor het AP | `0123456789` | 8–63 tekens |
| `AP timeout (min, 0=never)` | Min. tot AP automatisch uitschakelt | 30 | 0 = nooit · 1–1440 min. |

> **Sterk aangeraden bij installatie**: wijzig het AP-wachtwoord direct van de fabriekswaarde `0123456789` naar iets unieks per kas. Zonder wijziging is iedereen die de fysieke kas nadert in staat verbinding te maken.

> `AP timeout` voorkomt dat een vergeten AP-modus permanent open blijft staan — bij `0` blijft het AP open totdat u het handmatig uitschakelt.

#### WiFi client (verbinding met uw eigen netwerk)

| Veld | Beschrijving | Bereik / regels |
|---|---|---|
| `SSID` | Naam van het WiFi-netwerk dat de controller moet gebruiken | max 32 tekens |
| `Password` | Wachtwoord (WPA2-PSK) van dat WiFi-netwerk | 8–63 tekens · leeg laten bij Apply = bestaande PSK blijft staan |

Klik **Connect** om de verbinding tot stand te brengen. De controller probeert ~30 sec.; bij succes verschijnt het verkregen IP-adres op LCD-status-scherm 4 en is de webinterface op dat adres bereikbaar.

> **Statisch IP**: de firmware ondersteunt **geen** statisch IP-adres. Gebruik een **DHCP-reservering** op uw router (op MAC-adres) als u een vast adres wilt.

> **mDNS / hostname**: de controller registreert zichzelf niet op de lokale DNS. Gebruik altijd het IP-adres of maak zelf een DNS-record in uw router.

#### NTP en tijdzone

| Veld | Beschrijving | Default | Voorbeeld |
|---|---|---|---|
| `POSIX TZ string` | POSIX-tijdzone-notatie | (Nederlandse tijdzone) | `CET-1CEST,M3.5.0,M10.5.0/3` |

- **NTP-synchronisatie**: automatisch zodra de WiFi-client verbonden is. Default server: [pool.ntp.org](pool.ntp.org); niet configureerbaar.
- **Tijdzone-wijziging**: een aanpassing in dit veld is direct actief, een reboot is niet nodig.
- **Zomertijd**: wordt automatisch bepaald uit de POSIX-TZ-string (zie default Nederland).
- **Tijd handmatig instellen** (wanneer u geen internet-toegang heeft): op de controller via LCD-tijdscherm (scherm 5) → `#` → Beheerder-PIN → datum DDMMYY → `#` → tijd HHMM → `#`.

#### Geografische locatie (Location)

Wordt gebruikt voor het berekenen van zonsopkomst en zonsondergang, en daarmee de automatische dag/nacht-omschakeling van de klimaat-setpoints.

| Veld | Default | Eenheid |
|---|---|---|
| `Latitude` | 52.0 | ° (Nederland; positief = N, negatief = S) |
| `Longitude` | 5.0 | ° (Nederland; positief = E, negatief = W) |

**Automatische detectie**: bij eerste WiFi-verbinding doet de controller een geo-lookup via [ip-api.com](ip-api.com) en vult `latitude` / `longitude` zelfstandig in. De waarden worden in permanent geheugen bewaard.

> Wanneer de WiFi-verbinding via een mobiele hotspot loopt, kan de geo-lookup onverwachte resultaten opleveren (de provider-locatie wordt teruggegeven, niet uw kas-locatie). Pas in dat geval handmatig de locatie aan.

**Handmatig aanpassen**: velden `latitude` / `longitude` rechtstreeks editen. Decimale graden met teken — bijv. `52.218` voor 52°13′05″ N, `5.939` voor 5°56′21″ E.

#### Sessie-timeout

| Veld | Beschrijving | Default | Bereik |
|---|---|---|---|
| `Session timeout (min)` | Idle-timeout voor de webinterface en de LCD-menu | 5 min. | 1–1440 min. |

Geldt voor zowel LCD als webinterface. Een te lange waarde (bv. 60 min) is een veiligheidsrisico — een vergeten ingelogde sessie kan worden misbruikt.

#### OTA — firmware-update

![SCHERMAFBEELDING: tab System - OTA/ROTA, ingelogd als Beheerder](imagesBeheerder\kasControllerWebGUISystemTabBeheerder.png)

*Figuur 11: System-tab, ingelogd als Beheerder — alle systeem-instellingen op één plek: OTA instellingen*


In de System-tab staat ook de sectie **OTA update** met twee upload-knoppen: één voor het firmware-binair (`.bin`) en één voor de web-assets-ZIP. De volledige update-procedure inclusief verificatie-stappen staat in [§14 Firmware-update / OTA](#firmware-update--ota).

#### Remote update (ROTA) — automatische internet-update

Naast de handmatige upload hierboven kan de controller firmware- en asset-updates **zelf ophalen** van een internet-updateserver die door de **fabrikant wordt beheerd** (ROTA — *Remote OTA*). Zo werkt een unit zichzelf bij zonder bezoek ter plaatse — ook een unit die achter NAT staat. De sectie **Remote update (ROTA)** in de System-tab is **alleen zichtbaar voor de Beheerder**; de Boer kan deze instellingen niet zien of wijzigen.

| Veld | Beschrijving | Bereik / standaard |
|---|---|---|
| **Enable** | Zet automatische internet-updates aan of uit. Uit = de controller neemt nooit contact op met de updateserver. | uit (standaard) |
| **Check interval (h)** | Hoe vaak de controller de updateserver controleert op een nieuwere versie, in uren. | 1–168, standaard 24 |
| **Server URL** | Basis-URL van de updateserver. **Moet `https://` zijn.** | — |
| **Secret** | Toegangssleutel voor de updateserver, door de fabrikant verstrekt. **Wordt nooit getoond**; laat leeg om de opgeslagen waarde te behouden. | — |
| **Apply window (local h)** | Nachtvenster (lokale uren) waarin een gedownloade update geïnstalleerd mag worden en de controller mag herstarten. Beide velden gelijk = venster uitgeschakeld (installeren zodra geverifieerd). | 0–23, standaard 02–04 |
| **Server cert (PEM)** | Optioneel servercertificaat, door de fabrikant verstrekt. Laat leeg om het huidige te behouden. | — |
| **Last check** | Alleen-lezen: uitkomst van de laatste controle (bijv. *Up to date*, *Update available*, *Server unreachable*), de draaiende en aangeboden versie, en het tijdstip. | — |
| **Check now** | Vraagt de controller nu direct de updateserver te controleren. | — |

Klik **Apply ROTA settings** om op te slaan (validatie-dan-schrijven: een lege *Secret* of *Cert* laat de opgeslagen waarde ongewijzigd).

**Werking.** Als ROTA aanstaat controleert de controller periodiek de server. Vindt hij een nieuwere, geldige release, dan downloadt en **verifieert** hij beide bestanden (SHA-256 + grootte) vóór er iets naar flash wordt geschreven. De installatie (en herstart) gebeurt **alleen binnen het nachtvenster** en alleen als het rustig is (geen bewegende ramen, geen wind- of motoralarm, geen actieve web- of LCD-sessie). Buiten die voorwaarden wordt de update **uitgesteld** en de volgende nacht opnieuw geprobeerd. Zolang een geverifieerde update op zijn venster wacht, verschijnt er een blauwe badge **Update pending** in het Alarms-kaartje.

**Kanalen.** Een unit kan meedraaien op het **hoofdkanaal** (de reguliere, vrijgegeven versie) of op het **ontwikkelkanaal** (*soak* — nieuwe versies die eerst op een testunit worden beproefd voordat ze breed worden uitgerold). Welk kanaal een unit volgt, wordt door de fabrikant **op de server** ingesteld — niet op de unit zelf. De controller haalt eenvoudig de versie op die de server voor die unit aanbiedt.

> De verbinding met de updateserver is beveiligd en de fabrikant beheert de server en de toegang van elke unit. Als Beheerder hoeft u alleen ROTA in te schakelen en desgewenst het nachtvenster in te stellen; de overige verbindingsgegevens worden door de fabrikant verstrekt.

---

### 10.6 Access-tab (alleen Beheerder)

![SCHERMAFBEELDING: tab Access, ingelogd als Beheerder](imagesBeheerder\kasControllerWebGUIAccessTab.png)

*Figuur 12: Access-tab — PIN-beheer voor beide gebruikersrollen*

In de Access-tab wijzigt u de PIN-code van de Boer en die van de Beheerder. Zie [§9 PIN-management voor de Beheerder (webinterface, Access-tab)](#pin-management-voor-de-beheerder-webinterface-access-tab) voor de volledige procedure en de regels rondom lockout.

| Veld | Beschrijving | Bereik |
|---|---|---|
| `New PIN (4 digits)` — Farmer | Nieuwe Boer-PIN | exact 4 cijfers |
| `New PIN (8 digits)` — Admin | Nieuwe Beheerder-PIN | exact 8 cijfers |

> **Eigen Beheerder-PIN wijzigen kan alleen vanaf de webinterface** — niet vanaf de LCD. De LCD-menu's bieden geen PIN-wijzigings-functie. Voor het wijzigen van een Boer-PIN is een Beheerder-sessie vereist.

> **Beheerer-PIN vergeten** is niet zonder fysiek toegang oplosbaar. Zie [§18 Reset-procedure (BOOT-knop)](#18-reset-procedure-io0-knop-op-microprocessorboard) voor de fabrieksreset die ook de PIN's resetten.

De **Logout**-knop verschijnt op de Access-tab wanneer u ingelogd bent (Boer of Beheerder); klik hierop om de sessie direct te beëindigen, anders verloopt deze na de in System ingestelde sessie-timeout.

---

### 10.7 Log-tab (alleen Beheerder)

![SCHERMAFBEELDING: tab Log, ingelogd als Beheerder](imagesBeheerder\kasControllerWebGUILogTabBeheerder.png)

*Figuur 13: Log-tab — SD-kaart status en logbestand-download*

De Log-tab biedt toegang tot het event-logbestand-systeem. De kascontroller schrijft alle relevante gebeurtenissen (sensor-readings, raam-bewegingen, mode-wisselingen, alarmen, configuratie-wijzigingen) naar de **SD-kaart** in CSV-formaat. De firmware begint automatisch een nieuw bestand zodra het huidige **1 MB** groot is, en bewaart **30 bestanden per controller**: is die grens bereikt, dan verwijdert hij bij elke wissel de oudste, tot vijf tegelijk, tot hij weer op 30 zit. Bestanden van een andere controller op dezelfde kaart — een andere ID vooraan in de bestandsnaam — verwijdert hij nooit. *Tot en met 2.12.0 werkte dat opruimen niet: een kaart met meer dan 30 bestanden liep ongemerkt vol, en de lijst toonde alleen de oudste (gh#82).*

#### Velden en knoppen

| Element | Beschrijving |
|---|---|
| `SD card control` — **Mount** / **Unmount** | Handmatig mounten/unmounten van de SD-kaart |
| `Log source` (keuzelijst) | Kies een SD-bestand om te downloaden. **Nieuwste bovenaan**: eerst de bestanden van deze controller, daarna die van een andere. Het bestand waar de controller nu in schrijft, is gemarkeerd als *(current)* en al geselecteerd. Staan er meer dan 64 bestanden op de kaart, dan sluit de lijst af met een grijze regel *— N older file(s) on the card, not listed —*, zodat je ziet dat de lijst niet alles toont. Zonder SD-kaart toont de lijst `— no SD log files —` |
| **Download CSV** | Download het gekozen SD-bestand als CSV |
| Refresh-knop (↻) | Vernieuwt de lijst beschikbare SD-bestanden |

#### SD-kaart eisen

- **Bestandssysteem**: FAT32 (verplicht). exFAT en NTFS worden niet ondersteund
- **Capaciteit**: in de praktijk **max 32 GB** (SDXC-kaarten worden standaard als exFAT geformatteerd en moeten handmatig naar FAT32 worden gezet)
- **Klasse / snelheid**: geen minimum vereist — Class 4 of hoger volstaat voor de geringe schrijfbelasting van logging

#### Automatisch mounten

De firmware probeert de SD-kaart **automatisch te mounten**:
- bij het opstarten (direct na boot, tijdens de event-logger-initialisatie)
- daarna elke 60 seconden zolang er geen kaart gemount is
- **behalve na Unmount**: heb je de kaart zelf met **Unmount** vrijgegeven, dan blijft hij vrijgegeven tot je op **Mount** klikt of de controller herstart. Zo kun je de kaart veilig verwisselen zonder dat de controller hem tussendoor weer in gebruik neemt (sinds 2.4.10, gh#61)

Plaats een SD-kaart tijdens bedrijf en binnen één minuut wordt er automatisch een mount-poging gedaan — een power-cycle is niet nodig.

> **Verplicht voordat u een SD-kaart fysiek verwijdert**: klik **Unmount** in de Log-tab. Anders kunnen de laatste log-events verloren gaan of kan het bestandssysteem corrupt raken.

> **Logbestand-formaat**: het CSV-formaat (kolomnamen, event-types en parameter-ID's) en het meegeleverde Python-script `log/logparser.py` om ruwe logs naar leesbare tekst om te zetten, staan beschreven in [Bijlage F — Logbestand-formaat en `logparser` script](#bijlage-f--logbestand-formaat-en-logparser-script).

#### Diagnostics — coredump

Onderaan de Log-tab staat de sectie **Diagnostics**. Deze toont of er een coredump beschikbaar is in de flash-geheugenpartitie van de controller.

| Element | Beschrijving |
|---|---|
| `Coredump` statusregel | `Available — N bytes (N KB) • captured on fw ...` als er een coredump aanwezig is; anders `Not present` |
| **Download** | Download de coredump als `.bin`-bestand voor offline analyse |
| **Erase** | Wist de coredump-partitie; doe dit pas **nadat** het bestand is gedownload |

Een coredump wordt automatisch opgeslagen wanneer de firmware een **panic** heeft (ongeldige geheugen-toegang, task-watchdog-timeout). Bij het opstarten na een panic verschijnt op de Status-tab de blauwe badge **Coredump available** en staat in het SD-logbestand een `SYSTEM`-regel `Coredump from previous panic detected in flash`.

> **Coredump offline ontleden** met ESP-IDF:
> ```
> idf.py coredump-info -t raw -c coredump-<fw-versie>-<tijdstempel>.bin firmware-<fw-versie>.elf
> ```
> Het `.elf`-bestand staat in de release-map `bin/<versie>/` van de repository.

> **Coredump-inhoud kan gevoelige data bevatten** (WiFi-PSK, PIN, status-secret). Behandel het `.bin`-bestand als vertrouwelijke informatie en wis het van het werkstation zodra de analyse klaar is.

Zie [§14 — Coredump ophalen na een panic](#coredump-ophalen-na-een-panic) voor de volledige procedure.

---

### 10.8 Web-tab (alleen Beheerder) — status-rapportage naar extern dashboard

![SCHERMAFBEELDING: tab Web, ingelogd als Beheerder](imagesBeheerder\kasControllerWebGUIWebTabBeheerder.png)

*Figuur 14: Web-tab — configuratie van status-rapportage naar een externe web-server*

De kascontroller kan zijn actuele toestand periodiek naar een **externe web-server** sturen. Op die web-server draait een dashboard dat dezelfde gegevens toont als de eigen webinterface — zo kan iemand op afstand toch de werking van de kas volgen. Daarnaast wordt het laatst-gesloten logbestand van de SD-kaart één keer per dag (en/of bij elke logrotatie) naar dezelfde server geüpload.

De feature staat **standaard uit**. Inschakelen gebeurt volledig in deze tab.

![ComponentDiagramStatusWebsite](imagesBeheerder\StatusWebsiteComponentDiagram.png)

*Figuur 15: Overzicht Status-website*

#### Velden op tab Web

| Veld | Beschrijving | Default | Bereik / regels |
|---|---|---|---|
| `URL` | Eindpunt van het PHP-script | leeg | Moet beginnen met `http://` of `https://`, mag géén `?` of `#` bevatten, móét eindigen op `api.php`. Maximaal 128 tekens. Leeg laten = functie uit. |
| `Shared secret` | Token in `sourceidentifier`-header | leeg | Minimaal 16 tekens. Leeg laten bij `Apply` = bestaande token blijft staan. Wordt nooit teruggetoond bij heropenen van het tabblad. |
| `Interval (s)` | Tijd tussen POST's | 240 | 60–300 |
| `Enabled` | Hoofdschakelaar | uit | aan / uit. Bij `uit` worden geen POST's verstuurd, ook niet als URL en token correct zijn ingevuld. |
| `Climate` / `Wind` / `Windows` / `Mode` / `Sun` / `System` (6 vinkjes) | Welke tegels worden meegestuurd | alle 6 aan | Een uitgevinkt vinkje laat het bijhorende JSON-object weg uit de POST → de tegel verschijnt automatisch niet op het publieke dashboard. |
| `Daily upload time` | Lokale tijd waarop log-upload geprobeerd wordt | 03:15 | uu : mm, 24-uur klok |
| `Upload on rotation` | Ook uploaden zodra T9 een logbestand sluit | aan | aan / uit |

#### Actuele informatie over de werking van deze functie

Onderaan tab Web staan drie regels die elke 5 seconden ververst worden (zolang u op dit tabblad staat). Ze tonen wat T14 zelf intern weet, ze zijn niet handmatig in te vullen:

| Regel | Inhoud | Voorbeeld |
|---|---|---|
| `Last post` | Datum/tijd en uitkomst van laatste status-POST | `OK 2026-05-10 14:30:22` of `FAIL 2026-05-10 14:30:22` |
| `Last log upload` | Idem voor laatste log-upload | `OK 2026-05-10 03:15:08` of leeg als nooit geprobeerd |
| `Last uploaded file` | Bestandsnaam van het laatst succesvol geüploade logbestand | `5C88_20260507143022.csv` |

De auto-refresh werkt alleen deze drie regels — uw invoer in `URL`, `Shared secret`, intervalkeuze of vinkjes wordt nooit overschreven terwijl u typt. Pas op het moment dat u op **Apply** klikt worden de waarden eerst gevalideerd, daarna naar permanent geheuge  geschreven en daarna teruggelezen, zodat de formuliervelden exact tonen wat er in permanente geheugen staat.

#### Eerste keer instellen — stap voor stap

1. Log in als Beheerder en open tab **Web**.
2. Vul de URL van het PHP-eindpunt in (bv. `https://uw-server.nl/hbwv/api.php`).
3. Vraag aan de beheerder van de web-server de waarde van het shared secret. Plak die in `Shared secret`. Laat het secret-veld leeg als u het later eens wilt wijzigen zonder het opnieuw te hoeven invullen.
4. Stel het update `Interval (s)` in op een geschikte waarde — Lager dan 120 s of hoger dan 300 s wordt door de validatie geweigerd.
5. Vink desgewenst tegels uit die u **niet** publiek wilt tonen. Standaard staan alle zes aan.
6. Eventueel `Daily upload time` aanpassen naar een rustig moment in uw netwerk (bv. nacht).
7. Vink `Enabled` aan.
8. Klik **Apply**.

Binnen één intervalperiode hoort de regel `Last post` op `OK …` te springen. Blijft hij op `FAIL …` of leeg staan? Zie de tabel *Veelgemaakte fouten* hieronder.

#### Functie tijdelijk uitschakelen

Twee opties:
- Vink `Enabled` uit en klik **Apply**. URL en token blijven bewaard.
- Maak het URL-veld leeg en klik **Apply**. De feature is dan ook uit, ongeacht de stand van `Enabled`.

Bij OTA-firmware-update worden status-POST's automatisch overgeslagen totdat de update klaar is — u hoeft niks handmatig uit te zetten.

#### HTTPS

Endpoints met `https://` worden ondersteund maar **de controller controleert het certificaat NIET** (de verbinding is versleuteld maar niet geauthenticeerd). Dat is een bewuste keuze: anders moest de firmware een actuele CA-bundel meedragen en periodiek updaten. De gedeelde token (shared secret) in de header is de eigenlijke authenticatie. Wijzig de token meteen als u vermoedt dat hij is gelekt.

#### Veelgemaakte fouten

| Symptoom | Oorzaak | Oplossing |
|---|---|---|
| Bij **Apply** verschijnt rood `URL must end with "api.php"` | URL eindigt op een directorypad zoals `/api/` | Voeg `api.php` toe; de HTTP-client volgt geen 301-redirects, dus de server-side `DirectoryIndex` kan niet vertrouwd worden |
| Rood `URL must not contain ? or #` | Query-parameters in de URL | Verwijder ze — de firmware voegt zelf `?action=log` toe voor de log-upload |
| Rood `secret too short` | Minder dan 16 tekens | Vraag de beheerder van de website om een langer token |
| `Last post` blijft `FAIL` | Server bereikbaar maar weigert ('wrong secret') | Controleer dat `Shared secret` byte-exact gelijk is aan het shared secret op de web-server. Spaties/tabs aan het einde tellen mee! |
| `Last post` blijft leeg | WiFi-client verbinding niet actief, of klok niet via NTP gesynchroniseerd | Zie [§11 Eerste-installatie WiFi-verbinding](#11-eerste-installatie-wifi-verbinding) of [§10.5 NTP en tijdzone](#ntp-en-tijdzone). T14 wacht op beide vóórdat hij verstuurt. |
| Publiek dashboard toont een tegel met verkeerde inhoud | Mismatch in veldnamen tussen kascontroller-firmware en het web-dashboard | Beide moeten van dezelfde release-generatie zijn. |

#### Logbestand-upload — wat gaat er precies heen?

De controller upload het **meest recent gesloten** CSV-logbestand **van deze controller** op de SD-kaart (dus niet het bestand waar T9 op het moment van uploaden nog in schrijft). De bestandsnaam is van de vorm `<eenheid-ID>_YYYYMMDDHHMMSS.csv` (eenheid-ID + lokale aanmaaktijd), bijvoorbeeld `5C88_20260507143022.csv`, en is maximaal 1 MB groot — daarboven heeft T9 het al gerouteerd naar een nieuwer bestand.

Twee triggers, beide aan te zetten of uit te zetten:
- **On rotation**: zodra T9 een logbestand sluit (omdat het 1 MB heeft bereikt), wordt het vrijwel direct geüpload.
- **Daily**: elke dag rond `Daily upload time` lokaal wordt het laatst-gesloten bestand opnieuw beoordeeld; staat het al onder `Last uploaded file`, dan wordt het overgeslagen — anders wordt het geüpload.

Door deze dubbele aanpak, met deduplicatie op het **aanmaaktijdstip** in de bestandsnaam, wordt hetzelfde bestand nooit twee keer geüpload. Bestanden van een andere controller op dezelfde kaart worden niet geüpload (sinds 2.12.2, gh#82; daarvoor vergeleek de controller namen, en kon een bestand van een andere controller de upload voorgoed laten stilvallen), ook als de rotatie en de dagelijkse check op verschillende dagen vallen.

---

### 10.9 Adviezen voor instelling per teelttype

Een tabel met richtwaarden per teelt is te vinden in [Bijlage G](#bijlage-g--aanbevolen-startinstellingen-per-gewas).


Algemene vuistregels:
- **Nacht-Temperatuur** iets lager dan dag-Temperatuur (planten besparen energie)
- **Luchtvochtigheid boven 85%** voor langere tijd; verhoogd schimmelrisico
- **Luchtvochtigheid onder 50%** kan groei remmen
- Begin met **default-waarden** en stel pas bij na een week observeren

### 10.10 Standby-modus — controller tijdelijk pauzeren

**Standby** is een door de operator geïnitieerde pauze van de autonome klimaatregeling. T6 (Climate Control) stopt met het uitvaardigen van openings- of sluitingscommando's; de ramen blijven in hun huidige positie staan zolang Standby actief is. De ramen worden **niet** automatisch gesloten bij Standby-entry — dat zou een werkschot betekenen dat de operator niet expliciet vroeg.

#### Wanneer gebruik je Standby?

| Situatie | Reden |
|---|---|
| Onderhoud aan de motoren, sensoren of raamconstructie | Voorkomt dat een autonome cyclus midden in jouw werk een raam in beweging zet |
| Tijdelijk handmatig overnemen via de motorbox (zie [§15](#15-handmatige-overname-via-de-motorbox)) | De controller probeert niet meer te corrigeren wat jij handmatig instelt |
| Demonstratie / rondleiding waarbij ramen in een bepaalde stand moeten blijven | Geen autonome wijzigingen tijdens het tonen |
| Een korte test of meting waarbij je een stabiele uitgangstoestand nodig hebt | Geen interferentie door T6-tikken |

#### Wat blijft wél actief tijdens Standby?

- **Wind-override** (Mode: WIND): bij een storing of harde wind sluit T3 alsnog alle ramen. Standby overschrijft veiligheid niet.
- **Motor-alarm** (Mode: ALARM): de RRK-3 noodstop blijft volledig actief.
- **Logging, status-rapportage en sensor-poll** lopen normaal door — alleen de regel-loop (T6) staat in pauze.
- **De gebruikersinterface** (LCD + web) blijft volledig bedienbaar.

#### Standby aanzetten — twee surfaces

**Via het LCD (Scherm 3 → `#`):**
1. Druk `D` tot je op **scherm 3** (`Mode/Sess`) bent
2. Druk `#`
3. Niet ingelogd? Voer **Boer-PIN óf Beheerder-PIN** in (PIN-lengte bepaalt de rol); druk `#`
4. Het menu toont `Now:AUTO` (of `STANDBY`) en de keuzes `1=Auto 2=Stby *B`
5. Druk `2` om Standby aan te zetten; korte bevestiging `Mode: STANDBY / control paused` → LCD springt terug naar auto-rotatie, scherm 3 toont `Mode: STANDBY`

**Via de webinterface (Climate-tab → "Mode" bovenaan):**
1. Inloggen als Boer of Beheerder
2. Tab **Climate** → bovenaan de keuzelijst "Mode" instellen op **Standby (paused)**
3. Klik **Apply** → binnen ~2 seconden mirror't het LCD `Mode: STANDBY`

#### Standby uitzetten + automatische kalibratie

Dezelfde route, maar kies **Automatic** (LCD: toets `1`, web: "Normal (autonomous)" + Apply).

**Belangrijk**: zodra Standby wordt verlaten, voert T2 automatisch een **CLOSE_ALL kalibratiecyclus** uit (identiek aan de boot-kalibratie):
- Alle drie de motoren krijgen tegelijk een CLOSE-commando
- Tijdens de cyclus toont scherm 3 `Mode:Window Cal.` en RGB-LED wordt amber
- De cyclus duurt tot ~3 minuten (M3 bepaalt de tijd: 171 sec. travel time + marge)
- Na voltooiing gaat de controller automatisch terug naar `Mode: AUTO` en T6 hervat de regelcyclus vanaf een bekende CLOSED-baseline

Deze re-kalibratie is een **bewust ontwerp**: tijdens Standby kan iedereen handmatig aan de ramen hebben gezeten (via de motorbox-Hand-schakelaars of via §10.11 admin-only manual control), waardoor de gepersisteerde raampositie niet meer betrouwbaar is. Een verse kalibratie geeft T6 een schone slate om vanaf op te bouwen.

#### Persistentie over een reboot

Standby is **NVS-backed** (opgeslagen in permanent geheugen op de microprocessor). Een stroomstoring tijdens een bewuste maintenance-pauze schakelt de controller dus **niet** stilletjes weer in op `Mode: AUTO` — de unit komt terug in `Mode: STANDBY` precies zoals jij hem hebt achtergelaten. Vergeet daarom niet om Standby weer uit te zetten zodra het werk klaar is.

**Uitzondering:** de Stand-by die een teach van de raamstandsensor zet (zie [M3 raamstandsensor — kalibratie](#m3-raamstandsensor--kalibratie-commissioning)) en de Stand-by van de handmatige raambediening via de LCD (zie [§10.11](#1011-handmatige-raambediening-via-de-lcd-beheerder)) worden níét opgeslagen en vervallen bij een herstart. In het logboek herken je ze aan `value_b` = 1 in de `MODE`-regel.

In de logfile herken je een Standby-transitie als een `MODE` event:
```
2026-05-26T14:30:22,MODE,ADMIN,1,0,1,0    # Beheerder zet Standby AAN via LCD
2026-05-26T15:45:11,MODE,WEB,0,0,0,0      # Iemand zet Standby UIT via web
```
De `channel`-kolom carriert de surface-hint: **0 = web GUI, 1 = LCD**. De `initiator`-kolom carriert de operator-rol (FARMER / ADMIN / WEB).

#### Beperking — Standby kan niet bij actieve safety-overrides

De web-toggle is automatisch grijs (uitgeschakeld) wanneer `WIND_OVERRIDE`, `MOTOR_ALARM` of `CALIBRATING` actief is. Op de LCD ben je de toggle wel kunt openen, maar de commit wordt geweigerd zolang die hogere prioriteiten gelden. De prioriteitsketen in `dm_status_snapshot()` is end-to-end: `MOTOR_ALARM → WIND_OVERRIDE → STANDBY → AUTOMATIC`.

### 10.11 Handmatige raambediening via de LCD (Beheerder)

Voor maintenance met handschoenen, een netwerkstoring, commissioning, een snel emergency-override of een demonstratie kan de Beheerder direct vanaf de kascontroller M1, M2 en M3 individueel openen of sluiten — **zonder via de webinterface te hoeven gaan**. Deze functie is **alleen voor de Beheerder** beschikbaar; de boer wordt expliciet niet ondersteund om bewust de autonome logica te kunnen omzeilen, omdat de controller dan niet kan leren van welke condities tot de override aanzetten.

#### Procedure (Scherm 6 → `#`)

1. Druk `D` tot je op **scherm 6** (`Raamposities`) bent
2. Druk `#`
3. Niet ingelogd als Beheerder? Voer **8-cijferige Beheerder-PIN** in (let op: Boer-PIN wordt hier afgewezen); druk `#`
4. **Motor-pick-scherm**:
   ```
      +----------------+
      |OPEN CLOS UNK   |
      |1=M1 2=M2 3=M3*B|
      +----------------+
   ```
   Rij 1 toont de actuele toestand per kanaal; rij 2 de toetsfuncties. Druk `1`, `2` of `3` om M1, M2 of M3 te kiezen; `*` om af te breken
5. **Action-picker-scherm**:
   ```
      +----------------+
      |[M2] CLOSED     |
      |1=Open 2=Cls *Bk|
      +----------------+
   ```
   De kop `[Mx]` toont welk raam je geselecteerd hebt en wat zijn huidige toestand is. Druk `1` voor OPEN, `2` voor CLOSE, `*` voor terug naar de motor-picker
6. Bevestiging `Mx opening / command sent` (of `Mx closing`); het commando wordt onmiddellijk aan T2 doorgegeven en de motor gaat lopen. Je blijft op het action-scherm — kies opnieuw een actie voor hetzelfde raam of druk `*` om een ander raam te kiezen

#### Wat gebeurt er onder de motorkap?

- **De controller gaat automatisch in STANDBY-modus** zodra je het menu binnenkomt. T6 (Climate Control) blijft daardoor **gepauzeerd voor de volledige duur van je admin-sessie** — niet alleen terwijl je in het menu bent, maar ook ná `*=back` en totdat je sessie afloopt. Je handmatige raamposities worden niet stilletjes door T6 overschreven omdat de buitencondities veranderen
- **De "respect-window" voor je manuele posities = je admin-sessie-timeout**. STANDBY blijft actief, en T6 blijft gepauzeerd, vanaf het moment dat je het menu binnenkomt tot het moment dat je sessie eindigt (5 minuten na je laatste toetsdruk, of expliciet uitloggen). Pressing `*=back` om uit het menu te gaan beëindigt de respect-window NIET — STANDBY blijft staan, Scherm 3 blijft `Mode: STANDBY` tonen
- **Was STANDBY al actief** vóór je het menu binnenging (bijv. via Scherm 3 # of de web GUI), dan blijft STANDBY ook na sessie-einde gewoon actief — alleen STANDBY-modus die door het menu zelf is aangezet wordt bij sessie-einde auto-uitgezet
- **Deze STANDBY wordt niet opgeslagen** (sinds 2.8.0, gh#65). Start de controller tijdens je sessie opnieuw op (stroomonderbreking, update), dan zijn je sessie en deze STANDBY allebei voorbij: na het opstarten regelt de controller weer automatisch. Vóór 2.8.0 bleef hij in dat geval onbeperkt in STANDBY staan
- **Start een webbeheerder tegelijk een teach**, dan houden jullie de STANDBY allebei vast. Hij gaat pas uit als beide sessies voorbij zijn
- **Dwell-timers worden bypassed** voor manual commands (`SRC_OPERATOR_MANUAL`). Een raam dat zojuist autonoom geopend werd, mag onmiddellijk handmatig dichtgaan zonder op de `dwell_open_s` te wachten. De anti-thrash protectie geldt alleen voor T6's autonome loop, niet voor jouw bewuste keuzes
- **Elk commando wordt geaudit-logd** als `LOG_RELAY`-rij (T2 emit'eert die wanneer hij de relay daadwerkelijk activeert). De `source = SRC_OPERATOR_MANUAL` veld in het Q1-bericht draagt de admin-attributie door naar de T2 per-command logregel
- **De motor-positie wordt persistent opgeslagen** in NVS zoals bij elk T2-commando — geen aparte "laatste handmatige positie"-key
- **De STANDBY-entry en -exit verschijnen als `MODE`-rijen in het logbestand** met `initiator=ADMIN`, `channel=1` (LCD-surface) en `value_b=1` (een vastgehouden STANDBY) — de STANDBY-on rij verschijnt bij menu-binnenkomst, de STANDBY-off rij pas bij sessie-einde (timeout of expliciete logout). De hele sessie is achteraf herleidbaar. Na een herstart midden in de sessie volgt er geen STANDBY-off rij: de opstartregel sluit de STANDBY dan af

#### Veiligheids­gates blijven actief

| Gate | Wat doet het bij manual command? |
|---|---|
| `EG1.MOTOR_ALARM` (Mode: ALARM) | **Weigert alle commando's** — LCD-toon: `MOTOR ALARM / cmd refused` |
| `EG1.CALIBRATING` (Window Cal., boot of na Standby-exit) | **Weigert alle commando's** — LCD-toon: `Calibrating / wait + retry` |
| `EG1.WIND_OVERRIDE` (Mode: WIND) | **Weigert manual OPEN; CLOSE wordt geaccepteerd** (sluiten is bij wind altijd veilig) — LCD-toon: `WIND OVERRIDE / OPEN refused` |

De weigering is niet stilzwijgend — je krijgt een korte LCD-melding (~1.5 sec) en daarna ben je terug op het action-scherm om iets anders te proberen.

#### Sessie-einde — wat resumeert er?

Het manual-motor menu heeft géén korte idle-dismiss. STANDBY blijft actief totdat je expliciet uitlogt of totdat je admin-sessie afloopt.

| Actie | Effect |
|---|---|
| **`*=back`** vanuit motor-picker | LCD keert terug naar auto-rotatie status­schermen. **STANDBY blijft echter actief** voor de rest van je admin-sessie — Scherm 3 toont nog steeds `Mode: STANDBY`, T6 blijft gepauzeerd. Je kunt opnieuw `D` drukken naar Scherm 6 en met `#` weer het menu in, om verder handmatig te bedienen zonder dat T6 tussendoor commando's stuurt |
| **Expliciet uitloggen** (hoofdmenu → 3:Access → 3:Logout) | Sessie sluit direct. Auto-gezette STANDBY gaat uit, alle ramen sluiten één keer om te kalibreren (tot ~3 min; scherm 3 toont `Mode: Window Cal.`) en T6 hervat vanuit die gesloten uitgangspositie |
| **Sessie-timeout** (`cfg.session_timeout_min`, default 5 min vanaf laatste toetsdruk) | Identiek aan expliciet uitloggen: "Session timeout" melding, LCD naar auto-rotatie, auto-gezette STANDBY uit, kalibratie, T6 hervat |
| **Herstart** (stroomonderbreking, update) | Sessie en auto-gezette STANDBY zijn allebei voorbij. De controller start op zoals altijd en regelt weer automatisch (sinds 2.8.0; zie hierboven) |

**Het "respect-window" voor handmatige posities is dus de sessie-timeout**: vanaf het moment dat je de laatste toets indrukt heb je standaard 5 minuten waarin je manueel ingestelde raamposities behouden blijven. Geen toetsdruk binnen die 5 min ⇒ sessie loopt af ⇒ STANDBY uit ⇒ de ramen sluiten één keer ⇒ T6 hervat vanuit die gesloten stand.

**Kalibratie bij sessie-einde** (sinds 2026-09-10): bij beide auto-clear-routes (logout + timeout) sluiten alle ramen één keer, net als bij een Standby-exit via Scherm 3 of web. Handmatig ingestelde raamposities blijven dus staan **zolang je sessie loopt**, niet daarna. Vroeger bleven ze na sessie-einde staan, en dan weigerden de dwell-timers T6's commando's soms tot 25 minuten lang: de controller stond op AUTOMATIC en deed niets. Wil je een raam langer in een handmatige stand houden, zet dan **Standby aan** via Scherm 3 of web: die blijft staan tot je hem zelf uitzet.

Was STANDBY al aan via Scherm 3 of web toen je het manual-menu binnenging, dan blijft hij ook na sessie-einde aan, en sluiten de ramen niet: alleen door-dit-menu-aangezette STANDBY wordt auto-gewist.

---

## 11. Eerste-installatie WiFi-verbinding

Dit hoofdstuk beschrijft uitsluitend de **eenmalige procedure** om de kascontroller voor het eerst op een WiFi-netwerk te krijgen — bijvoorbeeld na de fabriekslevering of na een reset op niveau 2/3 (zie [§18 Reset-procedure](#18-reset-procedure-io0-knop-op-microprocessorboard)). Voor reguliere WiFi-aanpassingen ná de eerste installatie (AP-wachtwoord, client-SSID, NTP, locatie, sessie-timeout) gebruikt u de **System-tab** in de webinterface — zie [§10.5 System-tab](#105-system-tab-alleen-beheerder).

### 11.1 Eerste keer WiFi configureren (na fabrieksreset of nieuwe installatie)

Wanneer de kascontroller voor het eerst wordt aangesloten of na een reset niveau 2/3, is er nog geen WiFi-verbinding. Volg de volgende procedure:

1. **Schakel het AP in op de controller**:
   - Druk vanaf elk statusscherm `D` totdat u op het **WiFi-status scherm** (scherm 4) bent
   - Druk `#`
   - Voer de Beheerder-PIN in (8 cijfers), druk `#`
   - Het System-menu opent met `1=WiFi AP`
   - Druk `1` om het AP te activeren — bevestiging `WiFi AP / enabling...`
   - Het LCD toont nu `WiFi: AP active` met SSID `Greenhouse-XXXX`

2. **Verbind uw laptop/telefoon met het AP**:
   - Zoek het WiFi-netwerk `Greenhouse-XXXX` (waar `XXXX` de laatste twee bytes van het MAC-adres zijn)
   - AP-wachtwoord: standaard `0123456789` — **wijzig dit zo snel mogelijk** (zie [§10.5 System-tab → WiFi AP](#wifi-ap-access-point-op-de-controller))
   - Verbind met het WiFi-netwerk.

3. **Open in uw browser de webinterface op het AP**:
   - Browse naar → `http://192.168.4.1`
   - Log in als Beheerder

4. **Configureer client-mode WiFi**:
   - Tab **System** → sectie **WiFi client**
   - Vul bij **SSID** de naam van uw WiFi-netwerk in
   - Vul bij **Password** het WiFi-wachtwoord in
   - Klik **Connect**
   - De controller probeert binnen ~30 sec. verbinding te maken; het LCD toont `WiFi: connecting...`, daarna `WiFi: connected` en het IP-adres van de controller

5. **Verbind uw apparaat opnieuw met het kas-/thuisnetwerk** en open de webinterface op het nieuwe IP-adres. Het AP kan nu uitgezet worden:
   - Op de controller: System-menu → `1` om het AP weer uit te schakelen, of wacht totdat het AP automatisch uitgeschakeld wordt na 30 min.

> **Vervolgstappen na deze eerste installatie** — pas direct het AP-wachtwoord aan ([§10.5 → WiFi AP](#wifi-ap-access-point-op-de-controller)) en, indien nodig, de tijdzone, locatie en sessie-timeout via [§10.5 System-tab](#105-system-tab-alleen-beheerder). Status-rapportage naar een extern dashboard configureert u via [§10.8 Web-tab](#108-web-tab-alleen-beheerder--status-rapportage-naar-extern-dashboard).

### 11.2 Automatische herstart na WiFi-wijziging — sinds firmware 1.19.1

De controller voert **automatisch een herstart uit** wanneer een WiFi instelling wodt gewijzigd. De herstart wordt ~1 sec na het opslaan in permanent geheugen  uitgevoerd.

**Wijzigingen die géén herstart triggeren:** alle andere velden van de System-tab (NTP-server, tijdzone, locatie-coördinaten, sessie-timeout) blijven via de bestaande "schrijf-en-doorgaan"-paden lopen.

---

## 12. Alarmen en bedrijfsmodi — diagnose en herstel

Voor algemene uitleg van bedrijfsmodi: zie [boer-handleiding §12](boerHandleiding.md#12-alarmen-en-bedrijfsmodi--wat-betekenen-ze-wat-te-doen).

Onderstaande secties verdiepen de diagnose vanuit beheerder-perspectief.

### 12.1 Windbeveiliging — fijn afstemmen

#### `v_max` instellen op kas-locatie

- Default 6 m/s is voor de meeste Nederlandse kassen veilig
- Bij blootstelling aan harde rukwinden: **verlagen naar 4–5 m/s**
- Bij geluwde locatie: **verhogen tot 8–10 m/s**
- Combineer met een **groter `avg_win_wind`** (gemiddeld windvenster) om kortdurende rukwinden te dempen

#### Uitsluitings-zone

Stel in als ramen rechtstreeks blootgesteld zijn aan een specifieke windrichting (bijv. zuidwesten).

Voorbeeld: ramen blootgesteld aan zuidwesten (180–270°):
- `dir_excl_low = 180`
- `dir_excl_high = 270`

Wanneer Noord binnen de uitsluitings-zone ligt (door 0°): bijv. uitsluiting NO-N-NW = 315–45°:
- `Dir excl. low = 315`
- `Dir excl. high = 45`

Zone uitschakelen: `Dir excl. low = Dir excl. high` of negatief.

#### Hysteresis — flapperen rond v_max

*(Sinds 2.3.0)* de override heeft een ingebouwde dode band: sluiten bij `v_max`, weer vrijgeven pas onder `v_max − wind_hyst` (standaard 1 m/s, Wind-tab, Beheerder-only). Flikkert de override desondanks — bijvoorbeeld bij sterk vlagerige wind die telkens door de hele band heen zakt — **verhoog dan eerst `wind_hyst`** (bereik 0–5 m/s) en **daarnaast `avg_win_wind`** (standaard 6 min., bereik 1–30 min.; een venster van 5–10 min. dempt vlagen goed). `wind_hyst = 0` herstelt het oude gedrag zonder dode band.

*Ter referentie*: op 19 juli 2026 flikkerde de override op de productie-unit drie keer binnen enkele minuten bij wind die precies rond `v_max = 6.0` m/s hing — de aanleiding voor deze functie.

### 12.2 Motor-alarm — diagnose

Voor algemene uitleg: zie [boer-handleiding §12.6](boerHandleiding.md#126-motor-alarm-in-detail).

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

- Webinterface tab **Log** → SD-bestanden downloaden (CSV-formaat)
- Zoek naar `ALARM`-events met sensor-fault-flag
- Tijdstempels (UTC in het bestand) correleren met externe gebeurtenissen (storm, stroomdip)

> **Logbestand omzetten naar leesbare tekst**: in de git-repo zit een Python-script (`log/logparser.py`) waarmee je een ruw CSV-logbestand kunt omzetten naar een geformatteerde tekst-versie met begrijpelijke beschrijvingen per event. Voor het complete log-formaat (alle velden, event-types en parameter-ID's) en het gebruik van het script: zie [Bijlage F — Logbestand-formaat en `logparser` script](#bijlage-f--logbestand-formaat-en-logparser-script).

### 12.4 Sensor-poll-interval afstemmen

| Configuratie item | Beschrijving | Default | Bereik |
|---|---|---|---|
| `Sensor poll interval` | Sensor-leesfrequentie | 30 s | 15–120 s |

> **Geen reboot nodig.** De wijziging geldt vanaf de **volgende poll-cyclus**: T5 leest het
> interval elke ronde opnieuw uit de configuratie. Eerdere versies van deze handleiding en
> de tooltip in de web-GUI vermeldden ten onrechte “reboot vereist” en een bereik van
> 30–300 s; beide zijn gecorrigeerd in firmware 2.5.1 (gh#57).

- Korter (15–30 s): snellere reactie, meer Modbus-traffic
- Langer (60–120 s): minder bus-belasting, minder snelle reactie, langer voordat sensor-fault wordt gedetecteerd (2 mislukte polls)

### 12.5 RGB-LED kleuren samengevat

Zie [boer-handleiding §12.3](boerHandleiding.md#123-rgb-led-kleuren-samengevat). LCD-achtergrond mirror-t dezelfde status (blauw=OK, rood=alarm).

### 12.6 Logbestand-formaten

- **SD-bestanden**: CSV-bestanden op de SD-kaart. Bestandsnamen volgen het patroon `<eenheid-ID>_YYYYMMDDHHMMSS.csv` (lokale aanmaaktijd), bijvoorbeeld `5C88_20260507143022.csv`
- **CSV-velden**: timestamp (ISO 8601 UTC), event_type, initiator, ch, param, value_a, value_b
- **Event-types**: `SENSOR_HR`, `SUN`, `RELAY`, `MODE`, `SETPT`, `SESSION`, `ALARM`, `SYSTEM`, `PIN_AUTH`. (`SENSOR` is de oude enkelvoudige sensor-rij van vóór rc.1.4.0 — die komt in nieuwe bestanden niet meer voor, maar `logparser.py` leest hem nog voor oude archieven. `PIN_AUTH` is nieuw in 2.5.1 en registreert mislukte PIN-invoer en de lockout.)
- Download via webinterface tab **Log**

Voor de complete uitleg van het logbestand-formaat (alle velden, event-types, parameter-ID's, channel-states, alarm-codes) en het gebruik van het meegeleverde `logparser`-script: zie [Bijlage F](#bijlage-f--logbestand-formaat-en-logparser-script).

### 12.7 Unit-identificatie

Elke kascontroller een **vier-tekens hex-ID** afgeleid van de lage 2 bytes van het WiFi MAC-adres.

**Waar de ID zichtbaar is:**

| Surface | Waar | Voorbeeld |
|---|---|---|
| **AP-SSID** | LCD-scherm 4 wanneer AP actief is, en in WiFi-instellingen van een laptop/telefoon | `Greenhouse-5C88` |
| **LCD-scherm 7** *(FW/Up)* | Rechts uitgelijnd op regel 1, naast het firmware-versienummer | `FW: 1.20.0  5C88` |
| **Webinterface-voettekst** | Onderaan elke pagina, na het versienummer | `Greenhouse Controller – v1.20.0 · 5C88` |

---

## 13. Inschakelen na stroomuitval

Voor algemene procedure: zie [boer-handleiding §13](boerHandleiding.md#13-inschakelen-na-stroomuitval).

### Beheerder-specifieke checklist

1. **Datum/tijd** klopt? RTC-batterij oké? (Zo niet: handmatig instellen via LCD time-status `#`, eventueel batterij vervangen)
2. **WiFi-verbinding** komt terug? Anders: AP activeren en client-config opnieuw instellen (zie [§11.1](#111-eerste-keer-WiFi-configureren-na-fabrieksreset-of-nieuwe-installatie))
3. **Setpoints** nog correct? worden in het permanente geheugen bewaart, dus zou onveranderd moeten zijn
4. **Kalibratie** loopt door? Controleer `Mode:Window Cal.` daarna `Mode: AUTO`

### Kalibratie-problemen

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

![FOTO: CR2032 batterijhouder op het microprocessorboard, met de juiste oriëntatie + plus zichtbaar](imagesBeheerder\kasControllerRTCBackupBattery.png)

*Figuur 16: R2032 batterijhouder op het microprocessorboard, met de juiste oriëntatie + plus zichtbaar*

1. Voeding van kascontroller wegnemen (stekker of zekering uit)
2. Open de kast van de controller; houdt rekening met de flat-cable van het toetenbord
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

Beide bestanden — firmware **én** web-assets — horen in één OTA-cyclus mee te gaan. de controller controleert dit automatisch en meldt een afwijking. 

Een **MISMATCH**-badge in de Alarms-tegel wijst op een onvolledige OTA-update: de firmware-bank is wel omgezet maar de web-assets niet (of andersom). Eerste actie: harde refresh van de webpagina (`Ctrl+Shift+R`); blijft de melding staan, voer de OTA-procedure dan opnieuw uit met **beide** pakketten in dezelfde sessie. Zie [§6 Status-tab — versie-controle van firmware en web-assets](#status-tab--versie-controle-van-firmware-en-web-assets) voor de achtergrond van dit mechanisme.

#### Dual-bank rollback
- Bij **3 opeenvolgende mislukte pogingen om op te starten** gaat de controller automatisch terug naar de vorige firmware-versie
- Symptomen van rollback: onverwachte oude versie na update — controleer ook de Alarms-tegel (na rollback met oude web-assets verschijnt **MISMATCH** zolang nog niet beide pakketten opnieuw geladen zijn)

#### Firmware-update mislukt
- **Upload halverwege afgebroken** (bijvoorbeeld door een slechte WiFi-verbinding): de OTA-status in tab **System** toont een foutmelding die eindigt op `nothing installed`. Er is dan **niets geïnstalleerd**: de controller draait gewoon door op de huidige versie, en een nieuwe poging kan **direct**, zonder herstart. Het SD-logboek bevat een `SYSTEM`-regel met `value_a = 32` en de reden.
- Eindigt de melding op `upload firmware then assets again`, dan brak de upload van de **web-assets** af nadat de firmware al was geüpload. Die firmware is dan ook verworpen: upload **eerst opnieuw de firmware en daarna de web-assets**. Alleen de ZIP opnieuw uploaden zet nieuwe web-assets op de oude firmware (**MISMATCH**).
- Valt de laptop tijdens een upload weg zonder de verbinding netjes te sluiten, dan reageert de webinterface tot ongeveer **30 seconden** niet. Daarna geeft de controller de upload op en toont de status de foutmelding.
- Controleer laptop-WiFi stabiel
- Probeer kleinere chunks (browser-instelling)
- Bij blijvende fout: USB-flash via het LOLIN S3-board (procedure: zie `firmware/README.md` of leverancier)

### SD-kaart beheer

De kascontroller schrijft logbestanden naar de SD-kaart. Wanneer er geen kaart aanwezig of niet leesbaar is, wordt logging onderbroken — er is geen permanente fallback-opslag. Zorg dat de SD-kaart altijd gemount is om een volledig logboek te hebben.

#### Eisen aan de SD-kaart

- **Bestandssysteem**: **FAT32** (verplicht). exFAT en NTFS worden niet ondersteund
- **Capaciteit**: in de praktijk **maximaal 32 GB** (grotere SDXC-kaarten worden door Windows en macOS standaard als exFAT geformatteerd, en moeten met een tool als `guiformat` of `mkfs.fat` handmatig naar FAT32 worden geformatteerd)
- **Klasse / snelheid**: geen minimum vereist; een Class 4 of hoger volstaat ruimschoots voor de geringe schrijfbelasting van logging
- **Type**: standaard SD- of SDHC-kaart (volwaardig formaat, geen microSD met adapter — al werkt dat technisch wel)

#### Wat zijn "mounten" en "unmounten"?

- **Mounten** is het beschikbaar maken van de SD-kaart voor de firmware. Voor het mounten kan er nog geen file gemaakt of gelezen worden van de SD-kaart. De firmware leest het FAT32 bestandssysteem in, controleert dat de kaart lees- en schrijfbaar is, en opent een logbestand om naar te schrijven. Pas na succesvol mounten kan logging naar SD plaatsvinden.
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

### Coredump ophalen na een panic

Wanneer de kascontroller onverwacht herstart door een **panic** (software-storing, bijvoorbeeld een ongeldige geheugen-toegang of een task-watchdog-timeout) schrijft de ESP-IDF panic-handler automatisch een **coredump** — een geheugen-snapshot op het moment van de crash — naar een speciale partitie in flash geheugen. Dit bestand bevat de stack-traceback, register-staat en de taak-naam die crashte. Op een werkstation met de ESP-IDF tools kun je deze coredump offline ontleden tot een leesbare backtrace.

#### Hoe weet je dat er een coredump klaar staat?

Twee onafhankelijke signalen:

1. **Status-tab → Alarms-tegel**: blauwe **Coredump available**-badge zodra de controller bij het opstarten een coredump in flash heeft gedetecteerd. Deze badge blijft staan tot de coredump gewist wordt.
2. **SD-logbestand**: één regel met `SYSTEM`-event `value_a = 18` direct na de boot-rij. Het `logparser.py`-script (zie [Bijlage F](#bijlage-f--logbestand-formaat-en-logparser-script)) toont deze als:
   ```
   2026-05-19 03:42:19  [SYSTEM ]  System    Coredump from previous panic detected in flash: ~45 KB
   ```

#### Coredump downloaden

1. Inloggen als **Beheerder** in de webinterface
2. Tab **Log** → onder aan de pagina: sectie **Diagnostics**
3. Lees de regel "Coredump: Available — N bytes (N KB) • captured on fw ..."
4. Klik **Download**
5. De browser slaat het bestand op met de naam `coredump-<firmware-versie>-<unix-tijdstempel>.bin`

Onmiddellijk na downloaden wordt de **Erase**-knop actief. Daarvóór is hij gegrijst — je moet eerst de download bevestigen voor je iets kunt wissen.

#### Coredump offline ontleden

Op je werkstation, met **ESP-IDF** geïnstalleerd, en met het `.elf`-bestand bij de hand dat hoort bij de firmware-versie waarop de panic plaatsvond (te vinden in `bin/<versie>/firmware-<versie>.elf` in de repository of in het release-pakket van de leverancier):

```
idf.py coredump-info \
    -t raw \
    -c ~/Downloads/coredump-2.0.0-a.6.35.7-1779999999.bin \
    bin/2.0.0-a.6.35.7/firmware-2.0.0-a.6.35.7.elf
```

De uitvoer geeft:

- Naam van de taak die crashte
- Register-staat op het crash-moment (`PC`, `EXCCAUSE`, `EXCVADDR`)
- Volledige backtrace met `file.cpp:regelnummer`-verwijzingen
- Stand van alle andere taken op het moment van de crash + stack-watermerken

Dit is de informatie waarmee de leverancier of softwareontwikkelaar de oorzaak van de panic kan achterhalen zonder de fout te hoeven reproduceren.

#### Coredump wissen na succesvolle analyse

Pas wanneer je het bestand veilig hebt en de offline analyse is gelukt:

1. Tab **Log** → sectie **Diagnostics** → klik **Erase**
2. Bevestig de waarschuwingsdialoog ("After erase, the dump is unrecoverable.")
3. De badge **Coredump available** verdwijnt; de partitie is klaar voor het opvangen van de volgende panic

> **Niet wissen voordat je het bestand hebt!** De coredump wordt overschreven bij de volgende panic, maar tot dat moment is het de enige forensische bron — er bestaat geen tweede kopie. Wissen is onomkeerbaar.

#### Beveiliging van de coredump-endpoints

- Alle drie de endpoints (`status`, `download`, `erase`) zijn **alleen voor de Beheerder-rol** toegankelijk; een Boer-sessie krijgt HTTP 403. Zonder sessie HTTP 401.
- **Snelheidsbegrenzing**: maximaal 1 download- of erase-actie per 10 seconden. Een herhaalde snelle klik krijgt HTTP 429 met "rate limited".
- **Audit-spoor in SD-CSV**: elke download (`value_a = 19`) en elke wissing (`value_a = 20`) wordt geregistreerd met `initiator = WEB`. Bij ongeplande toegang kan dit later worden teruggevonden in het logbestand.
- **Coredump-inhoud kan gevoelige data bevatten** (bijv. WiFi-PSK, status-website-secret of een PIN die op het moment van de panic in het werkgeheugen stond). Behandel het gedownloade `.bin`-bestand daarom als gevoelige informatie en wis het van je werkstation zodra de analyse klaar is.

### Loggen instellingsveranderingen

De legt de kascontroller **elke instellingswijziging** vast in het logbestand op de SD-kaart, met:

- **Welke parameter** is gewijzigd (bijv. `t_max_day`, `wind_prot_en`, `status_intv_s`)
- **Wie** de wijziging deed en waar vandaan: `Boer (LCD)`, `Beheerder (LCD)`, of `Web UI`
- **Oude en de nieuwe waarde** voor numerieke parameters

Voor gevoelige velden (PIN-wijziging, WiFi-wachtwoord, status-website-secret, time-zone-string) wordt alleen de boodschap **"changed"** of **"set"** vastgelegd — de daadwerkelijke waarde komt **niet** in het logbestand. Dit voorkomt dat het SD-bestand een lek-bron wordt voor credentials.

#### Voorbeelden in parsed log

```
2026-05-19 14:35:00  [SETPT  ]  Admin (LCD)     t_max_day: 25 degC -> 27 degC
2026-05-19 14:36:00  [SETPT  ]  Web UI          poll_interval: 60 s -> 30 s
2026-05-19 14:37:12  [SETPT  ]  Web UI          wind_prot_en: enabled -> disabled
2026-05-19 14:42:18  [SETPT  ]  Web UI          pin_admin (changed)
2026-05-19 14:43:05  [SETPT  ]  Web UI          wifi_ssid (set)
2026-05-19 14:43:10  [SETPT  ]  Web UI          tz_str (set)
```

#### Wat dit voor de beheerder oplevert

- **Onverklaarde klimaatdrift**: wanneer ramen anders openen dan verwacht, geeft het audit-spoor onmiddellijk antwoord op de vraag "wie heeft welke setpoint wanneer veranderd?". Filter het CSV-bestand op `SETPT` om een tijdlijn op te bouwen.
- **PIN-veiligheid**: een onverwachte regel `pin_admin (changed)` of `pin_farmer (changed)` op een tijdstip dat je niet zelf bezig was → verandering door iemand anders. Tijd om de PIN opnieuw te wijzigen en de toegang te onderzoeken.
- **WiFi-credentials**: wijzigingen aan `wifi_ssid` / `wifi_psk` / `wifi_ap_psk` worden eveneens vastgelegd. Een onverwachte regel hier kan wijzen op ongewenste configuratie-toegang.
- **Status-website-instellingen**: alle 8 velden in tab **Web** (URL, secret, interval, expose-mask, log-upload-tijd, log-upload-rotatie) worden per gewijzigd veld vastgelegd, zodat een operator achteraf precies kan reconstrueren welke veld(en) bij een Apply zijn aangepast.

Voor de volledige tabel van parameter-ID's en de gebruikte sentinel-codering: zie de bijgewerkte [`log/logparser.md`](https://github.com/pe1mew/greenhouse-Controller/blob/main/log/logparser.md) in de git-repository.

#### SD-kaart vervangen / formatteren

1. Unmount via webinterface (zie hierboven)
2. Vervang SD-kaart, of formatteer hem opnieuw op FAT32 (Windows/macOS bij grotere SDXC-kaarten: gebruik een dedicated FAT32-formattertool)

#### Vrije ruimte en bestandsrotatie

Om te voorkomen dat de SD-kaart vol raakt:

- **Per logbestand** wordt geroteerd na 1 MB; daarna start de firmware een nieuw bestand met naam `<eenheid-ID>_YYYYMMDDHHMMSS.csv`
- **Maximaal 30 logbestanden per controller** worden bewaard; bij meer verwijdert de firmware bij elke rotatie de oudste van deze controller, tot vijf tegelijk, tot hij weer op 30 zit. Bestanden van een andere controller op dezelfde kaart worden nooit verwijderd (sinds 2.12.2, gh#82)
- **Minimaal 5 bestanden** blijven altijd bewaard (vloer): zelfs bij weinig vrije ruimte wordt nooit onder dit aantal verwijderd
- **Minimaal 4 MB vrije ruimte** vereist; daaronder probeert de firmware oudste bestanden te verwijderen om ruimte vrij te maken
- Zit de controller op de bestands-vloer (5) **én** is er minder dan 4 MB vrij, dan wordt SD-logging tijdelijk **opgeschort**. Een `SYSTEM`-event met `value_a = -2` markeert dit moment in het log; events worden niet opgeslagen totdat er weer ruimte is

> **Praktijk**: bij gewone bedrijfsvoering is een 8 GB-kaart ruim voldoende voor jaren logging. Bij vermoeden van problemen: download alle bestanden, formatteer de kaart opnieuw, plaats hem terug.

### Power-cycle uitvoeren

Zie [boer-handleiding §14](boerHandleiding.md#14-onderhoud--wat-de-boer-zelf-doet). Identieke procedure.

![FOTO: microprocessorboard met RESET-knop en BOOT-knop duidelijk gemarkeerd](images\LolinS3Reset.png)

*Figuur 17: microprocessorboard met RESET-knop*

#### Window Cal bij opstart — wanneer wel, wanneer niet

Bij élke opstart van de controller (power-cycle, druk op RESET-knop, geplande herstart door T15-supervisor, OTA-update, fabrieksreset, watchdog-reset, panic-reset) loopt de controller een vaste boot-procedure door die kan resulteren in **één van drie uitkomsten**:

| Voorwaarde bij opstart | Resultaat | LCD-modus | Hersteltijd |
|---|---|---|---|
| Motor-alarm actief | Geen kalibratie; controller gaat direct naar alarm-staat | `Mode: ALARM` | direct |
| **Geen alarm én alle 3 de ramen staan dich in permanent geheugen** | **Kalibratie overgeslagen** | `Mode: AUTO` | **~2 sec** |
| Geen alarm én ten minste één raam is `OPEN` of `UNKNOWN` in permanent geheugen | Volledige CLOSE_ALL kalibratie | `Mode:Window Cal.` | **~3 min** (~176 sec M3) |

**De skip-conditie hangt uitsluitend af van de raamposities op het moment dat de vorige firmware-sessie eindigde** — niet van het type opstart. Concrete consequenties:

- **Power-cycle 's nachts met alle ramen dicht** → skip (~2 sec). Identiek aan een geplande reboot of een OTA-update die op datzelfde moment plaatsvindt.
- **Power-cycle midden op een warme dag** met M3 of M2 open → volledige kalibratie (~3 min). Identiek aan een geplande reboot of OTA op datzelfde moment.
- **Fabrieksreset (BOOT-knop)** → permanente geheugen wordt gewist → alle drie posities default `UNKNOWN` → altijd volledige kalibratie.

**Hoe weet de controller dit?** Bij elke transitie naar een eind-positie (CLOSED of OPEN) wodt de raampositie opgeslagen in het permanente geheugen. Vóór het aansturen van een van de relais wordt eerst `UNKNOWN` weggeschreven, zodat een stroomuitval midden in een beweging correct als "onbekend → kalibreren" wordt hersteld. 

**Waarom is dit belangrijk?**

- De controller heeft **geen positie-feedback** van de motoren — hij volgt de raamposities intern bij op basis van de open/sluit-commando's die hij zelf heeft verstuurd. Tijdens een stroomuitval, een handmatige beweging op de RRK-3, of een motor-alarm gaat die interne aanname verloren of klopt niet meer met de werkelijkheid.
- De CLOSE_ALL kalibratie is de **enige manier** om die interne aanname weer in lijn te brengen met de fysieke werkelijkheid wanneer dat verloren is gegaan.
- **Duur van de kalibratie**: ~26 sec. voor M1 en M2 (gelijktijdig), ~176 sec. voor M3 — totaal dus ongeveer **3 minuten** voordat `Mode: AUTO` weer verschijnt.
- **Een handmatige beweging op de RRK-3 wordt door de kascontroller niet gedetecteerd** (alleen het motor-alarm wordt gemeld via GPIO42). De controller blijft de raamposities bijhouden zoals hij die zelf gestuurd had en zal die nog naar NVS persisteren. Een power-cycle na handmatige overname kan dus de "skip"-conditie raken terwijl de fysieke ramen niet werkelijk dicht zijn. Volg daarom altijd de procedure in [§15 *Handmatige overname via de motorbox*](#15-handmatige-overname-via-de-motorbox).

**Wanneer is een geforceerde power-cycle plus kalibratie nodig?**

| Situatie | Actie |
|---|---|
| Na handmatige overname op de motorbox | Voer power-cycle uit **terwijl ten minste één raam fysiek open staat** (bv. M3) — de NVS-positie klopt dan niet met `all closed` en de kalibratie loopt. Of: gebruik een fabrieksreset om NVS leeg te maken (overweging: alle setpoints gaan dan ook verloren). |
| Na onderhoud aan een raam-motor of bedrading | Zelfde benadering — open één raam handmatig vóór het opstarten zodat de skip-conditie niet trip. |
| Na herstel van een stroomuitval | Geen actie nodig; de controller doet automatisch het juiste op basis van de NVS-staat. |
| Bij wijziging van motor-travel-times in de webinterface | Niet strikt vereist (nieuwe waardes worden direct toegepast op de volgende beweging). Voor verificatie: forceer een kalibratie door één raam handmatig te openen vóór een power-cycle. |
| Na firmware-update (OTA) | Geen actie nodig. De controller herstart automatisch en evalueert de skip-conditie. |
| Bij motor-alarm-clearance | Niet vereist — de controller doet automatisch een 60-seconden guard + CLOSE_ALL re-kalibratie zodra het alarm wegvalt (zie [boer-handleiding §12.6](boerHandleiding.md#126-motor-alarm-in-detail)). |

> **Praktische tip**: plan een power-cycle die een kalibratie *moet* uitvoeren op een rustig moment (bv. avond). Tijdens de ~3 minuten kalibratie staan alle ramen dicht en is de klimaatregeling tijdelijk inactief. Een power-cycle waarbij de skip-conditie zal opgaan (alle ramen al dicht) is daarentegen elke moment veilig — hersteltijd ~2 sec.

---

## 15. Handmatige overname via de motorbox

Voor algemene uitleg en consequenties: zie [boer-handleiding §15](boerHandleiding.md#15-handmatige-overname-via-de-motorbox).

### Voor de beheerder bij onderhoud aan motoren of constructie

**Aanbevolen procedure tijdens onderhoud aan een raam-motor**:

1. Controleer LCD: `Mode: AUTO` actief? Geen lopende beweging?
2. Op de Hotraco RRK-3: zet de schakelaar van het betreffende raam **op handbediening** (of bij voorkeur **alle drie** als je niet zeker weet aan welk raam je werkt)
3. **Indien werkzaamheden aan motor zelf** (bedrading, vervangen): zet bovendien de motor-zekering in de RRK-3 uit, of haal de stekker eruit
4. Voer onderhoud uit
5. Na onderhoud: **eerst zekering / stekker terug**, dan **schakelaars terug op automatisch**
6. **Power-cycle de kascontroller** (zie [§14](#14-onderhoud--wat-de-beheerder-doet)) **met ten minste één raam fysiek open** om de CLOSE_ALL kalibratie af te dwingen — alleen zo weet de controller weer met zekerheid waar de ramen staan. Power-cyclen terwijl alle drie de ramen dicht staan trip mogelijk de NVS-skip (sinds 1.17.36, zie [§18](#18-reset-procedure-io0-knop-op-microprocessorboard)) waardoor de kalibratie wordt overgeslagen en de controller-aanname ongetest blijft

> Zie [boer-handleiding §15 — De kascontroller weet niet dat hij is uitgeschakeld](boerHandleiding.md#de-kascontroller-weet-niet-dat-hij-is-uitgeschakeld) voor de gevolgen van handmatige stand zonder power-cycle achteraf.

![Hotraco RRK-3 motorbox met de drie schakelaars per kanaal duidelijk in beeld; markeer welke positie hoort bij "automatisch" en welke bij "handbediening"](images\RBMotorControllerKnoppenstand.png)

*Figuur 18: Hotraco RRK-3 motorbox met de drie schakelaars per kanaal*

---

## 16. Probleemoplossing — Beheerder-niveau

### 16.1 Hardware-problemen

| Probleem | Diagnose | Actie |
|---|---|---|
| LCD blank, heartbeat-LED uit | Voeding weg | Voeding controleren; zekering nameten |
| LCD blank, heartbeat-LED knippert | LCD-bus probleem | Power-cycle; bij blijvende fout LCD-module vervangen |
| Heartbeat-LED steady aan (niet knipperend) | Firmware vastgelopen | Power-cycle of reset; bij herhaling firmware re-flash |
| Controller herstart onverwacht | **Software-panic** wordt vrijwel altijd opgevangen in een coredump | Inloggen als Beheerder → tab **Log** → sectie **Diagnostics** → check op `Coredump available`-badge in de Alarms-tegel. Download het `.bin`-bestand voor offline analyse — zie [§14 Coredump ophalen na een panic](#coredump-ophalen-na-een-panic). Tegelijkertijd: SD-logbestand downloaden — de regel direct vóór de boot-marker (`SYSTEM value_a=5`) toont wat de controller deed kort vóór de crash |

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
| SD-kaart vol / bijna vol | Oudste logbestanden worden automatisch verwijderd tot de vloer (3 bestanden) is bereikt | Download bestanden, formatteer de kaart opnieuw |

### 16.8 Tijd

| Probleem | Diagnose | Actie |
|---|---|---|
| Tijd 1970/2000 | RTC-batterij leeg, geen NTP | CR2032 vervangen, NTP herstellen |
| Tijd loopt fout | Tijdzone niet juist | `tz_str` controleren |
| Dag/nacht-omschakeling op verkeerde tijd | Lat/lon fout | Locatie opnieuw instellen |

### 16.9 PIN's en toegang

| Probleem | Diagnose | Actie |
|---|---|---|
| Beheerder-PIN vergeten | — | BOOT-knop niveau 1 reset (zie [§18](#18-reset-procedure-io0-knop-op-microprocessorboard)) |
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
| **Conflict-prioriteit** | Welke regel-as voorrang krijgt als T en RH *tegengestelde* acties vragen (vragen ze allebei om ventileren, dan wint altijd de hoogste stap) — zie [§4](#4-hoe-regelt-de-controller-het-klimaat) |
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
Zie [boer-handleiding §17](boerHandleiding.md#17-verklarende-woordenlijst) — identieke set strings.

---

## 18. Reset-procedure (IO0-knop op microprocessorboard)

Zie ook [boer-handleiding §18](boerHandleiding.md#18-reset-procedure-io0-knop-op-microprocessorboard) voor de basisprocedure.

### Niveau-overzicht

| Inhouden | LCD-melding | Effect |
|---:|---|---|
| 0–5 sec. | (geen melding) | Geen actie |
| 5–10 sec. | `Reset PIN?` | Niveau 1 — PIN's terug naar fabrieksstandaard |
| 10–15 sec. | `Reset settings?` | Niveau 2 — alle instellingen + PIN's reset. **Geen reboot** — en omdat WiFi-instellingen pas bij de volgende herstart worden toegepast, blijft de controller tot dat moment op het oude netwerk |
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

> **Let op bij verhuizing, reparatie of afvoer van een controller.** Een reset wist NVS
> *logisch* — niet de flash-sectoren — en de flash van deze controller is **niet
> versleuteld** (`CONFIG_SECURE_FLASH_ENC_ENABLED` en `CONFIG_NVS_ENCRYPTION` staan uit).
> Het WiFi-wachtwoord en de overige instellingen van de vorige locatie blijven daarmee met
> `esptool` uitleesbaar zolang de board niet volledig is herschreven. **Een reset op
> niveau 3 is hiervoor niet voldoende.** Verlaat een controller de locatie — verhuizing,
> retour voor reparatie, of afvoer — neem dan één van deze twee stappen:
>
> 1. **Wijzig het WiFi-wachtwoord van de kas** nadat de controller is verwijderd, of
> 2. **Flash de controller volledig opnieuw** (greenfield-procedure — wist de hele flash).

### Na niveau 2 of 3 — checklist herinstallatie

1. ☐ WiFi configureren (AP → client-mode, [§11.1](#111-eerste-keer-WiFi-configureren-na-fabrieksreset-of-nieuwe-installatie))
2. ☐ AP-wachtwoord wijzigen ([§10.5](#wifi-ap-access-point-op-de-controller))
3. ☐ Tijdzone instellen ([§10.5](#ntp-en-tijdzone))
4. ☐ Geografische locatie controleren / instellen ([§10.5](#geografische-locatie-location))
5. ☐ Sessie-timeout naar wens instellen ([§10.5](#sessie-timeout))
6. ☐ Motor-tijden controleren / instellen ([§10.4](#104-motors-tab-alleen-Beheerder))
7. ☐ Klimaat-setpoints instellen (boer of Beheerder namens boer)
8. ☐ hysteresis, glijdend gemiddelde fijnafstemmen
9. ☐ Wind v_max en eventueel uitsluitings-zone
10. ☐ Raamstandsensor op M3 (tab **Motors**, M3 → *Linear control*): zet *Position sensor fitted* op **Yes** — na de reset staat hij op No, en dan draait M3 tijdgestuurd. Controleer dat *Calibration* **VALID** toont; zo niet, voer een teach uit. *M3-besturing* staat na de reset op **Lineair** (fabrieksinstelling sinds 2.13.0)
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

De kascontroller schrijft gebeurtenissen naar de **SD-kaart** als CSV-bestanden. De bestandsnaam heeft het formaat `<eenheid-ID>_YYYYMMDDHHMMSS.csv` (eenheid-ID + lokale aanmaaktijd), bijvoorbeeld `5C88_20260507143022.csv`. De firmware roteert naar een nieuw bestand bij 1 MB en bewaart 30 bestanden per controller (zie [§10.7](#107-log-tab-alleen-beheerder)).

Download via webinterface tab **Log** (Beheerder-rol vereist).

#### CSV-formaat

Elke regel is één event met de volgende kolommen:

| Kolom | Beschrijving |
|---|---|
| `timestamp` | ISO 8601 **lokale tijd** (`YYYY-MM-DDTHH:MM:SS`). |
| `event_type` | Een van: `SENSOR`, `RELAY`, `MODE`, `SETPT`, `SESSION`, `ALARM`, `SYSTEM` |
| `initiator` | Wie veroorzaakte het event: `SYS`, `FARMER`, `ADMIN`, `MQTT`, `WEB` |
| `ch` | Motor-kanaal (1=M1, 2=M2, 3=M3) of 0 — voor `ALARM`-events of 4 (T/RH-sensor-fout) of 5 (wind-sensor-fout) |
| `param` | Parameter-ID (alleen bij `SETPT`-events) of 0 |
| `value_a` | Eerste waarde, betekenis afhankelijk van event-type |
| `value_b` | Tweede waarde, betekenis afhankelijk van event-type |

#### Event-types op hoofdlijnen

| Type | Wanneer gepost | Belangrijke velden |
|---|---|---|
| `SENSOR` | Iedere sensor-poll-cyclus | `value_a` = T (°C), `value_b` = RH (%) |
| `RELAY` | Bij motor-toestandsovergang | `ch` = motor, `value_a` = nieuwe state (0–6) |
| `MODE` | Bij wijziging van ventilatie-stap | `value_a` = stap (0–3) |
| `SETPT` | Bij elke wijziging van een setpoint of admin-instelling | `param` = parameter-ID, `value_a` = oud, `value_b` = nieuw. Bij gevoelige velden (PIN, WiFi-credentials, status-website-secret) wordt alleen `value_a = 1` als sentinel geschreven — de daadwerkelijke waarde komt **niet** in het log |
| `SESSION` | Bij login/logout | `value_a` = niveau (0/1/2) |
| `ALARM` | Wind-override, motor-alarm, sensor-fault | Specifieke codering per alarm-type; sensor_poll `ch = 4/5` om T/RH- en wind-fout-rijen te onderscheiden van motor-alarmen |
| `SYSTEM` | Systeem-events (boot, queue overflow, SD-fout, OTA-stadia, coredump-status, audit-rijen) | Verschilt per sub-event — zie `log/logparser.md` voor de volledige tabel |

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
python logparser.py 5C88_20260507143022.csv
# → output: parsed_5C88_20260507143022.txt

# Alle SD-bestanden in de huidige map parsen (chronologisch geordend, samengevoegd)
python logparser.py *
# → output: parsed_YYYYMMDD.txt
```

**Voorbeeld-output**:

```
Timestamp (local)    Type        Initiator       Description
--------------------------------------------------------------------
2026-05-19 14:30:22  [SENSOR ]   System          T=23 °C   RH=65 %
2026-05-19 14:30:52  [RELAY  ]   System          M1: → MOVING_OPEN
2026-05-19 14:31:10  [MODE   ]   System          Vent step → 1 (M1 open)
2026-05-19 14:35:00  [SETPT  ]   Admin (LCD)     t_max_day: 25 degC -> 27 degC
2026-05-19 14:36:00  [SETPT  ]   Web UI          poll_interval: 60 s -> 30 s
2026-05-19 14:37:12  [SETPT  ]   Web UI          wind_prot_en: enabled -> disabled
2026-05-19 14:42:18  [SETPT  ]   Web UI          pin_admin (changed)
2026-05-19 14:45:00  [ALARM  ]   System          WIND OVERRIDE: SET — speed 8.5 m/s ≥ v_max 5.0 m/s
2026-05-19 14:50:00  [ALARM  ]   System          WIND OVERRIDE: CLEARED — speed 3.2 m/s, direction 180°
2026-05-19 15:00:00  [SYSTEM ]   System          OTA: firmware POST started (bytes streaming to inactive bank)
2026-05-19 15:00:05  [SYSTEM ]   System          OTA: firmware verified OK — awaiting web-asset upload
2026-05-19 15:00:10  [SYSTEM ]   System          OTA: asset ZIP extracted OK — reboot scheduled (1 s)
2026-05-19 15:00:14  [SYSTEM ]   System          Boot: esp_reset_reason = 4 (PANIC)
2026-05-19 15:00:14  [SYSTEM ]   System          STA WiFi client: connected
2026-05-19 15:00:14  [SYSTEM ]   System          NTP: synced
2026-05-19 03:42:19  [SYSTEM ]   System          Coredump from previous panic detected in flash: ~45 KB
2026-05-19 08:15:02  [SYSTEM ]   Web UI          Coredump downloaded by admin (~45 KB transferred)
2026-05-19 08:18:33  [SYSTEM ]   Web UI          Coredump erased by admin (partition wiped)
```

#### Volledige documentatie

De **complete uitleg** — met daarin alle velden, alle event-types, alle parameter-ID's voor `SETPT`-events, alle channel-states voor `RELAY`-events, de codering van `ALARM`-events en bekende beperkingen — staat in **`log/logparser.md`** in de git-repository. Houd dit document naast deze handleiding bij het analyseren van logbestanden.

---
### Bijlage G — Aanbevolen startinstellingen per gewas

## Aanbevolen startinstellingen per gewas

> **Belangrijk** — deze waarden zijn **startpunten**, geen absolute regels. De ideale instelling voor jouw kas hangt af van locatie, seizoen, gewas-variëteit, groeistadium en persoonlijke ervaring. Stel de waarden in zoals hieronder, observeer een paar dagen, en bij sla, leuter een paar °C in de juiste richting tot het klopt met wat je in de kas ziet. De getallen zijn afgerond op hele graden / procenten — de kascontroller werkt sowieso met gehele getallen (zie [§4 Hoe regelt de controller het klimaat?](#4-hoe-regelt-de-controller-het-klimaat)).

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

- **T dag min–max** / **T nacht min–max**: temperatuurband. **Min**imum is de waarde waaronder de controller de ramen sluit (`T_min`); **max** is de waarde waarboven de controller de ramen opent (`T_max`). Tussen min en max gebeurt er niets (regelhysteresis — zie §10.1).
- **RH dag min–max** / **RH nacht min–max**: vochtigheidsband, alleen actief wanneer **RH-regeling** aan staat.
- **RH-regeling**: of de controller mag reageren op vochtigheid. **Aan** = vochtigheid stuurt mee in de raam-beslissing; **uit** = alleen temperatuur stuurt (handig voor gewassen waar vocht niet de beperkende factor is).
- **CR-prio** (Conflict Resolution-prioriteit): wat doet de controller als T en RH tegelijk om tegengestelde acties vragen? **T** = temperatuur wint (gebruikelijk bij koel-seizoen-gewassen en warme zomers); **RH** = vochtigheid wint (gebruikelijk wanneer een gewas vochtigheids-gevoelig is — schimmelziektes, botrytis, meeldauw). Let op wat **T** in de praktijk betekent: de temperatuur-as wint ook wanneer die om *dichte* ramen vraagt, dus met **T** opent de vochtregeling nooit uit zichzelf een raam — ze kan alleen een stap verhogen die de temperatuur al gevraagd had. Voor de gewassen met **RH** in deze kolom is juist dat het punt. Zie *Hoe de twee regel-assen worden gecombineerd* in [§4](#4-hoe-regelt-de-controller-het-klimaat).
- **Opmerkingen**: gewas-specifieke aandachtspunten waar de getallen alleen niet voldoende zijn.

### Hoe te gebruiken

1. Zoek je gewas op in de tabel (of het meest vergelijkbare).
2. Log in als boer op de webinterface of LCD (zie §9).
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

## 20. Versie en wijzigingshistorie

Inhoudelijke wijzigingen aan de firmware staan beschreven in het bestand `changelog.md` in de git-repository. Deze tabel houdt alleen bij welke firmware-versie door welke handleiding-versie wordt afgedekt.

| Versie | Datum | Firmware |
|---|---|---|
| 1.0 | Er was eens... | 1.16.34 |
| 1.1 | 2026-05-09 | 1.16.35–1.16.38 |
| 1.2 | 2026-05-10 | 1.16.39 |
| 1.3 | 2026-05-10 | 1.17.0–1.17.1 |
| 1.4 | 2026-05-11 | 1.17.2–1.17.8a |
| 1.5 | 2026-05-11 | 1.17.9–1.17.20 |
| 1.6 | 2026-05-11 | 1.17.21–1.17.25 |
| 1.7 | 2026-05-12 | 1.17.25 (alleen herordening §10/§11) |
| 1.8 | 2026-05-12 | 1.17.25 (alleen kop-/voettekst en figuur-nummering) |
| 1.9 | 2026-05-12 | 1.17.26 |
| 1.10 | 2026-05-14 | 1.17.27–1.18.2 |
| 1.11 | 2026-05-14 | 1.18.3 |
| 1.12 | 2026-05-15 | 1.19.0–1.20.0 |
| 1.13 | 2026-05-16 | 1.20.1-1.20.2 |
| 1.14 | 2026-05-16 | 1.20.2 (alleen documentatie — Bijlage G uitgebreid van 13 naar 28 gewassen passend bij de teelt in Wenumseveld: Meloen, Ananaskers, Spaghettiboon, Peulen, Rucola, Paksoi, Snijbiet, Raapsteel, Palmkool, Koolrabi, Bospeen, Bosbiet, Radijs, Groene selderij, Bloemen — geordend per gewas-familie. Inhoud blijft synchroon met boer-handleiding Bijlage B v1.12) |
| 1.15 | 2026-05-17 | 1.20.3 |
| 1.16 | 2026-05-19 | 2.0.0-a.6.32 → 2.0.0-a.6.35.7 |
| 1.17 | 2026-05-26 | 2.0.0-rc.1.5.0 |
| 1.18 | 2026-05-26 | 2.0.0-rc.1.5.1 |
| 1.19 | 2026-05-26 | 2.0.0-rc.1.5.2 |
| 1.20 | 2026-06-26 | 2.0.0 t/m 2.1.1 — T min dag/nacht gedocumenteerd (webinterface); SD-logbestand bestandsnaam eenheid-ID prefix (gh#30, 2.0.1); `avg_win_wind` naam en standaard gecorrigeerd; windgemiddelde onafhankelijk venster (gh#35, 2.1.0); standaard uitmiddelvenster gecorrigeerd naar 6 min; bugfix HTTP-statuscode in auditlog (gh#34, 2.1.1) |
| 1.21 | 2026-09-16 | 2.2.0 t/m 2.8.0 — automatische internet-update (ROTA) in tab System, met figuur (2.2.0); wind-hysterese (gh#46, 2.3.0); reset-procedure met de BOOT-knop, vergeten beheerder-PIN en de coredump-melding (gh#56, 2.4.8); bereik sensor-leesfrequentie 15–120 s (gh#57, 2.5.1); `dwell_open_s` in plaats van `dwell_open_min` (gh#63, 2.7.0); tab Motors per raam gegroepeerd, dodezone en kalibratie (teach) van de M3-raamstandsensor met de foutmeldingen (2.8.0); de Stand-by van een teach en van de handmatige raambediening via de LCD wordt niet opgeslagen en eindigt met de sessie of een herstart (gh#65, 2.8.0); het sessie-einde van de LCD-raambediening sluit de ramen één keer (gedrag sinds 2.4.5, tekst nu gecorrigeerd); afgebroken OTA-upload (2.8.0); statusregel Standby gecorrigeerd |
| 1.22 | 2026-09-17 | 2.9.0 — instelling *Position sensor fitted* voor de M3-raamstandsensor, standaard No; zonder sensor geen regel voor adres 40 meer en met sensor een storingsmelding als die niet reageert (gh#73); de reden bij het grijze Commissioning-blok noemt nu de echte oorzaak |
| 1.23 | 2026-09-19 | 2.10.0 — met een raamstandsensor beoordeelt de controller elke beweging van M3 (bevestigd, niet bereikt, niet beoordeeld) en controleert hij de looptijd; badges *M3 not confirmed*, *M3 travel time too short* en *M3 travel time too long*; beide eindsensoren tegelijk actief geldt als sensorstoring |
| 1.24 | 2026-09-20 | 2.11.0 — de kalibratie (commissioning) van de raamstandsensor werkt op elke build, niet langer alleen op een commissioning-build (gh#77) |
| 1.25 | 2026-09-20 | 2.12.0 — **lineaire besturing van M3**: de instelling *M3-besturing* (Tijdgestuurd / Lineair), de *Minimale tussentijd* die in lineaire besturing de rusttijden van M3 vervangt, en de regel eronder die zegt welke besturing werkelijk actief is |
| 1.26 | 2026-09-24 | 2.12.2 — in lineaire besturing wordt een stand vlak bij een eindstand een volledige gang tot de eindsensor, zodat "dicht" echt dicht is (gh#83); tab **Log**: nieuwste bestanden bovenaan, het huidige bestand gemarkeerd, een melding als de kaart meer bestanden heeft dan de lijst toont, en de kaart houdt weer 30 bestanden per controller (gh#82). Ook: hoe de T-as en de RH-as worden gecombineerd; en de SD-cijfers gecorrigeerd in §10.7, de upload-sectie, *Vrije ruimte en bestandsrotatie* en Bijlage F — rotatie bij 1 MB, 30 bestanden per controller, vloer 5, minimaal 4 MB vrij (stond op 512 KB, 10, 3 en 2 MB) — plus de automatische mount die een Unmount respecteert |
| 1.27 | 2026-09-25 | 2.13.0 — **Lineair is de fabrieksinstelling** (alleen voor een nieuwe of teruggezette controller); de lineaire besturing begint vanzelf, ook in rust (gh#86); de regel onder *M3-besturing* noemt de precieze reden als lineair niet actief is, in grijs als er niets mis is (gh#85) |
| 1.28 | 2026-09-25 | 2.14.0 — de LED-helderheid en de nachtstand (22:00–06:00) liggen vast en zijn geen instelling meer (gh#67); checklist §18 punt 10: de raamstandsensor aanmelden in plaats van de LED instellen |

---

*Einde van de beheerder-handleiding.*
