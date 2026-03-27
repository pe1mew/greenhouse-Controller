# hardware/fabrication

This directory stores the fabrication outputs generated from the KiCad PCB design in [`../pcb/`](../pcb/).

A new set of fabrication outputs is generated and committed here for every tagged hardware release (e.g. `hw-v1.0`).

## Expected contents (per release tag)

| File / folder | Description |
|---------------|-------------|
| `gerbers/` | Gerber files for all copper layers, silkscreen, soldermask, and board outline |
| `drill/` | Excellon drill file(s) |
| `bom.md` | Bill of Materials — component references, values, footprints, and supplier part numbers |
| `pick-and-place.csv` | Component placement file for SMT assembly |
| `fabrication-notes.md` | Board stackup, surface finish, quantity, and any special instructions for the PCB manufacturer |

## Generating outputs from KiCad

1. Open the PCB layout in KiCad (`hardware/pcb/greenhouse-controller.kicad_pcb`).
2. **Gerbers:** *File → Fabrication Outputs → Gerbers* — output to `fabrication/gerbers/`.
3. **Drill:** *File → Fabrication Outputs → Drill Files* — output to `fabrication/drill/`.
4. **BOM:** *Tools → Generate BOM* (or use the built-in BOM plugin) — output to `fabrication/bom.csv`.
5. **Pick-and-place:** *File → Fabrication Outputs → Component Placement* — output to `fabrication/pick-and-place.csv`.
6. Commit all generated files and tag the release.
