<p align="center">
  <img src="../web/aquachemd.png" width="120" alt="AquachemD logo">
</p>
<h1 align="center">AquachemD</h1>
<p align="center"><b>Open-source, automated pool water chemistry — pH, ORP, and dosing, done right.</b></p>
<hr>
<br>
<br>

# AquachemD API Reference

AquachemD exposes the same set of devices, readings, and actions over two interfaces:

- **HTTP** — a REST-style API under `/api/`, for polling state and issuing one-off commands.
- **MQTT** — the same devices published continuously under a configurable root topic (`mqtt_aquachemd_topic`, default `aquachemd`), for live updates and Home Assistant discovery.

Both interfaces are driven by the same underlying dispatch logic, so wherever possible the same path/topic and value work identically on either transport. Differences between the two are called out explicitly below.

This document does not cover the WebSocket interface used internally by the web dashboard.

---

## Conventions

- **Device ID** (`<id>`) is whatever ID that device was given in `aquachemd.conf` (e.g. `PH_1`, `PMP_1`, `TNK_1`). IDs are user-configured, not fixed by the software — the prefixes used in the examples below (`TEMP_`, `PMP_`, `CS_`, etc.) reflect one particular install's naming convention, not a reserved scheme.
- **State/value semantics:**
  - `0` = Off
  - `1` = On
  - `2` = Enabled (armed — will turn on automatically when its dosing/condition logic calls for it)
  - `3` = Disabled (a safety condition is currently blocking it — see below)
  - `127` = sensor not configured / not present
- Binary/condition sensors report **`SAFE`/`UNSAFE`** (`status`) rather than On/Off, with `int_status` `1`/`0` respectively.

---

## HTTP API

Base URL: `http://<host>:<port>/api/` — port is whatever `listen_address` is configured to (`88` in the example below).

### Reading state — `GET /api/devices`

Returns every configured device and its current state in one call. Example (trimmed):

```json
{
  "AquachemD": "1.0.6",
  "type": "devices",
  "time": "08:53 AM",
  "date": "09/06/26 Sun",
  "devices": {
    "AquachemD": {
      "id": "AquachemD", "label": "AquachemD", "status": "ON", "int_status": 1,
      "type": "switch", "attributes": ["set_off", "set_on", "reset_stats"]
    },
    "CS_1": {
      "id": "CS_1", "label": "Flow Cell Level Sensor", "status": "SAFE", "int_status": 1,
      "type": "binary_sensor", "attributes": ["delay"],
      "delay_active": "OFF", "delay_duration": 0
    },
    "PMP_3": {
      "id": "PMP_3", "label": "Booster Pump", "status": "ON", "int_status": 1,
      "type": "switch", "value": 1,
      "attributes": ["set_on", "set_off", "set_enabled"]
    },
    "TEMP_1": {
      "id": "TEMP_1", "label": "Flow Cell Temperature", "status": "ON", "int_status": 1,
      "value": 28.887, "uom": "°C", "type": "sensor",
      "stats": { "id": "TEMP_1_stats", "avg": 28.78, "max": 28.887, "min": 28.542, "duration": "1w" },
      "attributes": ["stats", "reset_stats"]
    },
    "PMP_1": {
      "id": "PMP_1", "label": "Acid doser", "status": "DISABLED", "int_status": 3,
      "type": "switch", "value": 0,
      "timer_active": "OFF", "timer_duration": 0,
      "timer_default_runtime": 2, "timer_max_runtime": 60,
      "attributes": ["timer", "set_on", "set_off", "set_enabled", "dosestats", "valve", "ph_pump", "reset_dose_stats"]
    },
    "TNK_1": {
      "id": "TNK_1", "label": "Acid Tank Level", "status": "ON", "int_status": 1,
      "type": "level_sensor", "value": 91.96, "uom": "%",
      "alt_value": { "id": "TNK_1_alt", "value": 3.219, "uom": "gal" },
      "attributes": ["alt_value", "set_tank_level"]
    }
  }
}
```

