Objective: Generate the final engineering documentation (Schematic, Netlist, BOM, Layout Notes) for a custom doorlock PCB. This design is based on the Seeed Studio XIAO ESP32-C6 reference design, but with specific modifications to optimize it for a battery-powered, 6V (4×AA) doorlock application.

Critical Update: To support both the Doorlock MCU (UART1) and the Tuya Camera MCU (SPI) without pin conflicts, we are sacrificing the general-purpose I/O pins D4 and D5 and reassigning them to UART1 functions. D0, D1, and D2 remain as spare GPIOs.

---

## ⚠️ REVISION 2026-07-29 — READ BEFORE MANUFACTURE

Three corrections to the revision above. **The first one is blocking: as
originally specified the board could not be programmed at all.**

### R1 (BLOCKING) — UART0 RX must be GPIO17, not GPIO7

The ESP32-C6 **ROM bootloader's** UART0 download interface is fixed at
**GPIO16 (TX) / GPIO17 (RX)**. UART0 can be remapped in *application* code, but
**not in ROM** — and serial download mode runs in ROM, before any application
exists. With the USB-C connector removed, UART0 is the **only** programming and
recovery path on this board.

The original mapping put UART0 RX on GPIO7 and handed GPIO17 to UART1 TX (the
doorlock MCU). esptool would transmit into a pin nothing is listening on, and
the board would be unflashable — including for factory rework, field firmware
recovery and RMA, with no USB fallback.

**Resolution — swap UART1 RX and UART0 RX:**

| Signal | Was | **Now** | Note |
|---|---|---|---|
| UART0 RX (J3, programming) | GPIO7 ❌ | **GPIO17** | ROM-fixed, non-negotiable |
| UART1 RX ← doorlock MCU TX (J2) | GPIO17 | **GPIO7** | GPIO7 is LP-capable, no loss |

GPIO16 (UART0 TX) is unchanged and already correct. All J2/J3 tables, the
netlist and the manufacturing notes below have been updated accordingly.

### R2 — Firmware build flag: USB CDC must be OFF for this board

With no USB-C, `Serial` must be routed to the **UART0 pins**, not USB CDC.
Production builds therefore use `CDCOnBoot=default` (Disabled); only the
Waveshare / XIAO **dev** boards use `CDCOnBoot=cdc`. Getting this wrong
produces a board that flashes fine but is **completely silent on J3** — the
failure looks like dead hardware and wastes hours. (Learned the hard way on the
bench, 2026-07-28: the inverse mistake silenced serial on both dev boards.)

### R3 — LDO halves battery life; consider a buck

`HT7833-3.3` is the right call for the 28 V rating and 6.6 V fresh-cell input,
and dropping the buck simplifies the BOM. But it is a **linear** regulator: at
6 V in / 3.3 V out roughly **45% of every mAh is dissipated as heat**,
permanently. 4×AA at ~2800 mAh yields only ~1540 mAh of useful energy.

Also check thermals: at Wi-Fi TX peaks (~300 mA) the LDO dissipates ~0.8 W —
a lot for SOT-23-3, marginal for SOT-89. Prefer **SOT-89** if the LDO is kept.

A modern buck with nanoamp-class quiescent current (TPS62740-class, ~360 nA)
would recover most of that. The classic objection — quiescent drain dominating
in a device asleep 99.9% of the time — no longer holds at those figures.

**DECIDED (operator, 2026-07-29): KEEP THE LDO — HT7833 in SOT-89 — for the
10-unit prototype run.** Rationale: this board already carries ~10 departures
from the proven XIAO reference (USB-C removed, charger removed, buck removed,
LDO swapped with a footprint change, four new connectors, UART/SPI rerouting).
Adding a switching regulator — inductor placement, feedback network, careful
return-path routing — on a cramped 21×17.5 mm outline introduces another
independent source of first-run layout error. ~1.4 years of runtime is already
acceptable for a doorlock. SOT-89 addresses the thermal concern. Revisit for
rev 2 only if field data justifies it.

