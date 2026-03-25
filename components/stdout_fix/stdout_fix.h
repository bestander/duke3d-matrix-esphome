#pragma once
#include "esphome/core/component.h"
#include <cstdio>

namespace esphome {
namespace stdout_fix {

// Runs before Logger (priority 1500) to unbuffer stdout.
// Without this, IDF 5.x installs the USB-JTAG console driver during startup
// with full buffering, then ESPHome's logger fails to reinstall it and skips
// the setvbuf(stdout, _IONBF) call — causing silent USB-JTAG output.
class StdoutFix : public Component {
 public:
  void setup() override { setvbuf(stdout, nullptr, _IONBF, 0); }
  float get_setup_priority() const override { return 2000.0f; }
};

}  // namespace stdout_fix
}  // namespace esphome
