# TCL Ocarina AC — ESPHome External Component

An [ESPHome](https://esphome.io) external component for TCL Ocarina series air conditioners. Communicates via the AC indoor unit's internal UART port using the reverse-engineered 38-byte control / 61-byte status protocol.

## Supported hardware

- AC: TCL Ocarina series (confirmed). Other TCL models using the same UART protocol may also work.
- MCU: ESP-01 (ESP8266 with 1 MB flash). The UART is wired to the hardware serial pins (TX=GPIO1, RX=GPIO3), so the serial logger must be disabled.

## Features

| Entity | Type | Description |
|--------|------|-------------|
| AC | Climate | Mode, fan speed, target temperature (16–30 °C), presets |
| Display | Switch | Panel LED on/off |
| Beeper | Switch | Acknowledgement beep on/off |
| Gentle Wind | Switch | Gentle Wind mode (perforated louver, dispersed airflow) |
| Vertical Louver | Select | 3 swing ranges + 5 fixed positions + off |
| Horizontal Louver | Select | 4 swing ranges + 5 fixed positions + off |

**Climate modes:** Off, Cool, Heat, Dry, Fan Only, Heat/Cool (Auto)

**Fan speeds:** Auto, Low, Medium, Middle, High, Focus, Quiet, Diffuse

**Presets:** None, ECO, Sleep, Comfort

All entities update from the AC status in real time, so changes made from the physical remote are reflected in Home Assistant within ~1 second.

## Installation

Add to your ESPHome YAML:

```yaml
external_components:
  - source: github://V-Plum/tcl_ac_esphome
    components: [tcl_ac]
```

## Configuration

See [`tcl_ac_esp01.yaml`](tcl_ac_esp01.yaml) for a complete working example. Minimal setup:

```yaml
esphome:
  name: tcl-ac

esp8266:
  board: esp01_1m

logger:
  baud_rate: 0  # must be disabled — GPIO1/GPIO3 used by AC UART

uart:
  id: ac_uart
  tx_pin: GPIO1
  rx_pin: GPIO3
  baud_rate: 9600
  data_bits: 8
  parity: EVEN
  stop_bits: 1

external_components:
  - source: github://V-Plum/tcl_ac_esphome
    components: [tcl_ac]

climate:
  - platform: tcl_ac
    name: "AC"
    id: ac
    uart_id: ac_uart

switch:
  - platform: tcl_ac
    name: "AC Display"
    tcl_ac_id: ac
    type: display

  - platform: tcl_ac
    name: "AC Beeper"
    tcl_ac_id: ac
    type: beeper

  - platform: tcl_ac
    name: "AC Gentle Wind"
    tcl_ac_id: ac
    type: gentle_wind

select:
  - platform: tcl_ac
    name: "AC Vertical Louver"
    tcl_ac_id: ac
    type: vertical_louver

  - platform: tcl_ac
    name: "AC Horizontal Louver"
    tcl_ac_id: ac
    type: horizontal_louver
```

## Notes

- First-time Wi-Fi setup is done via Improv Serial or the captive portal fallback AP.
- The component polls the AC every second and sends control frames with retry logic. There is no keep-alive heartbeat — the AC holds its last state.
- Protocol reverse-engineered with contributions from the community (sanyadez, xaxexa).