**How to get that field data without a respin:** fit a small external buck
module inline on the 6 V battery lead of one or two prototypes and compare
runtime against LDO units under identical duty cycle. That measures the real
benefit on real firmware before committing any board area.

**Also worth measuring in field test — end-of-life cutoff.** An LDO cannot
regulate once V_in falls below V_out + dropout, so a 4×AA pack becomes
"flat" from the lock's perspective at roughly 3.6–3.8 V (dropout is worst at
Wi-Fi TX peaks). A buck keeps working further down the discharge curve, so its
advantage is *more* than the ~45% conversion figure alone suggests. Record the
actual cutoff voltage in testing — it is the number that decides rev 2.

### R4 (informational) — firmware needs a production pin profile

This board differs from the bench hardware, so `blelock/doorlock/doorlock.ino`
needs a production pin set before the first unit is powered:

| | Bench (Waveshare) | **Production (this board)** |
|---|---|---|
| Tuya UART TX / RX | 16 / 17 | **17 / 7** |
| SRDY / MRDY | 7 / 8 | **3 / 23** |
| LCD + touch | GPIO 1,2,14,15,22,23 + I2C 18,19,20,21 | **absent** |

`SRDY` on GPIO3 remains LP-capable, so deep-sleep wake is preserved.

---

The final output should be a comprehensive document that clearly outlines the schematic changes and the physical modifications to the PCB, ready for manufacturing by Seeed Studio.

PART 1: CORE HARDWARE STRATEGY – MODIFIED XIAO PCB
Design Basis:

This custom PCB is strictly derived from the Seeed Studio XIAO ESP32-C6 reference design (PCB version R1.0).

Form Factor: The PCB outline must remain 21.0mm × 17.5mm.

Core Layout: The ESP32-C6-MINI-1-N8 module, antenna tuning, crystal/oscillator circuitry, SW1 (Reset/EN), and SW2 (Boot/GPIO9) must remain 100% unchanged from the standard XIAO board.

Component Removals:

Omit from Assembly: The USB-C connector, SGM40567 battery charger IC, SGM6029C buck converter, and all their associated passives (resistors, capacitors, inductors) must be completely removed from the BOM. The PCB pads may remain unpopulated.

Component Replacement (Critical):

LDO (U2): Replace the existing LDO with an HT7833-3.3 (SOT-89 or SOT-23-3 package).

Footprint Change: The PCB footprint for U2 must be changed from SOT-23-5 (for the original SGM2040) to SOT-89 or SOT-23-3. Route VIN, VOUT, and GND accordingly. The Enable pin is no longer present.

Reason: The HT7833 handles up to 28V input, safely accepting the 6.6V from fresh 4×AA batteries, while the original LDO would be dangerously over-volted.

Internal Routing & Sacrificed Pins:

Sacrificed Pins: To resolve pin conflicts between UART1 and SPI, we are sacrificing the edge connector pins D4 (GPIO22) and D5 (GPIO23) for UART1 functions. They will no longer be available as general-purpose I/O.

Non-functional Edge Pins: The edge castellations D3, D4, D5, D8, D9, D10, and the 5V pad will remain physically present on the PCB outline. However, they will be left unconnected (floating) on the PCB for this variant.

Spare GPIOs: D0 (GPIO0), D1 (GPIO1), and D2 (GPIO2) remain as free, spare general-purpose I/O for future sensors or LEDs.

PART 2: SCHEMATIC & UPDATED PIN MAPPING (Conflict-Free)
Update the schematic with the following exact pin mapping. Ensure that all traces are rerouted internally from the ESP32-C6 module directly to the new grouped connectors, bypassing the edge castellations.

J1 – Battery Connector (2-pin, 2.54mm pitch)
Located on the bottom edge.

Pin 1: VIN (6V from 4×AA battery)

Pin 2: GND

J2 – Doorlock MCU Connector (5-pin, 1.27mm or 2.0mm pitch)
Located on the bottom edge, grouped with J1 and J3.

