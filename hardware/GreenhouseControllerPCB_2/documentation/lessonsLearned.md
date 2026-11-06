# Lessons learned on version 1.x of the greenhouse controller PCB

The following items shall be considerd for version 2.x of the greenhouse controller PCB. 

# Power supply

 - The internal 24V power supply may be replaced by a external Passive PoE (2-pair powering Pins 4, 5 (+) and pins 7, 8 (-)) such as: 
   - [Ubiquiti POE-24-24W-G (Hi-Power)](https://eu.store.ui.com/eu/en/products/poe-24) 
 - for this a RJ45 connector may be used.

## Interfaces

### Modbus

 - there shall be room for 2 modbus transceivers, bot connected to the ESP32S3
 - Modbus shall be using RJ45 connectors, preferrebaly vertical inserted. 
 - RJ45 connectors shall carry 24V passive PoE as specified in the ModBusRS485DualCableJunction and ModBusRS485SingleCableJunction PCB's 
 
# PCB design review

**Components**
- The two 20 kΩ pull-up resistors are unnecessary; the transceiver is already fail-safe.
- Seen from the connector, the TVS diode is not at the front of the circuit. Place it as the first component, directly after the connector.
- Capacitor C1 probably does little because of its large loop area and high inductance.

**EMC and layout**
- From an EMC standpoint, there will likely be a number of issues. Consider where the energy in a signal is located, and pay attention to the loop area of the return currents. This doesn't have to harm signal integrity, but it can.
- There are plenty of stitching vias, but also unintended antenna structures.
- Crosstalk is likely where traces run right next to each other over long distances while the ground plane is far away.
- A two-layer board quickly becomes problematic for EMC/EMI, especially if you can't make one side (almost) entirely GND.

**Recommendations**
- Add (resettable) fuses, filters and TVS diodes on the power connections, and filters on external signals.
- Do this at the input of the Mean Well supply and at the output of the buck converter as well
 
 
