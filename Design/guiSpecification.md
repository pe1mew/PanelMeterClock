# GUI / User Interface Specification
## PanelMeterClock

---

## 1. Document Control

### 1.1 Identification

| Field | Value |
|-------|-------|
| Document title | GUI / User Interface Specification — PanelMeterClock |
| Document ID | PMC-GUI-001 |
| Version | 0.1 (draft) |
| Date | 2026-06-08 |
| Author | Remko Welling |
| Status | Draft — under review |

### 1.2 Revision History

| Version | Date | Author | Change Summary |
|---------|------|--------|----------------|
| 0.1 | 2026-06-08 | Remko Welling | Initial draft — physical operator interface (front panel, status LEDs, rotary encoder, connectors) |

### 1.3 Relationship to Other Documents

| Document | Role |
|----------|------|
| PMC-FRS-001 Functional Requirements Specification | Defines *what* the clock does; the embedded **web GUI** is specified there (§12) |
| PMC-STD-001 Software Technical Design | Firmware design; will own the LED-status and encoder/set-mode logic |
| PMC-HTD-001 Hardware Technical Design | Hardware design; will own the LED and encoder GPIO assignment and circuitry |

This document specifies the **physical operator interface** of the clock — the front-panel display and indicators, the local control, and the user-facing connectors. The browser-based configuration interface is **out of scope** here and is specified in PMC-FRS-001 §12.

---

## 2. Introduction

### 2.1 Purpose

This document defines the clock's physical user interface so that a user can read the time, see the health of each time source, and set the time locally without any network or receiver. Each requirement is paired with an acceptance / verification criterion. Implementation detail (GPIO assignment, drive circuitry, firmware structure) is specified in the Technical Design (PMC-HTD-001 / PMC-STD-001).

### 2.2 Scope

**In scope:**
- Front-panel layout: three panel meters and four status LEDs
- Meaning and behaviour of the four time-source status LEDs (RTC, DCF, NTP, GNSS)
- Local control: rotary encoder with push button, and the manual time-set procedure
- User-facing connectors on the right side (GNSS antenna, USB-C)

**Out of scope:**
- The embedded web GUI (PMC-FRS-001 §12)
- Electrical, mechanical and GPIO detail (PMC-HTD-001) and firmware structure (PMC-STD-001)
- Enclosure industrial design, meter face artwork

### 2.3 Definitions

| Term | Definition |
|------|------------|
| RTC | The clock's battery-backed **real-time clock** (DS1307Z) — it retains the time across power loss, is shown when no external source (GNSS / NTP / DCF77) is governing, and can be set manually with the rotary encoder. Priority 4 in PMC-FRS-001 §5.4; see PMC-HTD-001 §8 and PMC-STD-001 §5.11. |
| Status LED | One of four front-panel white indicators showing the state of one time source |
| Encoder | Rotary encoder with an integral momentary push button, used for local operation |
| Set mode | The state entered from the encoder in which the user adjusts the RTC time |
| Active source | The highest-priority valid time source currently driving the display (GNSS > NTP > DCF77 > RTC) |

---

## 3. Physical Layout

### 3.1 Front Face

The front presents the three analog panel meters with the four status LEDs in a vertical line to the right of the meters.

```
 FRONT VIEW
 ┌────────────────────────────────────────────────────┐
 │                                                    │
 │    +---+       +---+       +---+                   │
 │    │ H │       │ M │       │ S │        ● RTC      │
 │    +---+       +---+       +---+        ● DCF      │
 │   Hours      Minutes     Seconds        ● NTP      │
 │                                         ● GNSS     │
 │                                                    │
 └────────────────────────────────────────────────────┘
  └── three moving-coil panel meters ──┘└ four white ┘
                                          status LEDs
                                          (vertical)
```

### 3.2 Right Side (not visible from the front)

The local control and the user-facing connectors are on the right-hand side of the enclosure.

```
 RIGHT SIDE
 ┌───────────────────────────┐
 │                           │
 │     (O)   rotary encoder  │
 │           + push button   │
 │                           │
 │      0    SMA — GNSS      │
 │           antenna         │
 │                           │
 │     [ ]   USB-C — power/  │
 │           programming     │
 │                           │
 └───────────────────────────┘
```

