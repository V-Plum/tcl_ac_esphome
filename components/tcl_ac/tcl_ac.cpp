#include "tcl_ac.h"
#include "esphome/core/log.h"

#include <cmath>
#include <cstring>

namespace esphome {
namespace tcl_ac {

static const char *const TAG = "tcl_ac";

// ============================================================
// Protocol constants — TCL Ocarina / 38-byte frame variant
// Verified against: sanyadez/tcl-ac-ukrainian, xaxexa/ESPHome-TCLAC
// ============================================================
namespace {

// Poll frame: asks the indoor unit to send its current status
static const uint8_t POLL_[] = {0xBB, 0x00, 0x01, 0x04, 0x02, 0x01, 0x00, 0xBD};

// ---- TX byte 7  (MSB→LSB: eco | display | beep | on_tmr_en | off_tmr_en | power | 0 | 0) ----
constexpr uint8_t TX7_ECO     = 0b10000000;  // bit7 — ECO preset
constexpr uint8_t TX7_DISPLAY = 0b01000000;  // bit6 — panel display on/off
constexpr uint8_t TX7_BEEP    = 0b00100000;  // bit5 — beeper on/off
constexpr uint8_t TX7_POWER   = 0b00000100;  // bit2 — power on

// ---- TX byte 8  (MSB→LSB: quiet | diffuse/turbo | ? | comfort | mode[3:0]) ----
constexpr uint8_t TX8_QUIET   = 0b10000000;  // bit7 — quiet/mute fan
constexpr uint8_t TX8_DIFFUSE = 0b01000000;  // bit6 — diffuse fan (remote labels this "Turbo")
constexpr uint8_t TX8_COMFORT = 0b00010000;  // bit4 — COMFORT preset (health mode)

constexpr uint8_t TX8_MODE_AUTO = 0b00001000;
constexpr uint8_t TX8_MODE_COOL = 0b00000011;
constexpr uint8_t TX8_MODE_DRY  = 0b00000010;
constexpr uint8_t TX8_MODE_FAN  = 0b00000111;
constexpr uint8_t TX8_MODE_HEAT = 0b00000001;

// ---- TX byte 10  (bit6: gentle wind, bits 5-3: vertical swing, bits 2-0: fan speed) ----
constexpr uint8_t TX10_GENTLE_WIND = 0b01000000;  // bit6 — Gentle Wind mode
constexpr uint8_t TX10_SWING_V     = 0b00111000;  // bits 3-5: vertical swing active
constexpr uint8_t TX10_FAN_AUTO   = 0b000;
constexpr uint8_t TX10_FAN_LOW    = 0b001;
constexpr uint8_t TX10_FAN_MEDIUM = 0b011;
constexpr uint8_t TX10_FAN_FOCUS  = 0b101;
constexpr uint8_t TX10_FAN_MIDDLE = 0b110;
constexpr uint8_t TX10_FAN_HIGH   = 0b111;

// ---- TX byte 11  (bit3: horizontal swing) ----
constexpr uint8_t TX11_SWING_H = 0b00001000;

// ---- TX byte 19  (bit0: SLEEP preset) ----
constexpr uint8_t TX19_SLEEP = 0b00000001;


}  // namespace

// ============================================================
// Helpers
// ============================================================

uint8_t TCLACClimate::checksum_xor_(const uint8_t *data, size_t len) {
  uint8_t crc = 0;
  for (size_t i = 0; i < len; i++) crc ^= data[i];
  return crc;
}

void TCLACClimate::reset_rx_() {
  rx_pos_   = 0;
  rx_total_ = 0;
}

void TCLACClimate::arm_resend_(uint32_t now, int count) {
  control_resend_left_     = count;
  next_control_send_ms_    = now + 120;
  suppress_poll_until_ms_  = now + 900;
  last_poll_ms_            = now;
}

// ============================================================
// TX frame builder — single source of truth for every outgoing control frame
// ============================================================
void TCLACClimate::build_control_frame_() {
  memset(this->tx_, 0, sizeof(this->tx_));

  // Fixed header — sanyadez confirms: BB 00 01 03 20 03 01
  this->tx_[0] = 0xBB;
  this->tx_[1] = 0x00;
  this->tx_[2] = 0x01;
  this->tx_[3] = 0x03;   // control command type
  this->tx_[4] = 0x20;   // payload length = 32
  this->tx_[5] = 0x03;
  this->tx_[6] = 0x01;
  this->tx_[13] = 0x01;  // required constant (present in all reference implementations)
  // tx_[12] = 0x00  →  Celsius (already zeroed by memset)

  // ---- Byte 7: power, display, beeper, ECO ----
  if (this->mode != climate::CLIMATE_MODE_OFF) this->tx_[7] |= TX7_POWER;
  if (this->display_on_)     this->tx_[7] |= TX7_DISPLAY;
  if (this->beeper_on_)      this->tx_[7] |= TX7_BEEP;
  if (this->gentle_wind_on_) this->tx_[10] |= TX10_GENTLE_WIND;

  const auto pr = this->preset.has_value() ? this->preset.value() : climate::CLIMATE_PRESET_NONE;
  if (pr == climate::CLIMATE_PRESET_ECO) this->tx_[7] |= TX7_ECO;

  // ---- Byte 8: mode (bits 3:0), fan specials (bits 6-7), COMFORT (bit 4) ----
  if (pr == climate::CLIMATE_PRESET_COMFORT) this->tx_[8] |= TX8_COMFORT;

  switch (this->mode) {
    case climate::CLIMATE_MODE_OFF:                                            break;
    case climate::CLIMATE_MODE_AUTO:
    case climate::CLIMATE_MODE_HEAT_COOL: this->tx_[8] |= TX8_MODE_AUTO;     break;
    case climate::CLIMATE_MODE_COOL:      this->tx_[8] |= TX8_MODE_COOL;     break;
    case climate::CLIMATE_MODE_DRY:       this->tx_[8] |= TX8_MODE_DRY;      break;
    case climate::CLIMATE_MODE_FAN_ONLY:  this->tx_[8] |= TX8_MODE_FAN;      break;
    case climate::CLIMATE_MODE_HEAT:      this->tx_[8] |= TX8_MODE_HEAT;     break;
    default:                              this->tx_[8] |= TX8_MODE_AUTO;     break;
  }

  // ---- Bytes 8 + 10: fan speed ----
  const auto fan = this->fan_mode.has_value() ? this->fan_mode.value() : climate::CLIMATE_FAN_AUTO;
  switch (fan) {
    case climate::CLIMATE_FAN_QUIET:   this->tx_[8]  |= TX8_QUIET;              break;
    case climate::CLIMATE_FAN_DIFFUSE: this->tx_[8]  |= TX8_DIFFUSE;            break;
    case climate::CLIMATE_FAN_LOW:     this->tx_[10] |= TX10_FAN_LOW;           break;
    case climate::CLIMATE_FAN_MEDIUM:  this->tx_[10] |= TX10_FAN_MEDIUM;        break;
    case climate::CLIMATE_FAN_MIDDLE:  this->tx_[10] |= TX10_FAN_MIDDLE;        break;
    case climate::CLIMATE_FAN_HIGH:    this->tx_[10] |= TX10_FAN_HIGH;          break;
    case climate::CLIMATE_FAN_FOCUS:   this->tx_[10] |= TX10_FAN_FOCUS;         break;
    default: /* AUTO = 0, already zeroed */                                       break;
  }

  // ---- TX[10] bits 5-3 / TX[32]: vertical louver ----
  // ---- TX[11] bit 3   / TX[33]: horizontal louver ----
  // Index 0 = last/off; 1-3 = swing modes; 4-8 = fix positions (vertical)
  //                      1-4 = swing modes; 5-9 = fix positions (horizontal)
  switch (this->v_louver_) {
    case 1: this->tx_[10] |= TX10_SWING_V; this->tx_[32] |= 0x08; break;  // swing full
    case 2: this->tx_[10] |= TX10_SWING_V; this->tx_[32] |= 0x10; break;  // swing upper
    case 3: this->tx_[10] |= TX10_SWING_V; this->tx_[32] |= 0x18; break;  // swing lower
    case 4: this->tx_[32] |= 0x01; break;  // fix full up
    case 5: this->tx_[32] |= 0x02; break;  // fix upper
    case 6: this->tx_[32] |= 0x03; break;  // fix center
    case 7: this->tx_[32] |= 0x04; break;  // fix lower
    case 8: this->tx_[32] |= 0x05; break;  // fix full down
    default: break;
  }
  switch (this->h_louver_) {
    case 1: this->tx_[11] |= TX11_SWING_H; this->tx_[33] |= 0x08; break;  // swing full
    case 2: this->tx_[11] |= TX11_SWING_H; this->tx_[33] |= 0x10; break;  // swing left
    case 3: this->tx_[11] |= TX11_SWING_H; this->tx_[33] |= 0x18; break;  // swing center
    case 4: this->tx_[11] |= TX11_SWING_H; this->tx_[33] |= 0x20; break;  // swing right
    case 5: this->tx_[33] |= 0x01; break;  // fix full left
    case 6: this->tx_[33] |= 0x02; break;  // fix left
    case 7: this->tx_[33] |= 0x03; break;  // fix center
    case 8: this->tx_[33] |= 0x04; break;  // fix right
    case 9: this->tx_[33] |= 0x05; break;  // fix full right
    default: break;
  }

  // ---- Byte 9: target temperature encoded as (31 - °C) ----
  float tgt = this->target_temperature;
  if (isnan(tgt)) tgt = 24.0f;
  tgt = std::max(16.0f, std::min(30.0f, tgt));
  this->tx_[9] = static_cast<uint8_t>(31 - static_cast<int>(tgt + 0.5f));

  // ---- Byte 19: SLEEP preset ----
  if (pr == climate::CLIMATE_PRESET_SLEEP) this->tx_[19] |= TX19_SLEEP;

  // ---- Checksum: XOR of all bytes except the last ----
  this->tx_[37] = checksum_xor_(this->tx_, sizeof(this->tx_) - 1);
}

void TCLACClimate::send_control_frame_() {
  this->build_control_frame_();
  this->write_array(this->tx_, sizeof(this->tx_));
  ESP_LOGD(TAG, "TX CTRL mode=%d fan=%d preset=%d v=%d h=%d tgt=%.1f",
           (int) this->mode,
           this->fan_mode.has_value() ? (int) this->fan_mode.value() : -1,
           this->preset.has_value() ? (int) this->preset.value() : -1,
           this->v_louver_, this->h_louver_,
           this->target_temperature);
}

// ============================================================
// Component lifecycle
// ============================================================

void TCLACClimate::setup() {
  this->mode         = climate::CLIMATE_MODE_OFF;
  this->fan_mode     = climate::CLIMATE_FAN_AUTO;
  this->target_temperature = 24.0f;
  this->current_temperature = NAN;
  this->publish_state();
  this->publish_aux_();
  reset_rx_();
  ESP_LOGI(TAG, "TCL Ocarina AC ready");
}

void TCLACClimate::dump_config() {
  ESP_LOGCONFIG(TAG, "TCL Ocarina AC:");
  ESP_LOGCONFIG(TAG, "  UART: 9600 8E1 (ESP-01: TX=GPIO1 RX=GPIO3)");
  ESP_LOGCONFIG(TAG, "  Display: %s  Beeper: %s",
                this->display_on_ ? "on" : "off",
                this->beeper_on_  ? "on" : "off");
}

// ============================================================
// Aux switch entities (display + beeper)
// ============================================================

void TCLACSwitch::write_state(bool state) {
  if (this->parent_ == nullptr) { this->publish_state(state); return; }
  if (this->type_ == TCLACSwitchType::DISPLAY_ON)
    this->parent_->set_display_on(state);
  else if (this->type_ == TCLACSwitchType::BEEPER_ON)
    this->parent_->set_beeper_on(state);
  else
    this->parent_->set_gentle_wind_on(state);
}

void TCLACClimate::set_gentle_wind_on(bool on) {
  if (this->gentle_wind_on_ == on) { if (this->gentle_wind_switch_) this->gentle_wind_switch_->publish_state(on); return; }
  const uint32_t now = millis();
  this->gentle_wind_on_      = on;
  this->pending_gw_          = true;
  this->pending_gw_value_    = on;
  this->pending_gw_until_ms_ = now + 1500;
  this->send_control_frame_();
  this->arm_resend_(now, 2);
  if (this->gentle_wind_switch_) this->gentle_wind_switch_->publish_state(on);
}

void TCLACClimate::publish_aux_() {
  if (this->display_switch_) this->display_switch_->publish_state(this->display_on_);
  if (this->beeper_switch_)  this->beeper_switch_->publish_state(this->beeper_on_);
}

void TCLACClimate::set_display_on(bool on) {
  if (this->display_on_ == on) { this->publish_aux_(); return; }
  this->display_on_ = on;
  const uint32_t now = millis();
  this->send_control_frame_();
  this->arm_resend_(now, 2);
  this->publish_aux_();
}

void TCLACClimate::set_beeper_on(bool on) {
  if (this->beeper_on_ == on) { this->publish_aux_(); return; }
  this->beeper_on_ = on;
  const uint32_t now = millis();
  this->send_control_frame_();
  this->arm_resend_(now, 2);
  this->publish_aux_();
}

// ============================================================
// ClimateTraits — advertise capabilities to Home Assistant
// ============================================================

climate::ClimateTraits TCLACClimate::traits() {
  climate::ClimateTraits t;

  t.set_supported_modes({
      climate::CLIMATE_MODE_OFF,
      climate::CLIMATE_MODE_HEAT_COOL,
      climate::CLIMATE_MODE_COOL,
      climate::CLIMATE_MODE_DRY,
      climate::CLIMATE_MODE_FAN_ONLY,
      climate::CLIMATE_MODE_HEAT,
  });

  t.set_supported_fan_modes({
      climate::CLIMATE_FAN_AUTO,
      climate::CLIMATE_FAN_LOW,
      climate::CLIMATE_FAN_MEDIUM,
      climate::CLIMATE_FAN_HIGH,
      climate::CLIMATE_FAN_MIDDLE,
      climate::CLIMATE_FAN_FOCUS,
      climate::CLIMATE_FAN_QUIET,
      climate::CLIMATE_FAN_DIFFUSE,
  });

  t.set_supported_presets({
      climate::CLIMATE_PRESET_NONE,
      climate::CLIMATE_PRESET_ECO,
      climate::CLIMATE_PRESET_SLEEP,
      climate::CLIMATE_PRESET_COMFORT,
  });

  t.add_feature_flags(climate::CLIMATE_SUPPORTS_CURRENT_TEMPERATURE);
  t.clear_feature_flags(climate::CLIMATE_REQUIRES_TWO_POINT_TARGET_TEMPERATURE |
                        climate::CLIMATE_SUPPORTS_TWO_POINT_TARGET_TEMPERATURE);
  t.set_visual_min_temperature(16);
  t.set_visual_max_temperature(30);
  t.set_visual_temperature_step(1.0f);

  return t;
}

// ============================================================
// RX frame parser
// ============================================================

void TCLACClimate::handle_frame_(const uint8_t *d, size_t len) {
  if (len < 8 || d[0] != 0xBB) return;

  if (checksum_xor_(d, len - 1) != d[len - 1]) {
    ESP_LOGD(TAG, "RX bad CRC len=%u", (unsigned) len);
    return;
  }

  // Snapshot state before parsing so we can skip publish_state() if nothing changed
  const climate::ClimateMode              prev_mode    = this->mode;
  const optional<climate::ClimateFanMode> prev_fan     = this->fan_mode;
  const float                             prev_target  = this->target_temperature;
  const float                             prev_current = this->current_temperature;

  // Temperatures — bytes 17-18: raw sensor value; byte 8 low nibble: target
  if (len > 19) {
    this->target_temperature = float((d[8] & 0x0F) + 16);
    const uint16_t raw = (uint16_t(d[17]) << 8) | uint16_t(d[18]);
    // Formula confirmed by xaxexa and sanyadez: raw ADC → Fahrenheit → Celsius
    this->current_temperature = ((raw / 374.0f) - 32.0f) / 1.8f;
  }

  // Power / mode (byte 7)
  constexpr uint8_t RX_MODE_POS  = 7;
  constexpr uint8_t RX_POWER_BIT = (1 << 4);
  constexpr uint8_t RX_MODE_MASK = 0x3F;
  constexpr uint8_t RX_MODE_AUTO = 0x35;
  constexpr uint8_t RX_MODE_COOL = 0x31;
  constexpr uint8_t RX_MODE_DRY  = 0x33;
  constexpr uint8_t RX_MODE_FAN  = 0x32;
  constexpr uint8_t RX_MODE_HEAT = 0x34;

  // Fan speed (byte 8 upper nibble)
  constexpr uint8_t RX_FAN_POS    = 8;
  constexpr uint8_t RX_FAN_MASK   = 0xF0;
  constexpr uint8_t RX_FAN_AUTO   = 0x80;
  constexpr uint8_t RX_FAN_LOW    = 0x90;
  constexpr uint8_t RX_FAN_MEDIUM = 0xA0;
  constexpr uint8_t RX_FAN_MIDDLE = 0xC0;
  constexpr uint8_t RX_FAN_HIGH   = 0xD0;
  constexpr uint8_t RX_FAN_FOCUS  = 0xB0;

  // Special fan flags
  constexpr uint8_t RX_QUIET_POS   = 33;
  constexpr uint8_t RX_QUIET_BIT   = 0x80;
  constexpr uint8_t RX_DIFFUSE_BIT = 0x80;  // bit7 of RX_MODE_POS byte

  const bool power_on = (d[RX_MODE_POS] & RX_POWER_BIT) != 0;

  if (!power_on) {
    this->mode = climate::CLIMATE_MODE_OFF;
  } else {
    switch ((d[RX_MODE_POS] & RX_MODE_MASK) | 0x20) {  // bit5 clears when display off; force it for mode lookup
      case RX_MODE_AUTO: this->mode = climate::CLIMATE_MODE_HEAT_COOL; break;
      case RX_MODE_COOL: this->mode = climate::CLIMATE_MODE_COOL;      break;
      case RX_MODE_DRY:  this->mode = climate::CLIMATE_MODE_DRY;       break;
      case RX_MODE_FAN:  this->mode = climate::CLIMATE_MODE_FAN_ONLY;  break;
      case RX_MODE_HEAT: this->mode = climate::CLIMATE_MODE_HEAT;      break;
      default:           this->mode = climate::CLIMATE_MODE_HEAT_COOL; break;
    }

    if (len > RX_QUIET_POS && (d[RX_QUIET_POS] & RX_QUIET_BIT)) {
      this->fan_mode = climate::CLIMATE_FAN_QUIET;
    } else if (d[RX_MODE_POS] & RX_DIFFUSE_BIT) {
      this->fan_mode = climate::CLIMATE_FAN_DIFFUSE;
    } else {
      switch (d[RX_FAN_POS] & RX_FAN_MASK) {
        case RX_FAN_AUTO:   this->fan_mode = climate::CLIMATE_FAN_AUTO;   break;
        case RX_FAN_LOW:    this->fan_mode = climate::CLIMATE_FAN_LOW;    break;
        case RX_FAN_MEDIUM: this->fan_mode = climate::CLIMATE_FAN_MEDIUM; break;
        case RX_FAN_MIDDLE: this->fan_mode = climate::CLIMATE_FAN_MIDDLE; break;
        case RX_FAN_FOCUS:  this->fan_mode = climate::CLIMATE_FAN_FOCUS;  break;
        case RX_FAN_HIGH:   this->fan_mode = climate::CLIMATE_FAN_HIGH;   break;
        default:            this->fan_mode = climate::CLIMATE_FAN_AUTO;   break;
      }
    }
  }

  // Anti-bounce: if we sent a command and the AC hasn't confirmed it yet,
  // restore the desired values rather than letting the stale RX echo flip the HA UI.
  const uint32_t now = millis();
  if (pending_mask_ != 0) {
    const bool expired = (int32_t)(now - pending_until_ms_) >= 0;
    bool confirmed = true;
    if (pending_mask_ & PEND_MODE_) confirmed &= (this->mode == pending_mode_);
    if (pending_mask_ & PEND_FAN_)  confirmed &= (this->fan_mode.has_value() &&
                                                   this->fan_mode.value() == pending_fan_);
    if (pending_mask_ & PEND_TEMP_) confirmed &= (std::fabs(this->target_temperature - pending_target_) < 0.1f);

    if (!expired && !confirmed) {
      if (pending_mask_ & PEND_MODE_) this->mode              = pending_mode_;
      if (pending_mask_ & PEND_FAN_)  this->fan_mode          = pending_fan_;
      if (pending_mask_ & PEND_TEMP_) this->target_temperature = pending_target_;
    } else {
      pending_mask_ = 0;
    }
  }

  const bool fan_changed =
      this->fan_mode.has_value() != prev_fan.has_value() ||
      (this->fan_mode.has_value() && this->fan_mode.value() != prev_fan.value());

  const bool changed =
      this->mode != prev_mode                                              ||
      fan_changed                                                          ||
      std::fabs(this->target_temperature  - prev_target)  > 0.09f         ||
      std::fabs(this->current_temperature - prev_current) > 0.09f;

  if (changed) this->publish_state();

  ESP_LOGD(TAG, "RX OK len=%u pwr=%d mode=%d fan=%d curr=%.1f tgt=%.1f",
           (unsigned) len, (int) power_on,
           (int) this->mode,
           this->fan_mode.has_value() ? (int) this->fan_mode.value() : -1,
           this->current_temperature,
           this->target_temperature);
  // Gentle Wind: RX[50] bit5 (0x20) — confirmed by remote toggle capture
  if (len > 50) {
    const bool gw = (d[50] & 0x20) != 0;
    if (this->pending_gw_ && (int32_t)(now - this->pending_gw_until_ms_) < 0) {
      if (gw == this->pending_gw_value_)
        this->pending_gw_ = false;                            // AC confirmed our command
      else
        this->gentle_wind_on_ = this->pending_gw_value_;     // hold commanded state, don't flip resend
    } else {
      this->pending_gw_ = false;
      if (gw != this->gentle_wind_on_) {
        this->gentle_wind_on_ = gw;
        if (this->gentle_wind_switch_) this->gentle_wind_switch_->publish_state(gw);
      }
    }
  }

  // Louver state: RX[51]=vertical, RX[52]=horizontal — same bit encoding as TX[32]/TX[33]
  if (len > 52) {
    uint8_t new_v;
    switch (d[51]) {
      case 0x01: new_v = 4; break;  case 0x02: new_v = 5; break;
      case 0x03: new_v = 6; break;  case 0x04: new_v = 7; break;
      case 0x05: new_v = 8; break;
      case 0x08: new_v = 1; break;  case 0x10: new_v = 2; break;
      case 0x18: new_v = 3; break;
      default:   new_v = 0; break;
    }
    uint8_t new_h;
    switch (d[52]) {
      case 0x01: new_h = 5; break;  case 0x02: new_h = 6; break;
      case 0x03: new_h = 7; break;  case 0x04: new_h = 8; break;
      case 0x05: new_h = 9; break;
      case 0x08: new_h = 1; break;  case 0x10: new_h = 2; break;
      case 0x18: new_h = 3; break;  case 0x20: new_h = 4; break;
      default:   new_h = 0; break;
    }
    if (new_v != this->v_louver_) {
      this->v_louver_ = new_v;
      if (this->v_louver_select_) {
        const auto &opts = this->v_louver_select_->traits.get_options();
        if (new_v < opts.size()) this->v_louver_select_->publish_state(opts[new_v]);
      }
    }
    if (new_h != this->h_louver_) {
      this->h_louver_ = new_h;
      if (this->h_louver_select_) {
        const auto &opts = this->h_louver_select_->traits.get_options();
        if (new_h < opts.size()) this->h_louver_select_->publish_state(opts[new_h]);
      }
    }
  }
}

// ============================================================
// Main loop — polling + non-blocking resend
// ============================================================

void TCLACClimate::loop() {
  const uint32_t now = millis();

  // Non-blocking resend: send additional copies of the last control frame
  if (control_resend_left_ > 0 && now >= next_control_send_ms_) {
    this->send_control_frame_();
    control_resend_left_--;
    next_control_send_ms_ = now + 150;
  }

  // Status poll: every 1 second, but not while the AC is processing a control command
  if (now - last_poll_ms_ >= 1000 && now >= suppress_poll_until_ms_) {
    last_poll_ms_ = now;
    this->write_array(POLL_, sizeof(POLL_));
    ESP_LOGD(TAG, "TX POLL");
  }

  // RX: frame-sync on 0xBB, read payload length from byte 4, validate on completion
  while (this->available()) {
    uint8_t b;
    if (!this->read_byte(&b)) break;

    if (rx_pos_ == 0) {
      if (b != 0xBB) continue;
      rx_[rx_pos_++] = b;
      continue;
    }

    rx_[rx_pos_++] = b;

    if (rx_pos_ == 5) {
      rx_total_ = size_t(5) + size_t(rx_[4]) + 1;
      if (rx_total_ > sizeof(rx_)) { reset_rx_(); continue; }
    }

    if (rx_total_ && rx_pos_ >= rx_total_) {
      handle_frame_(rx_, rx_total_);
      reset_rx_();
      continue;
    }

    if (rx_pos_ >= sizeof(rx_)) reset_rx_();
  }
}

// ============================================================
// Control — handles ClimateCall from Home Assistant
// ============================================================

void TCLACClimate::control(const climate::ClimateCall &call) {
  const uint32_t now = millis();

  const bool has_any =
      call.get_mode().has_value()                    ||
      call.get_fan_mode().has_value()                ||
      call.get_target_temperature().has_value()      ||
      call.get_target_temperature_low().has_value()  ||
      call.get_target_temperature_high().has_value() ||
      call.get_preset().has_value();

  if (!has_any) return;

  uint8_t new_pending = 0;

  if (call.get_mode().has_value()) {
    const auto m = *call.get_mode();
    if (this->mode != m) {
      this->mode          = m;
      this->pending_mode_ = m;
      new_pending |= PEND_MODE_;
    }
  }

  if (call.get_fan_mode().has_value()) {
    const auto f = *call.get_fan_mode();
    if (!this->fan_mode.has_value() || this->fan_mode.value() != f) {
      this->fan_mode      = f;
      this->pending_fan_  = f;
      new_pending |= PEND_FAN_;
    }
  }

  // HA may send a single target or (in AUTO) a low/high pair — handle both
  if (call.get_target_temperature().has_value()) {
    const float t = *call.get_target_temperature();
    if (std::fabs(this->target_temperature - t) > 0.1f) {
      this->target_temperature = t;
      this->pending_target_    = t;
      new_pending |= PEND_TEMP_;
    }
  } else {
    const bool has_lo = call.get_target_temperature_low().has_value();
    const bool has_hi = call.get_target_temperature_high().has_value();
    if (has_lo || has_hi) {
      const float lo = has_lo ? *call.get_target_temperature_low()  : *call.get_target_temperature_high();
      const float hi = has_hi ? *call.get_target_temperature_high() : *call.get_target_temperature_low();
      const float t  = (lo + hi) * 0.5f;
      if (std::fabs(this->target_temperature - t) > 0.1f) {
        this->target_temperature = t;
        this->pending_target_    = t;
        new_pending |= PEND_TEMP_;
      }
    }
  }

  if (call.get_preset().has_value()) this->preset = *call.get_preset();

  if (new_pending) {
    this->pending_mask_     = new_pending;
    this->pending_until_ms_ = now + 1500;
  }

  this->send_control_frame_();
  // Fan changes are least reliable on UART; give them an extra retry
  this->arm_resend_(now, (new_pending & PEND_FAN_) ? 3 : 2);
  this->publish_state();
}

// ============================================================
// Louver select entities
// ============================================================

void TCLACSelect::control(const std::string &value) {
  auto idx = this->index_of(value);
  if (!idx.has_value()) return;
  if (this->type_ == TCLACSelectType::LOUVER_V)
    this->parent_->set_vertical_louver(static_cast<uint8_t>(idx.value()));
  else
    this->parent_->set_horizontal_louver(static_cast<uint8_t>(idx.value()));
  this->publish_state(value);
}

void TCLACClimate::set_vertical_louver(uint8_t idx) {
  if (this->v_louver_ == idx) return;
  this->v_louver_ = idx;
  const uint32_t now = millis();
  this->send_control_frame_();
  this->arm_resend_(now, 2);
}

void TCLACClimate::set_horizontal_louver(uint8_t idx) {
  if (this->h_louver_ == idx) return;
  this->h_louver_ = idx;
  const uint32_t now = millis();
  this->send_control_frame_();
  this->arm_resend_(now, 2);
}

}  // namespace tcl_ac
}  // namespace esphome
