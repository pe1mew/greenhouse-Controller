# hardware/pcb

This directory contains the **KiCad** PCB design files for the Greenhouse Ventilation Controller.

## Tool

| Item | Value |
|------|-------|
| Tool | KiCad EDA version 8 or later |
| Licence | KiCad is free and open-source (GPL) |

## Expected contents (to be added)

| File / folder | Description |
|---------------|-------------|
| `greenhouse-controller.kicad_pro` | KiCad project file |
| `greenhouse-controller.kicad_sch` | Schematic |
| `greenhouse-controller.kicad_pcb` | PCB layout |
| `sym-lib-table` | Project symbol library table |
| `fp-lib-table` | Project footprint library table |
| `libraries/` | Project-specific symbol and footprint libraries |

## Design references

- Target board dimensions and component placement are guided by the enclosure selected in TDS §4.10 (Multicomp Pro MC001110, 222 × 146 × 55 mm).
- GPIO and peripheral assignments are specified in TDS §4.11.
- Fabrication outputs (Gerbers, BOM, pick-and-place) are generated from this design and stored in [`../fabrication/`](../fabrication/).