Notable fields:
- **`attributes`** lists which actions/extras apply to that specific device — use this to discover what a device supports rather than hardcoding by device type.
- **`stats`** appears only on sensors with averaging enabled (`*_sensor_statistics` configured), giving the rolling `avg`/`max`/`min` over the configured `duration` (e.g. `"1d"`, `"1w"`).
- **`alt_value`** on tank/level sensors gives the reading in its original unit (gallons/mL) alongside the primary `value`, which is always a percentage.
- A `status` of `DISABLED` with `int_status: 3` means a safety interlock is currently blocking that device — check its condition sensors' `status` for which one.
- **`delay_active`/`delay_duration`** on a condition means it's configured to wait before being trusted after it's satisfied, even though its raw `status` may already show `SAFE`. This exists so sensors don't get read the instant a condition clears — e.g. a filter-pump condition with a 60-second delay means AquachemD waits a minute after the pump starts before trusting pH/ORP readings again, since water that sat stagnant in the pump/flow cell overnight reads inaccurately until fresh water has actually circulated through. While `delay_active` is `ON`, treat any sensor gated by that condition as not yet reliable.
- **`PMP_3`** above is a plain GPIO switch (`gpio_switch_*` in config) — same shape as a doser's `switch` type, but with no `timer_*` fields or chemical-role attribute (`ph_pump`/`orp_pump`), since it has no dosing behavior at all — just on/off/enabled control.

### Other read-only endpoints

These exist and are reachable at `/api/<name>`, but their response formats aren't fully documented here yet — treat as provisional until confirmed:

