# Device profiles — the DP map as data

**One profile file, several consumers.** `locksim/`, the doorlock firmware (via
`ozprofile_gen.h`) and the app's model list (via `models.json`) all resolve from
the *same* JSON. They therefore cannot disagree about what a DP number means —
which is how the bench stops validating our own fiction (`ozkey-27 §2.1`).

## 🔴 There is no generic Tuya lock DP map

Tuya assigns DP numbers **per product category**, chosen at PID creation, and
categories reuse the same numbers for different things:

```
DP 76   unlock_ble  (Luona DS013-T3)   vs   fill_light   (Tuya Wi-Fi Lock Pro)
DP 21   navigation_volume              vs   liftup_double_lock
DP 45   battery_percentage             vs   palm-print unlock record (Zigbee)
```

**We ship remote unlock on DP 76.** On the right map it opens a door; on the
wrong one it turns on a lamp. So there is no safe "generic" fallback, and a
profile is **one real product**, never a family. Full argument:
`docs/DPSuppliers/genericDPList.md §1`.

## Layout

```
profiles/
  tuya-lock-catalogue.json     the standard list FOR ONE CATEGORY. Add a DP here ONCE.
  products/
    tuya-luona-ds013-t3.json   Luona Smart DS013-T3 — selects 34. THE ONE WE SHIP ON.
    tuya-wifi-lock-pro.json    Tuya's own published standard — standalone, 42 DPs.
    tuya-ladin-f7-t3.json      Ladin — STUB, 4 DPs, unusable (ozkey-42).
  models.json                  GENERATED — do not edit. See below.
```

A supplier in the **same category** is still ~20 lines: a `selects` list against
the catalogue. A supplier from a **different** category gets a `standalone`
profile with its own `entries` and must not select from the catalogue at all.
Check with the three-question fingerprint test in `genericDPList.md §5`.

**Deleted 2026-08-20** and not coming back: `ozkie-legacy-v0` (our invented map —
DP 1 unlock, DP 21-24 credentials, none of which exist on any real product),
`tuya-generic-lock` (Luona's map with the PID stripped out), `ozsim-fullfeature`,
`tuya-t3-videolock`. No entry anywhere is `status: fiction` any more.

## Resolution order

1. Start with the catalogue entries named in the product's `selects` list.
2. Apply `overrides` (same `dp`, product-specific enum or range).
3. Add `extra` (DPs that exist only on this product).
4. A product that sets `"standalone": true` skips the catalogue entirely.

## Entry shape

```json
{
  "dp": 60,
  "name": "alarm",
  "type": "enum",
  "dir": "up",
  "status": "confirmed",
  "verb": "event.alarm",
  "field": "type",
  "enum": { "0": "wrong_finger", "1": "wrong_password" }
}
```

| Field | Meaning |
|---|---|
| `dp` | the Data Point id |
| `name` | catalogue name. **Use Tuya's, verbatim** — renaming a published standard is how we got into this mess |
| `type` | `raw` \| `bool` \| `value` \| `string` \| `enum` \| `bitmap` |
| `dir` | `up` (MCU→us) \| `down` (us→MCU) \| `both` |
| `status` | see below |
| `verb` / `field` | the OZKIE verb this DP REPORTS (`ozkey-28`) |
| `verb_down` / `field_down` | the OZKIE verb this DP ACCEPTS as a command (catalogue rev 2) |
| `enum` / `range` / `codec` | type-specific |

🔴 **`dir: both` is what Tuya PERMITS, not what OZKIE should send.** 32 of 36
catalogue entries are `both`; only 20 carry a `verb_down`. **The absence of
`verb_down` is meaningful** — it is how we say "never command this DP".

### `status` — the honesty field

| Value | Meaning | Firmware behaviour |
|---|---|---|
| `confirmed` | type **and** payload semantics documented by the supplier | normal |
| `reserved` | DP exists and its type is known, but the **payload layout is not supplied** | reject with `UNSUPPORTED` |
| `unknown` | seen in the wild, meaning not established | log id + length only, never publish payload |
| `fiction` | we made it up. **No entry uses this today** — kept only so an old file cannot silently re-mean something | do not reintroduce |

`reserved` is doing real work: `ozkey-27 §2.5` — every credential-write DP on
DS013-T3 is `raw`/`string` with a documented *type* and **no payload layout**.
We cannot write those codecs, so they must fail loudly rather than send
plausible-looking bytes at a door lock. It is also why **no profile advertises
`pin_sync`**: capability is derived from what resolves to a `confirmed` DP.

## `in_lock` — DPs 101/102/103 belong on every profile

`bond_revoke`, `invite_cancel`, `list_bonds`. **Ours**, not Tuya's — our BLE
bond-management channel, handled inside the module and never forwarded to the
MCU, so they are product-independent.

🔴 They lived only on `ozkie-legacy-v0`; deleting it silently removed bond revoke
from every product and only the tests caught it. **101-103 are not reserved by
Tuya** — re-verify the allocation against every new supplier's DP list.

## Generated outputs — never edit by hand

`python3 blelock/tools/gen_profile.py` writes two files from this directory:

| output | consumer | contents |
|---|---|---|
| `blelock/common/ozprofile_gen.h` | firmware | flat PROGMEM DP + verb tables, no runtime parse, no heap |
| `profiles/models.json` | Nexus → the app (`XF-123`) | identity + `firmware_id`, `pairable`, `caps`, `verbs` |

- **`firmware_id` = `fw-{profile-id}-r{rev}`** — no build date, so it changes
  when the DP map changes and not when someone recompiles (`XF-123 §13.1`).
- **`pairable` is derived** — a profile earns it by having a PID *and* a complete
  DP map. Never a flag anyone sets.
- **`caps`/`verbs` in `models.json` are advisory.** The authoritative copy is the
  `verbs` array the lock itself reports at enrol (`XF-121`, `XF-123 §14.1`).
- The generator **refuses to build** an ambiguous command map, and
  `--check` (first thing in `npm test --prefix locksim`) fails if either output
  is stale.

## Choosing the profile: build time, not runtime

```
make -C blelock flash BOARD=19 PORT=/dev/cu.usbmodemXXXX PROFILE=tuya-luona-ds013-t3
```

**The far end of the UART gets no vote.** Firmware once adopted whatever the
MCU's `0x01` reply claimed and was seen switching maps mid-session while the
simulator's UI never moved (`XF-118 §4`) — anything that can put bytes on that
wire could redefine what a DP means. PID discovery now **confirms, never adopts**.

Omit `PROFILE=` and the build warns loudly at boot: it falls back to
`tuya-wifi-lock-pro`, chosen as the least-wrong default because it is a real
published standard, **but it is still a guess** about which lock is in the door.

## Not in here

**Pin numbers.** `link.srdy`/`link.mrdy` in a profile describe the *MCU's
convention* (assert level, level-vs-pulse). Which GPIO they land on is a
property of our board, not the supplier's — and we already have four different
answers (`ozkey-27 §8.4`). Board pinout is board config.

**Transport.** Command words (`0x06`/`0x07` vs the low-power set), the wake
handshake and module identity are per-vendor and are **not** in the DP list —
see `genericDPList.md §4`.

---

*`ozkey-27` for the findings behind this design · `ozkey-28` for the verbs ·
`docs/DPSuppliers/genericDPList.md` for the catalogue and its provenance ·
`XF-123` for the Nexus model registry.*
