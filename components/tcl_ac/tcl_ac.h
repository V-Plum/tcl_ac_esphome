#pragma once

#include "esphome/core/component.h"
#include "esphome/components/climate/climate.h"
#include "esphome/components/switch/switch.h"
#include "esphome/components/uart/uart.h"

namespace esphome {
namespace tcl_ac {

// NOTE: Do NOT name anything "DISPLAY" — ESP8266 Arduino core defines a DISPLAY macro.

enum TCLACSwitchType : uint8_t {
  DISPLAY_ON = 0,
  BEEPER_ON  = 1,
};

class TCLACClimate;

class TCLACSwitch : public switch_::Switch, public Parented<TCLACClimate> {
 public:
  void set_type(TCLACSwitchType type) { this->type_ = type; }

 protected:
  void write_state(bool state) override;
  TCLACSwitchType type_{TCLACSwitchType::DISPLAY_ON};
};

class TCLACClimate : public climate::Climate, public uart::UARTDevice, public Component {
 public:
  // UARTDevice default ctor used; parent set via register_uart_device → set_uart_parent()

  void setup() override;
  void loop() override;
  void dump_config() override;
  climate::ClimateTraits traits() override;
  void control(const climate::ClimateCall &call) override;

  void set_display_on(bool on);
  void set_beeper_on(bool on);
  bool get_display_on() const { return this->display_on_; }
  bool get_beeper_on() const  { return this->beeper_on_;  }
  void set_display_switch(switch_::Switch *sw) { this->display_switch_ = sw; }
  void set_beeper_switch(switch_::Switch *sw)  { this->beeper_switch_  = sw; }

 protected:
  static uint8_t checksum_xor_(const uint8_t *data, size_t len);
  void reset_rx_();
  void build_control_frame_();
  void send_control_frame_();
  void handle_frame_(const uint8_t *d, size_t len);
  void publish_aux_();
  void arm_resend_(uint32_t now, int count);

  // RX buffer — 96 bytes covers any known response (max observed: 61 bytes)
  uint8_t rx_[96];
  size_t  rx_pos_{0};
  size_t  rx_total_{0};

  // TX control frame — 38 bytes confirmed by sanyadez (same protocol variant)
  uint8_t tx_[38];

  // Non-blocking resend: repeat control frame a few times for reliability over noisy UART
  int      control_resend_left_{0};
  uint32_t next_control_send_ms_{0};

  // Polling: request AC status every second, suppressed briefly after a control TX
  uint32_t last_poll_ms_{0};
  uint32_t suppress_poll_until_ms_{0};

  // Pending confirmation: hold desired state in the HA UI while the AC processes the command,
  // so RX echoing the old value does not cause a visible bounce.
  static constexpr uint8_t PEND_MODE_ = 0x01;
  static constexpr uint8_t PEND_FAN_  = 0x02;
  static constexpr uint8_t PEND_TEMP_ = 0x04;

  uint8_t  pending_mask_{0};
  uint32_t pending_until_ms_{0};

  climate::ClimateMode    pending_mode_{climate::CLIMATE_MODE_OFF};
  climate::ClimateFanMode pending_fan_{climate::CLIMATE_FAN_AUTO};
  float                   pending_target_{0.0f};

  // Display / beeper current state (exposed as independent ESPHome switch entities)
  bool display_on_{true};
  bool beeper_on_{true};
  switch_::Switch *display_switch_{nullptr};
  switch_::Switch *beeper_switch_{nullptr};
};

}  // namespace tcl_ac
}  // namespace esphome