### 3.3 Layout Requirements

| ID | Requirement | Acceptance / Verification Criteria |
|----|-------------|------------------------------------|
| UI-LAY-001 | The front face shall present the three panel meters (hours, minutes, seconds) with four status LEDs in a vertical line immediately to their right. | From the front, three meters and a vertical row of four LEDs are visible, arranged as above. |
| UI-LAY-002 | The rotary encoder and the GNSS-antenna and USB-C connectors shall be on the right side, not on the front. | None of the control or connectors are visible from the front; all are reachable on the right side. |

---

## 4. Primary Display — Panel Meters

| ID | Requirement | Acceptance / Verification Criteria |
|----|-------------|------------------------------------|
| UI-DSP-001 | The three panel meters shall be the primary time display, showing local hours, minutes and seconds as specified in PMC-FRS-001 §7. | Meters read the current local time (see FR-DSP-001..003). |
| UI-DSP-002 | The hours, minutes and seconds meters shall be arranged left-to-right in that order. | Left meter = hours, centre = minutes, right = seconds. |

---

## 5. Status Indicators (Time-Source LEDs)

The four LEDs are a single colour (white); their meaning is conveyed by **position + label + state**, not colour. Each LED is labelled (RTC, DCF, NTP, GNSS).

**Indicator states:**

| State | Meaning |
|-------|---------|
| Off | The source is disabled or not present |
| Blinking | The source is enabled and acquiring / not yet valid |
| Steady on | The source currently has valid time data |

The clock always shows time from the **highest-priority valid source** (GNSS > NTP > DCF77 > RTC); the user reads the active source as the highest lit LED in that order.

| ID | Requirement | Acceptance / Verification Criteria |
|----|-------------|------------------------------------|
| UI-IND-001 | Four front-panel white LEDs shall indicate the status of the four time sources, ordered top-to-bottom: RTC, DCF, NTP, GNSS. | Four labelled LEDs are visible in that order. |
| UI-IND-002 | Each LED shall use the common state model (off / blinking / steady) to show its source's state. | A source with no data shows off or blinking; a source with valid data shows steady. |
| UI-IND-003 | The **GNSS** LED shall be steady on a valid fix, blinking while searching, and off when GNSS is disabled. | LED tracks GNSS fix/search/disabled states. |
| UI-IND-004 | The **NTP** LED shall be steady when recently synchronised, blinking while attempting, and off when there is no network. | LED tracks NTP sync/attempt/no-network states. |
| UI-IND-005 | The **DCF** LED shall be steady on a valid DCF77 decode, blinking while receiving/decoding, and off when DCF77 is disabled. | LED tracks DCF77 lock/decoding/disabled states. |
| UI-IND-006 | The **RTC** LED shall be steady when the clock is running on the battery-backed RTC (no GNSS/NTP/DCF source valid), and shall blink if the RTC time is invalid (e.g. a depleted backup battery). | With GNSS/NTP/DCF off the RTC LED is steady; with a depleted backup battery it blinks; when a higher source is valid it is off. |
| UI-IND-007 | The indicators shall make the active (governing) source identifiable. | The highest-priority lit LED (GNSS, else NTP, else DCF, else RTC) corresponds to the source driving the display. |
| UI-IND-008 | During manual time-setting (§6) the RTC LED shall blink to signal that set mode is active. | Entering set mode makes the RTC LED blink; leaving set mode stops it. |
| UI-IND-009 | The LEDs shall be readable from the front under normal indoor lighting. | All four LEDs are clearly distinguishable on/off at a normal viewing distance. |

---

## 6. Local Control — Rotary Encoder

A rotary encoder with an integral push button on the right side lets the user set the RTC when there is no connectivity (no GNSS / NTP / DCF77). It is the only physical control.

### 6.1 Set-Mode Interaction