Pin	Function	GPIO	XIAO Label	Notes
1	UART1 TX → MCU RX	GPIO22	D4	**R1: was GPIO17 — GPIO17 is reserved for ROM UART0 RX**
2	UART1 RX ← MCU TX	GPIO7	D7	**R1: was GPIO22. GPIO7 is LP-capable**
3	SRDY (MCU → Module wake)	GPIO3	(Internal)	Fixed internal pin, LP-capable (deep-sleep wake)
4	MRDY (Module → MCU wake)	GPIO23	D5	Sacrificed D5. Idle HIGH
5	GND	-	-	Dedicated ground reference
J3 – Programming / Debug Connector (4-pin, 1.27mm or 2.0mm pitch)
Located on the bottom edge, grouped with J1 and J2.

Pin	Function	GPIO	XIAO Label	Notes
1	3.3V (Output)	-	3V3	-
2	UART0 TX → USB-UART RX	GPIO16	D6	ROM-fixed
3	UART0 RX ← USB-UART TX	**GPIO17**	(Internal)	**R1: was GPIO7. ROM-fixed — the board is unflashable on any other pin**
4	GND	-	GND	-
J4 – SPI Bus for Tuya Camera MCU (6-pin, 1.27mm or 2.0mm pitch)
Located on the bottom edge, grouped with the others.

**R5 (2026-07-29): widened 6-pin → 7-pin to add a data-ready interrupt.**

Pin	Function	GPIO	XIAO Label	Notes
1	SPI SCK	GPIO19	D8	Dedicated SPI clock
2	SPI MOSI	GPIO18	D10	Now dedicated to SPI
3	SPI MISO	GPIO20	D9	Now dedicated to SPI
4	SPI CS	GPIO21	D3	Chip select
5	**CAM_DRDY (camera MCU → module, IRQ)**	**GPIO2**	**D2**	**R5 — see below. Spare GPIO reallocated**
6	3.3V	-	3V3	Power for camera MCU
7	GND	-	GND	Common ground

**Why R5 is needed:** the C6 is SPI **master**; the camera MCU is the slave.
A slave cannot initiate a transfer, so without a dedicated line the master must
**poll** for "frame ready" — which wastes power on a battery device and adds
latency to a live video path. One interrupt line lets the C6 sleep until the
camera actually has data. This is standard for any SPI-attached streaming
peripheral and is very cheap to add now; it cannot be retrofitted after tooling.

Remaining spare GPIOs after R5: **D0 (GPIO0), D1 (GPIO1)** — both ADC- and
LP-capable, so either can serve a tamper switch, status LED, piezo, or a
wake-on-motion PIR for the video variant.
Strapping Pins (Do NOT Touch):

GPIO8: Pulled HIGH via R3. Do not route to any connector.

GPIO9: Pulled HIGH via R2 + SW2. Do not route to any connector.

PART 3: UPDATED NETLIST & BOM
Netlist Updates (Critical):

VBAT_IN → J1.1, D1 anode.

VIN_PROT → D1 cathode, D2, C1+, U2.VIN.

+3.3V → U2.VOUT, C2+, U1.VDDPST1/2, C3+, C4+, R1-R5 tops, J3.1, J4.5.

GND → J1.2, D2 cathode, C1-, U2.GND, C2-, U1.GND, C3-, C4-, SW1/2 far side, J3.4, J2.5, J4.6.

EN → U1.EN, R1 bottom, SW1 near side.

GPIO8 → U1.GPIO8, R3 bottom.

GPIO9 → U1.GPIO9, R2 bottom, SW2 near side.

TXD_DBG (UART0 TX) → U1.GPIO16, J3.2.

RXD_DBG (UART0 RX) → U1.**GPIO17**, J3.3. **(R1: changed from GPIO7 — ROM-fixed download pin, non-negotiable)**

TXD_MCU (UART1 TX) → U1.**GPIO22**, J2.1. **(R1: changed from GPIO17)**

RXD_MCU (UART1 RX) → U1.**GPIO7**, J2.2. **(R1: changed from GPIO22)**

SRDY → U1.GPIO3, J2.3, R4 bottom.

