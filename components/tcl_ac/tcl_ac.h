#pragma once

#include "esphome/core/component.h"
#include "esphome/components/climate/climate.h"
#include "esphome/components/select/select.h"
#include "esphome/components/switch/switch.h"
#include "esphome/components/uart/uart.h"

namespace esphome {
namespace tcl_ac {

// NOTE: Do NOT name anything "DISPLAY" — ESP8266 Arduino core defines a DISPLAY macro.

enum TCLACSwitchType : uint8_t {
  DISPLAY_ON   = 0,
  BEEPER_ON    = 1,
  GENTLE_WIND  = 2,
};

enum TCLACSelectType : uint8_t {
  LOUVER_V = 0,
  LOUVER_H = 1,
};

class TCLACClimate;

class TCLACSwitch : public switch_::Switch, public Parented<TCLACClimate> {
 public:
  void set_type(TCLACSwitchType type) { this->type_ = type; }

 protected:
  void write_state(bool state) override;
  TCLACSwitchType type_{TCLACSwitchType::DISPLAY_ON};
};

// Index maps directly to option position in select.py V_OPTIONS / H_OPTIONS.
// Vertical:   0=last  1=swing_full  2=swing_upper  3=swing_lower
//             4=fix_top  5=fix_upper  6=fix_center  7=fix_lower  8=fix_bottom
// Horizontal: 0=last  1=swing_full  2=swing_left  3=swing_center  4=swing_right
//             5=fix_full_left  6=fix_left  7=fix_center  8=fix_right  9=fix_full_right
class TCLACSelect : public select::Select, public Parented<TCLACClimate> {
 public:
  void set_type(TCLACSelectType type) { this->type_ = type; }

 protected:
  void control(const std::string &value) override;
  TCLACSelectType type_{TCLACSelectType::LOUVER_V};
};

class TCLACClimate : public climate::Climate, public uart::UARTDevice, public Component {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;
  climate::ClimateTraits traits() override;
  void control(const climate::ClimateCall &call) override;

  void set_display_on(bool on);
  void set_beeper_on(bool on);
  void set_gentle_wind_on(bool on);
  bool get_display_on()     const { return this->display_on_; }
  bool get_beeper_on()      const { return this->beeper_on_;  }
  bool get_gentle_wind_on() const { return this->gentle_wind_on_; }
  void set_display_switch(switch_::Switch *sw)     { this->display_switch_     = sw; }
  void set_beeper_switch(switch_::Switch *sw)      { this->beeper_switch_      = sw; }
  void set_gentle_wind_switch(switch_::Switch *sw) { this->gentle_wind_switch_ = sw; }

  void set_vertical_louver(uint8_t idx);
  void set_horizontal_louver(uint8_t idx);
  void set_vertical_louver_select(select::Select *s)   { this->v_louver_select_ = s; }
  void set_horizontal_louver_select(select::Select *s) { this->h_louver_select_ = s; }

 protected:
  static uint8_t checksum_xor_(const uint8_t *data, size_t len);
  void reset_rx_();
  void build_control_frame_();
  void send_control_frame_();
  void handle_frame_(const uint8_t *d, size_t len);
  void publish_aux_();
  void arm_resend_(uint32_t now, int count);

  uint8_t rx_[96];
  size_t  rx_pos_{0};
  size_t  rx_total_{0};

  uint8_t tx_[38];

  int      control_resend_left_{0};
  uint32_t next_control_send_ms_{0};

  uint32_t last_poll_ms_{0};
  uint32_t suppress_poll_until_ms_{0};

  static constexpr uint8_t PEND_MODE_ = 0x01;
  static constexpr uint8_t PEND_FAN_  = 0x02;
  static constexpr uint8_t PEND_TEMP_ = 0x04;

  uint8_t  pending_mask_{0};
  uint32_t pending_until_ms_{0};

  climate::ClimateMode pending_mode_{climate::CLIMATE_MODE_OFF};
  std::string          pending_fan_{"Авто"};
  float                pending_target_{0.0f};

  bool display_on_{true};
  bool beeper_on_{true};
  bool gentle_wind_on_{false};
  bool     pending_gw_{false};
  bool     pending_gw_value_{false};
  uint32_t pending_gw_until_ms_{0};
  switch_::Switch *display_switch_{nullptr};
  switch_::Switch *beeper_switch_{nullptr};
  switch_::Switch *gentle_wind_switch_{nullptr};

  // 0xFF = unknown (not yet received from AC); 0 = off/last; see TCLACSelect comment above
  uint8_t v_louver_{0xFF};
  uint8_t h_louver_{0xFF};
  select::Select *v_louver_select_{nullptr};
  select::Select *h_louver_select_{nullptr};
};

}  // namespace tcl_ac
}  // namespace esphome