```
        ┌───────────┐  long press (~2 s)  ┌───────────────┐
        │  NORMAL   │────────────────────►│  SET: HOURS   │  rotate ⟳ = hours ±
        │ (display) │                     └───────┬───────┘
        └───────────┘                             │ short press
              ▲                                   ▼
              │                           ┌───────────────┐
              │                           │ SET: MINUTES  │  rotate ⟳ = minutes ±
              │                           └───────┬───────┘
              │                                   │ short press
              │                                   ▼
              │                           ┌───────────────┐
              │                           │ SET: SECONDS  │  rotate ⟳ = seconds ±
              │                           └───────┬───────┘
              │     short press (apply)           │
              └───────────────────────────────────┘
   Inactivity timeout (~30 s) in any SET state → return to NORMAL without applying.
```

While in set mode, the **meter for the field being edited tracks the encoder** (there is no numeric readout); the other two meters hold their value. The RTC LED blinks (UI-IND-008).

| ID | Requirement | Acceptance / Verification Criteria |
|----|-------------|------------------------------------|
| UI-CTL-001 | A rotary encoder with push button shall be provided on the right side for local operation without connectivity. | An encoder with push button is present and usable on the right side. |
| UI-CTL-002 | A long press shall enter time-set mode, starting at the Hours field. | Holding the button enters set mode with Hours active. |
| UI-CTL-003 | Rotating the encoder shall adjust the active field (clockwise = increase, anticlockwise = decrease), wrapping at the field limits (hours 0–23, minutes/seconds 0–59). | Rotation changes the active field and wraps at its limits. |
| UI-CTL-004 | A short press shall advance to the next field in the order Hours → Minutes → Seconds. | Each short press moves to the next field. |
| UI-CTL-005 | A short press after the Seconds field shall apply the entered time to the RTC and return to normal display. | Completing the sequence sets the clock to the entered time and exits set mode. |
| UI-CTL-006 | While a field is being edited, the corresponding meter shall track the value being set. | Editing hours moves the hours meter; same for minutes and seconds. |
| UI-CTL-007 | Inactivity in set mode for a configurable timeout shall exit without changing the time. | No input for the timeout returns to normal display with the time unchanged. |
| UI-CTL-008 | A manually-set time shall be overridden when a higher-priority source becomes valid, per PMC-FRS-001 §5.4. | After a manual set, an arriving GNSS/NTP/DCF77 time takes over automatically. |
| UI-CTL-009 | In normal display mode the encoder shall not change the displayed time (only a long press enters set mode), preventing accidental changes. | Rotating or short-pressing in normal mode does not alter the time. |

---

## 7. Connectors (Right Side)

| ID | Requirement | Acceptance / Verification Criteria |
|----|-------------|------------------------------------|
| UI-CON-001 | An SMA connector for the external GNSS antenna shall be accessible on the right side. | An SMA antenna connector is present on the right side (see PMC-HTD-001 §6.5). |
| UI-CON-002 | A USB-C connector shall be accessible on the right side for powering and programming the clock. | The clock powers from, and can be programmed over, the right-side USB-C port (see PMC-HTD-001 §9–§10). |

---

## 8. Traceability

| GUI requirement | Related FRS | Related TDS |
|-----------------|-------------|-------------|
| UI-DSP-001..002 (meters) | FR-DSP-001..003 | PMC-STD-001 §5.2, PMC-HTD-001 §3–§5 |
| UI-IND-001..009 (status LEDs) | FR-BOOT-021, FR-GPS-005, FR-NTP-004, FR-DCF-007, FR-RTC-005, §5.4 | PMC-STD-001 §5.12, PMC-HTD-001 §3 (GPIO 4–7) |
| UI-CTL-001..009 (encoder / set) | §5.4 (RTC = Priority 4), FR-RTC-004, FR-BOOT-001..004 | PMC-STD-001 §5.11–§5.12, PMC-HTD-001 §3 (GPIO 13/14/47) |
| UI-CON-001 (SMA) | — | PMC-HTD-001 §6.5 |
| UI-CON-002 (USB-C) | — | PMC-HTD-001 §9, §10 |

Test methods and pass conditions for these requirements belong in PMC-STD-001 §8 (firmware-observable behaviour) and PMC-HTD-001 §13 (hardware checks).