MRDY → U1.GPIO23, J2.4. (Note: Changed from GPIO20)

SPI_SCK → U1.GPIO19, J4.1.

SPI_MOSI → U1.GPIO18, J4.2.

SPI_MISO → U1.GPIO20, J4.3.

SPI_CS → U1.GPIO21, J4.4.

CAM_DRDY → U1.**GPIO2**, J4.5. **(R5: new interrupt line, was a spare GPIO)**

BOM Updates:

U2 → HT7833-3.3 (SOT-89 or SOT-23-3 package). (Remove SGM2040-3.3)

Remove USB-C Connector, SGM40567 Charger IC, SGM6029C Buck Converter, and all associated passives.

J1 → 2-pin, 2.54mm pitch battery connector (e.g., JST-XH).

J2 → 5-pin, 1.27mm or 2.0mm pitch connector.

J3 → 4-pin, 1.27mm or 2.0mm pitch connector.

J4 → **7-pin** (R5: was 6-pin), 1.27mm or 2.0mm pitch connector.

U2 packaging note (R3): if the HT7833 LDO is retained, prefer **SOT-89** over SOT-23-3 — at Wi-Fi TX peaks (~300 mA) it dissipates ~0.8 W, which SOT-23-3 will struggle to shed.

PART 4: SEEED STUDIO MANUFACTURING NOTES
Include the following exact notes at the bottom of the document for Seeed Studio:

Design Basis & Layout Note (Modified XIAO Variant):

This PCB is a custom variant of the Seeed Studio XIAO ESP32-C6 reference design. The following mandatory modifications are applied:

USB-C & Charger Removed: The USB-C connector, SGM40567 charger IC, SGM6029C buck converter, and all associated passives are omitted from the BOM and assembly.

LDO Replaced: The LDO (U2) is replaced with an HT7833-3.3 (SOT-89 or SOT-23-3 package). The PCB footprint must be changed from SOT-23-5 to SOT-89/SOT-23-3. Route VIN, VOUT, and GND accordingly.

Edge Connectors (D3-D10, 5V): These castellations are physically present on the PCB outline but are not routed internally (left floating). They are non-functional on this variant.

New Connector Grouping (Bottom Edge): The freed area at the bottom edge (where the USB-C port was located) now hosts four grouped connectors (J1, J2, J3, J4) as per the pin tables above.

Pin Conflict Resolution (Sacrificed Pins): To provide a dedicated SPI bus for the camera and a dedicated UART1 for the doorlock MCU, the board sacrifices D4 (GPIO22) and D5 (GPIO23) for UART1 TX and MRDY, and D7 (GPIO7) for UART1 RX. GPIO18 and GPIO20 are now dedicated to SPI MOSI and SPI MISO. D2 (GPIO2) becomes CAM_DRDY.

Programming (REVISED — R1): Programming and debugging are performed via J3 using an external USB-UART adapter, on **UART0 = GPIO16 (TX) / GPIO17 (RX)**. **GPIO17 is mandatory and cannot be substituted**: the ESP32-C6 ROM bootloader's serial-download interface is fixed to GPIO16/GPIO17, and application-level UART remapping does not apply in ROM. With the USB-C connector omitted, this is the board's ONLY programming and recovery path — an error here yields an unflashable board with no fallback, including for factory rework and RMA.

J4 Interrupt Line (R5): J4 is a 7-pin connector (was 6). Pin 5 is CAM_DRDY (GPIO2), a camera-MCU→module interrupt so the module need not poll for frame-ready — required for acceptable power and latency on the video variant.

Firmware Build Flag (R2): Production firmware for this board MUST be built with USB CDC on boot DISABLED (`CDCOnBoot=default`), so that `Serial` is routed to the UART0 pins on J3. Dev boards retaining USB use `CDCOnBoot=cdc`. A build with the wrong flag flashes successfully but is completely silent on J3.

All other aspects (ESP32-C6 module, antenna, crystal, 3×3mm buttons SW1/SW2, and 21×17.5mm form factor) remain identical to the standard XIAO ESP32-C6.

