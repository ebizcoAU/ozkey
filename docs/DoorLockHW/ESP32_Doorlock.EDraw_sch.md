Generate a circuit schematic diagram for an ESP32-C6 doorlock baseboard.

## Board Overview
- Board name: ESP32-C6 Doorlock Baseboard
- Board revision: Rev C / V3.1
- Board size: 35mm × 25mm
- Core processor: ESP32-C6-MINI-1-N8 module with on-board PCB antenna

## Power Supply Section (Left side, top to bottom)
- J1: 2-pin JST-XH battery connector (2.50mm pitch). Pin 1 = VBAT (+6.0V from 4xAA batteries), Pin 2 = GND. Current rating: 3A.
- D1: SS12 Schottky diode (SOD-123FL). Reverse polarity protection. Connected in-series with battery positive line.
- D2: SMBJ8.5A TVS diode (SMB/DO-214AA). ESD/surge protection. Connected across J1 input rails.
- C1: 22µF, 16V, 0805 bulk input capacitor. Connected between VIN and GND. Place close to U2 VIN pin.
- U2: 1.5A Synchronous Buck Converter (SOT-23-5L). Steps 6.0V battery input down to 3.3V. Efficiency: 92-95%. Peak current: 950mA.
- L1: 2.2µH power inductor. Connected to U2 SW pin. Place near U2.
- C2: 15µF, 16V, 0805 bulk output capacitor. Connected between VOUT and GND. Place close to U2 VOUT pin.
- Label the LDO as "Buck Converter: 6.0V → 3.3V, 1.5A, 92-95% efficient"

## Main IC (Center)
- U1: ESP32-C6-MINI-1-N8 module. Size: 13.2mm × 16.6mm. Castellation pads. 8MB flash, on-board PCB antenna.
- Label: "ESP32-C6-MINI-1-N8 | 8MB Flash | PCB Antenna | XIAO 21×17.5mm"
- Connect 3.3V rail to VDDPST1 and VDDPST2.
- Connect all GND pins to ground plane.
- R1: 10kΩ pull-up resistor from EN pin to 3.3V.
- R2: 10kΩ pull-up resistor from GPIO8 (BOOT) to 3.3V.
- Y1: 32.768 kHz quartz crystal (3.2mm × 1.5mm SMD). Connected to U1 clock pins. Add C5 and C6 load capacitors (values per crystal spec, typically 12.5pF). Label: "Y1: 32.768 kHz — offline passcode accuracy".
- Antenna keep-out: 5mm zone from module antenna edge. No copper, traces, or components in this zone. Module antenna faces outward.

## MCU Interface (Right side, top to bottom)
- J2: 5-pin right-angle header (2.54mm pitch). Label "To Doorlock MCU (Tuya Keep-Alive)".
  - Pin 1: TXD (GPIO0) → arrow right (module → MCU)
  - Pin 2: RXD (GPIO1) → arrow left (MCU → module)
  - Pin 3: SRDY (GPIO3) → arrow left (active-low, MCU wakes module). R3: 10kΩ pull-up to 3.3V.
  - Pin 4: MRDY (GPIO2) → arrow right (active-low, module wakes MCU). R4: 10kΩ pull-up to 3.3V.
  - Pin 5: GND (dedicated ground return)
- J3: 3-pin right-angle header (2.54mm pitch). I2C Expansion.
  - Pin 1: SDA (GPIO22)
  - Pin 2: SCL (GPIO23)
  - Pin 3: GND
- J4: 2-pin right-angle header (2.54mm pitch). SPARE GPIO.
  - Pin 1: GPIO6
  - Pin 2: GPIO7
- J5: 6-pin right-angle header (2.54mm pitch). SPI Camera Interface.
  - Pin 1: SCK (GPIO19)
  - Pin 2: MOSI (GPIO18)
  - Pin 3: MISO (GPIO20)
  - Pin 4: CS (GPIO21)
  - Pin 5: 3V3
  - Pin 6: GND

## Programming Interface (Bottom, interior)
- J6: 6-pin surface-mount POGO pads. No connector housing. Label "POGO Programming Interface".
  - Pin 1: 3V3
  - Pin 2: TXD0 (GPIO16)
  - Pin 3: RXD0 (GPIO17)
  - Pin 4: GND
  - Pin 5: EN (GPIO9) — 10kΩ pull-up to 3.3V
  - Pin 6: BOOT (GPIO8) — 10kΩ pull-up to 3.3V

## Decoupling Capacitors
- C3, C4: 0.1µF, 16V, 0402 decoupling capacitors. Place close to U1 VDDPST1 and VDDPST2.

## Notes & Labels
- Protocol: "UART 115200 8N1, 0x55AA (Tuya standard)"
- Topology: "Keep-Alive — module always powered"
- Strapping pins: "ESP32-C6 Strapping Pins: GPIO4, GPIO5, GPIO8, GPIO9, GPIO15. GPIO8/9 used with 10k pull-ups. Boot modes: GPIO8=1, GPIO9=0 = Download Boot; GPIO8=x, GPIO9=1 = Normal Boot; GPIO8=0, GPIO9=0 = INVALID"
- Revision: "Rev C / V3.1 — 2026-08-01"
- Board size: "35mm × 25mm"
- SW1/SW2 removed — POGO handles programming

## Style Requirements
- Use standard circuit symbols
- Show all power, ground, and signal connections clearly
- Arrange components in a clean, professional layout
- Label all components with reference designators
- Add pin numbers on all connectors
- Show net labels for power and signal connections
- Use hierarchical organization: Power Supply (left), Main IC (center), Connectors (right)