| Path | Purpose |
| :--- | :--- |
| `/api/homebridge` | Device list in a format tailored for the [homebridge-aquadaemon](https://github.com/aqualinkd/homebridge-aquadaemon) plugin. |
| `/api/schedules` | Current cron-based dosing schedules. |
| `/api/config` | Current running configuration. |
| `/api/dosestats` | Dosing history/statistics. |
| `/api/calibrate` | Sensor calibration state. |
| `/api/instantreading` | Forces/returns an immediate sensor read outside the normal poll cycle. |

### Sending a command

Commands are sent as `PUT` (or `POST`) requests to `/api/<id>/<action>`, with the value passed either as a query string (`?value=1`) or as a request body (`value=1`):

```bash
curl "http://localhost:88/api/PMP_1/set?value=1" -X PUT
# or
curl "http://localhost:88/api/PMP_1/set" -d value=1 -X PUT
```

| Path | Value | Effect |
| :--- | :--- | :--- |
| `<id>/set` | `0` / `1` / `2` | Set a switch/doser to Off / On / Enabled. |
| `<id>/timer/set` | seconds (>0) | Turn a doser on for an explicit number of seconds, e.g. `PMP_2/timer/set` with `value=20` runs it for 20s regardless of the configured default. |
| `<id>/timer/default/set` | seconds (>0) | Change the *default* dose duration used when a plain `set`/`1` doesn't specify a runtime. |
| `<id>/level/set` or `<id>/level/percent/set` | 0–100 | Manually set a tank's tracked level, as a percentage. |
| `<id>/level/remaining/set` | volume | Manually set a tank's tracked level, in that tank's own unit (gal/mL) rather than percent. |
| `<id>/reset_sensor_stats` (alias: `<id>/reset_stats`) | any | Reset a sensor's rolling average/max/min. Sending this to the master `AquachemD` ID instead of an individual sensor's ID resets by hours (`value` = hours) rather than resetting a single sensor immediately. `reset_stats` still works as a legacy alias, but `reset_sensor_stats` is the current name — use it for anything new. |
| `<id>/reset_dose_stats` | any | Reset a doser's running dose total for the current period (`running_total_ml`). Only valid on a pH/ORP/H2O doser — returns HTTP 400 on anything else. |

A failed request (unknown device, out-of-range value, or a condition rejecting the change) returns HTTP 400 with a short text reason; a successful one returns HTTP 200.

---

## MQTT API

Base topic: `<mqtt_aquachemd_topic>/` (default `aquachemd/`) — every topic below is relative to that root, e.g. `aquachemd/PH_1`.

### State — published by AquachemD

AquachemD publishes on connect, on any change, and periodically per `mqtt_timed_update`. Real examples from a running instance:

```
aquachemd/AquachemD/state 1                  # master on/off
aquachemd/Alive 1                            # heartbeat

# Binary / condition sensors — value is state (0=UNSAFE/1=SAFE)
aquachemd/CS_1/state 1                       # Flow Cell Level Sensor
aquachemd/CS_4/state 0                       # e.g. Pool Cleaner tripped

# Value sensors — bare topic is the reading, /state is validity, /average is the rolling average (if configured)
aquachemd/TEMP_1 28.89
aquachemd/TEMP_1/state 1
aquachemd/TEMP_1/average 28.89
aquachemd/PH_1 7.60
aquachemd/PH_1/average 7.61
aquachemd/ORP_1 717.70
aquachemd/ORP_1/average 719.71

# A sensor that isn't configured/present reports 127 for both:
aquachemd/TEMP_7/state 127

# Pressure and system (self-monitoring) sensors follow the same pattern
aquachemd/PRS_1 7.47
aquachemd/SYS_1 55.50                        # CPU temp
aquachemd/SYS_2 1.75                         # CPU load %

# Generic external MQTT-sourced sensor (e.g. SWG % relayed from AqualinkD)
aquachemd/MQT_1 35.00

# Dosers — /state is the mode (0/1/2/3, see Conventions above)
aquachemd/PMP_1/state 2
aquachemd/PMP_1/timer/state 0                # is a dose currently running
aquachemd/PMP_1/timer/duration 0             # seconds remaining in the current dose
aquachemd/PMP_1/timer/default 2              # configured default dose duration
aquachemd/PMP_1/running_dose_ml 28.00        # running total for the period
aquachemd/PMP_1/last_dose_ml 2.00            # volume of the most recent single dose

# A dose in progress on a different doser
aquachemd/PMP_2/timer/duration 597
aquachemd/PMP_2/timer/default 600
aquachemd/PMP_2/timer/state 1
aquachemd/PMP_2/state 1
aquachemd/PMP_2/total_water_ml 9000.00

# Tank/level sensors — bare topic is percent full; /level/remaining and /level/volume give the same figures in the tank's own unit
aquachemd/TNK_1 91.96
aquachemd/TNK_1/level/remaining 3.22
aquachemd/TNK_1/level/volume 3.50
```

The running-total topic name reflects the doser's configured type — `total_acid_ml` for a pH doser, `total_water_ml` for an H2O/topup doser, and presumably `total_chlorine_ml` for an ORP doser (not confirmed against a live example at time of writing — worth double-checking against an actual ORP doser install).

### Commands — published to AquachemD

Every command in the HTTP table above has a direct MQTT equivalent: publish the same path (with the base topic prepended) as the topic, and the value as the payload — no query string or request body, since MQTT has no such concept.

```
aquachemd/PMP_2/set 1                # Turn pump 2 on, using its configured default duration
aquachemd/PMP_2/timer/set 20         # Turn pump 2 on for exactly 20 seconds
aquachemd/PMP_2/timer/default/set 600  # Change pump 2's default dose duration to 600s
aquachemd/TNK_1/level/set 50         # Set tank 1's tracked level to 50%
aquachemd/TNK_1/level/remaining/set 2.5  # Set tank 1's tracked level to 2.5 (gal/mL, per its configured unit)
aquachemd/PH_1/reset_sensor_stats 1  # Reset PH_1's rolling average/max/min (value is ignored; reset_stats also still works)
aquachemd/PMP_1/reset_dose_stats 1   # Reset PMP_1's running dose total for the current period (value is ignored)
aquachemd/PMP_3/set 1                # Plain GPIO switch -- same /set action as a doser, just no timer/role behind it
```

Non-numeric payloads `on`, `heat`, and `cool` are also accepted as synonyms for `1`, for compatibility with Home Assistant's own MQTT switch/climate conventions.

Only topics ending in a recognized command suffix (`set`, `reset_sensor_stats`/`reset_stats`, `reset_dose_stats`) are actioned — everything else received on the AquachemD topic tree is a state publish and is ignored if sent inbound.

---

## Related

- [homebridge-aquadaemon](https://github.com/aqualinkd/homebridge-aquadaemon) — Homebridge plugin consuming this same MQTT/HTTP interface to bridge AquachemD into Apple HomeKit.
- [AqualinkD](https://github.com/aqualinkd/aqualinkd) — AquachemD's MQTT condition interlocks are designed to read AqualinkD's published state directly (e.g. gating dosing on filter pump status).