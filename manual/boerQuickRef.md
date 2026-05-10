# Quick-reference — Kascontroller voor de boer

**Versie 1.16.39** · 2026-05-10 · Volledige uitleg in `boerHandleiding.pdf` · Lamineren en naast de kast hangen.

### RGB-LED · in één oogopslag

| Kleur | `Mode:` | Wat doe jij? |
|---|---|---|
| **Groen** | `AUTO` — alles in orde | Niets |
| **Oranje** | Waarschuwing — wind, sensor of windbeveiliging-uit | Lees Mode-regel; bij `** SENSOR FAULT` → bel beheerder |
| **Rood** | `ALARM` — motor-noodstop, ramen niet meer aangestuurd | **Bel de beheerder** — niet zelf ingrijpen |

**Heartbeat-LED** (kleine amber): knippert 1×/sec = OK. Knippert niet → power-cycle (zie achterkant).

`[FOTO: vooraanzicht kascontroller met pijl naar RGB-LED, heartbeat-LED en LCD]`

### Bedrijfsmodi (Mode-regel op LCD scherm 3)

| LCD | Wat doet de controller? |
|---|---|
| `Mode: AUTO` | Regelt T en RH binnen de setpoints |
| `Mode: WIND` | Alle ramen dicht; klimaatregeling onderdrukt — wachten tot wind afneemt |
| `Mode: ALARM` | Alle relais uit; motoren staan stil — bel beheerder |
| `Mode:Window Cal.` | Sluit M1+M2+M3 voor kalibratie (~3 min) — wachten, niet ingrijpen |

> Reset na motor-alarm duurt **3–4 min** (60 s wachttijd + ~3 min kalibratie). Niet ingrijpen.

### LCD-statusschermen (auto-rotatie, elk 5 sec)

```
1 Temp/RH  →  2 Wind  →  3 Mode/Sess  →  4 WiFi  →  5 Tijd  →  6 Raamposities  →  (1)
```

Toets **`D`** = direct naar volgend scherm. Toets **`#`** = direct naar het bijhorende menu (werkt op de schermen met instellingen: T/RH, Wind, WiFi, Datum/tijd). Sensor-uitval: `** SENSOR FAULT` op rij 2 + rode achtergrond → bel beheerder.

<div style="page-break-before: always;"></div>

### Toetsenbord (4 × 4)

| Toets | Functie |
|:---:|:---|
| **D** | Volgende statusscherm |
| **#** | Snelweg → bijhorend instellingen-menu · in menu/PIN: bevestig |
| **\*** | Eén niveau terug · bij invoer: wis cijfer |
| **0–9** | Cijfer invoeren · op statusscherm: opent hoofdmenu |
| **A / B** | Vorige / volgende setpoint · `B` = teken ± bij invoer |
| **C** | Geen functie |

Vanaf elk statusscherm opent **elke toets behalve `D`** het hoofdmenu.

**Inloggen** — *LCD*: druk willekeurige toets (niet `D`) → menu → 4-cijferige Farmer-PIN → `#`. **Default `1234` — direct wijzigen.** *Web*: lees IP van LCD-scherm 4 → open in browser op zelfde wifi → tab **Access** → Farmer-PIN. Sessie verloopt na ~5 min inactief.

### FAQ — eerste actie

| Probleem | Doe dit |
|---|---|
| LCD blank / leeg | Heartbeat-LED zichtbaar? Zo niet → power-cycle |
| Heartbeat-LED knippert niet | Power-cycle |
| RGB-LED rood (`Mode: ALARM`) | **Bel beheerder** — zelf niets doen |
| RGB-LED oranje | Lees Mode-regel; `** SENSOR FAULT` → bel beheerder |
| PIN vergeten (Farmer) | Bel beheerder |
| Wind-alarm telkens terug | Beheerder kan windgemiddelde-venster langer zetten |

**Power-cycle** — (1) voeding eraf · (2) 10 sec wachten · (3) voeding terug; LCD licht binnen sec. op, kalibratie ~3 min, dan `Mode: AUTO`. ⚠ **Druk NIET op de BOOT-knop** — die start een fabrieksreset. Gebruik alleen de RESET-knop of de stroomschakelaar. `[FOTO: microprocessorboard met RESET-knop groen en BOOT-knop rood gemarkeerd]`

**Motorbox (Hotraco RRK-3)** — schakelaars **moeten op AUTO staan**, anders kan de controller niet sturen en werkt **windbeveiliging niet** (LCD blijft wel `Mode: WIND` tonen). Zie boerHandleiding §13. `[FOTO: motorbox-schakelaars in AUTO-stand]`

**Beheerder** · Naam: ____________________ · Tel: ____________________ · E-mail: ____________________
