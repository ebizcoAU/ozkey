# Device profiles — the DP map as data

**One profile file, two consumers.** `locksim/` and the doorlock firmware load
the *same* JSON. They therefore cannot disagree about what a DP number means —
which is how the bench stops validating our own fiction (`ozkey-27 §2.1`).

## Why it is split into a catalogue + products

`ozkey-27 §3`: Tuya DP numbers are **not** invented per manufacturer. They come
from a **standard catalogue per product category**, and a manufacturer *selects
a subset* at PID creation. Proof: DP 42 and DP 76 (`unlock_ble`) carry the same
meaning across two different products in two different categories.

So a dozen suppliers is **not** a dozen DP lists to write. It is one catalogue
plus a dozen short selections:

```
profiles/
  tuya-lock-catalogue.json     the standard. Add a DP here ONCE.
  products/
    tuya-ds013-t3.json         Smart Lock DS013-T3  — selects a subset
    tuya-t3-videolock.json     Video Lock (partial) — selects a subset
    ozkie-legacy-v0.json       OUR INVENTED MAP. Self-contained. See below.
```

A new supplier is normally a ~20-line selection file. Only genuinely
product-specific DPs get defined inline in the product.

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
| `verb` | the OZKIE v1 verb this maps to (`ozkey-28`) |
| `field` | which arg/field of that verb |
| `enum` / `range` / `codec` | type-specific |

### `status` — the honesty field

| Value | Meaning | Firmware behaviour |
|---|---|---|
| `confirmed` | type **and** payload semantics documented by the supplier | normal |
| `reserved` | DP exists and its type is known, but the **payload layout is not supplied** | reject with `UNSUPPORTED` |
| `unknown` | seen in the wild, meaning not established | log id + length only, never publish payload |
| `fiction` | **we made this up.** Only in `ozkie-legacy-v0` | works, but is a lie — do not port |

`reserved` is doing real work: `ozkey-27 §2.5` — every credential-write DP on
DS013-T3 is `raw`/`string` with a documented *type* and **no payload layout**.
We cannot write those codecs, so they must fail loudly rather than send
plausible-looking bytes at a door lock.

## Why `ozkie-legacy-v0` exists

It is our invented DP map — the one that ships in `doorlock-1.58` and that the
current BANOI build constructs frames against. Every entry is marked `fiction`.

**It is the default until the app migrates.** Swapping straight to the real
catalogue would break DoorA/DoorB and the shipping app on the next flash. Two
profiles, staged migration, no flag day (`ozkey-28 §1.1`).

## Not in here

**Pin numbers.** `link.srdy`/`link.mrdy` in a profile describe the *MCU's
convention* (assert level, level-vs-pulse). Which GPIO they land on is a
property of our board, not the supplier's — and we already have four different
answers (`ozkey-27 §8.4`). Board pinout is board config.

---

*See `ozkey-27` for the findings behind this design, `ozkey-28` for the verbs.*
