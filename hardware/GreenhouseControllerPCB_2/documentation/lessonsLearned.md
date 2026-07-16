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
 
 
