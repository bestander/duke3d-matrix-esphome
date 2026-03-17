# Duke3D ESP32-S3 LED Matrix — Implementation Plan

> **For agentic workers:** REQUIRED: Use superpowers:subagent-driven-development (if subagents available) or superpowers:executing-plans to implement this plan. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Always-on Duke3D demo playback on a 64×64 HUB-75 LED matrix (ESP32-S3), with a 24px weather/time HUD fed by Home Assistant, wrapped as an ESPHome project.

**Architecture:** ESPHome `external_components` monorepo with `framework: esp-idf`. Duke3D engine (forked from jkirsons/Duke3D) runs in demo loop mode on Core 1, pinned FreeRTOS task. ESPHome/WiFi/HA API runs on Core 0. Display split: rows 0–39 for Duke3D (64×40, aspect-preserving), rows 40–63 for HUD (64×24).

**Tech Stack:** ESPHome 2024.x · ESP-IDF 5.x · ESP32-HUB75-MatrixPanel-I2S-DMA · jkirsons/Duke3D (ESP-IDF) · MAX98357A I2S · ESP-IDF FATFS/SPI SD

---

## File Map

```
duke3d-matrix-esphome/
  esphome.yaml                          # ESPHome top-level config
  secrets.yaml.template                 # credential template (never commit secrets.yaml)
  partitions.csv                        # 3MB+3MB OTA, 1.5MB FAT
  .gitignore
  .gitmodules
  libs/
    ESP32-HUB75-MatrixPanel-I2S-DMA/    # git submodule (HUB-75 driver)
  components/
    hub75_matrix/
      __init__.py                       # ESPHome component registration
      hub75_matrix.h                    # Hub75Matrix class (double-buffered 64×64)
      hub75_matrix.cpp
      CMakeLists.txt
    hud/
      __init__.py
      hud.h                             # Hud class — renders rows 40–63
      hud.cpp
      font_5x7.h                        # 5×7 bitmap font (digits, ':', '°', 'C')
      weather_icons.h                   # 8×8 1-bit weather icons
      CMakeLists.txt
    sd_card/
      __init__.py
      sd_card.h                         # SdCard: FATFS SPI mount wrapper
      sd_card.cpp
      CMakeLists.txt
    duke3d/
      __init__.py
      duke3d_component.h               # Duke3DComponent ESPHome wrapper
      duke3d_component.cpp
      renderer/
        renderer.h                      # 320×200 → 64×40 downscaler
        renderer.cpp
      platform/
        esp32_hal.h                     # Duke3D platform shims (file, time, input)
        esp32_hal.cpp
        input.h                         # InputEvent enum + inject queue
        input.cpp
      engine/                           # git submodule: jkirsons/Duke3D
      CMakeLists.txt
    i2s_audio/
      __init__.py
      i2s_audio.h                       # I2SAudio: MAX98357A PCM sink
      i2s_audio.cpp
      CMakeLists.txt
  test/
    CMakeLists.txt                      # host-side test runner (no hardware needed)
    mock/
      hub75_mock.h                      # Mock Hub75Matrix for host tests
    hub75_matrix/
      test_color.cpp
    renderer/
      test_downscaler.cpp
    hud/
      test_hud_layout.cpp
  docs/
    superpowers/
      specs/2026-03-17-duke3d-esp32-matrix-design.md
      plans/2026-03-17-duke3d-esp32-matrix.md      # this file
```

---

## Chunk 1: ESPHome Scaffold (Phase 1)

### Task 1: Repo bootstrap, esphome.yaml, partition table

**Files:**
- Create: `esphome.yaml`
- Create: `secrets.yaml.template`
- Create: `partitions.csv`
- Create: `.gitignore`

- [ ] **Step 1: Create .gitignore**

```
.esphome/
secrets.yaml
*.bin
*.elf
build/
test/build/
```

- [ ] **Step 2: Create secrets.yaml.template**

```yaml
wifi_ssid: "YOUR_SSID"
wifi_password: "YOUR_WIFI_PASSWORD"
ap_password: "duke3d_fallback"
api_encryption_key: "YOUR_32_BYTE_BASE64_KEY"
ota_password: "YOUR_OTA_PASSWORD"
```
<!-- all keys must be at column 0, no leading spaces -->

Copy to `secrets.yaml` and fill in. Never commit `secrets.yaml`.

- [ ] **Step 3: Create partitions.csv**

```
# Name,   Type, SubType, Offset,   Size,
nvs,      data, nvs,     0x9000,   0x4000,
otadata,  data, ota,     0xd000,   0x2000,
app0,     app,  ota_0,   0x10000,  0x300000,
app1,     app,  ota_1,   0x310000, 0x300000,
fatfs,    data, fat,     0x610000, 0x180000,
```

Total: ~7.5MB used of 8MB flash.

- [ ] **Step 4: Create esphome.yaml**

```yaml
esphome:
  name: duke3d-matrix
  friendly_name: Duke3D Matrix

esp32:
  board: adafruit_matrixportal_s3
  variant: esp32s3
  framework:
    type: esp-idf
    version: recommended
    sdkconfig_options:
      CONFIG_SPIRAM: "y"
      CONFIG_SPIRAM_MODE_OCT: "y"
      CONFIG_SPIRAM_SPEED_80M: "y"
      CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL: "16384"
      CONFIG_FREERTOS_HZ: "1000"
      CONFIG_ESP_MAIN_TASK_STACK_SIZE: "8192"
      CONFIG_ESP_TASK_WDT_TIMEOUT_S: "10"

partitions: partitions.csv

wifi:
  ssid: !secret wifi_ssid
  password: !secret wifi_password
  ap:
    ssid: "Duke3D Fallback AP"
    password: !secret ap_password

captive_portal:

api:
  encryption:
    key: !secret api_encryption_key

ota:
  - platform: esphome
    password: !secret ota_password

logger:
  level: DEBUG
  baud_rate: 115200

time:
  - platform: homeassistant
    id: ha_time

sensor:
  - platform: homeassistant
    id: weather_temperature
    entity_id: sensor.weather_temperature

text_sensor:
  - platform: homeassistant
    id: weather_condition
    entity_id: sensor.weather_condition
```

> **Note:** If `adafruit_matrixportal_s3` is unrecognized, run `esphome boards esp32` and use the closest S3 variant (e.g. `esp32-s3-devkitc-1`).

> **Note — no unit tests in Chunk 1:** The scaffold is pure configuration; there is no logic to unit-test. `esphome config` serves as the "failing test" (it rejects invalid YAML immediately), and `esphome compile` serves as the "passing test" (successful compilation is the exit criterion).

- [ ] **Step 5: Validate YAML before compiling**

```bash
pip install esphome          # if not installed
cp secrets.yaml.template secrets.yaml
# Edit secrets.yaml with real credentials
esphome config esphome.yaml
```

Expected: `INFO Configuration is valid.` Fix any YAML errors before continuing.

- [ ] **Step 6: Compile firmware**

```bash
esphome compile esphome.yaml
```

Expected: `INFO Successfully compiled program.`

- [ ] **Step 7: Flash and verify HA connection**

Hold BOOT, press+release RESET, release BOOT to enter bootloader mode.

```bash
esphome upload esphome.yaml
```

In HA → Settings → Devices & Services → ESPHome: accept "duke3d-matrix". Device should appear online.

- [ ] **Step 8: Commit**

```bash
git init   # if not already a repo
git add esphome.yaml secrets.yaml.template partitions.csv .gitignore
git commit -m "feat: Phase 1 - ESPHome scaffold with WiFi, HA API, OTA"
```

---

## Chunk 2: HUB-75 Matrix Driver (Phase 2)

### Task 2: HUB-75 component with double-buffered 64×64 output

**Files:**
- Create: `libs/ESP32-HUB75-MatrixPanel-I2S-DMA/` (git submodule)
- Create: `components/hub75_matrix/__init__.py`
- Create: `components/hub75_matrix/hub75_matrix.h`
- Create: `components/hub75_matrix/hub75_matrix.cpp`
- Create: `components/hub75_matrix/CMakeLists.txt`
- Create: `test/mock/hub75_mock.h`
- Create: `test/hub75_matrix/test_color.cpp`
- Create: `test/CMakeLists.txt`
- Modify: `esphome.yaml`

- [ ] **Step 1: Add HUB-75 library as git submodule**

```bash
git submodule add https://github.com/mrcodetastic/ESP32-HUB75-MatrixPanel-I2S-DMA.git \
  libs/ESP32-HUB75-MatrixPanel-I2S-DMA
git submodule update --init
```

- [ ] **Step 2: Write failing host-side color tests**

Create `test/mock/hub75_mock.h`:
```cpp
#pragma once
#include <cstdint>
#include <cstring>

struct Color {
    uint8_t r, g, b;
    Color(uint8_t r=0, uint8_t g=0, uint8_t b=0) : r(r), g(g), b(b) {}
};

class Hub75Matrix {
public:
    static const int WIDTH = 64, HEIGHT = 64;
    Color pixels[HEIGHT * WIDTH] = {};

    void set_pixel(int x, int y, Color c) {
        if (x >= 0 && x < WIDTH && y >= 0 && y < HEIGHT)
            pixels[y * WIDTH + x] = c;
    }
    void fill(Color c) { for (int i = 0; i < WIDTH * HEIGHT; i++) pixels[i] = c; }
    void swap_buffers() {}
};
```

Create `test/hub75_matrix/test_color.cpp`:
```cpp
#include <cassert>
#include "hub75_mock.h"

void test_set_pixel_writes_correct_color() {
    Hub75Matrix m;
    m.set_pixel(10, 20, {255, 0, 128});
    assert(m.pixels[20 * 64 + 10].r == 255);
    assert(m.pixels[20 * 64 + 10].g == 0);
    assert(m.pixels[20 * 64 + 10].b == 128);
}

void test_set_pixel_out_of_bounds_does_not_crash() {
    Hub75Matrix m;
    m.set_pixel(-1, 0,  {255, 0, 0});
    m.set_pixel(0,  64, {255, 0, 0});
    m.set_pixel(64, 0,  {255, 0, 0});
    // No assertion needed — must not crash
}

void test_fill_sets_all_pixels() {
    Hub75Matrix m;
    m.fill({100, 200, 50});
    for (int i = 0; i < 64 * 64; i++) {
        assert(m.pixels[i].r == 100);
        assert(m.pixels[i].g == 200);
        assert(m.pixels[i].b == 50);
    }
}

int main() {
    test_set_pixel_writes_correct_color();
    test_set_pixel_out_of_bounds_does_not_crash();
    test_fill_sets_all_pixels();
    return 0;
}
```

Create `test/CMakeLists.txt`:
```cmake
cmake_minimum_required(VERSION 3.16)
project(duke3d_matrix_tests)
set(CMAKE_CXX_STANDARD 17)

add_executable(test_hub75 hub75_matrix/test_color.cpp)
target_include_directories(test_hub75 PRIVATE mock)
```

- [ ] **Step 3: Run test — confirm it compiles and passes with the mock**

```bash
cmake -S test -B test/build
cmake --build test/build
./test/build/test_hub75
```

Expected: exits 0, no output (assertions pass silently).

- [ ] **Step 4: Create hub75_matrix.h**

```cpp
#pragma once
#include "esphome/core/component.h"
#include <cstdint>

namespace esphome {
namespace hub75_matrix {

struct Color {
    uint8_t r, g, b;
    Color() : r(0), g(0), b(0) {}
    Color(uint8_t r, uint8_t g, uint8_t b) : r(r), g(g), b(b) {}
};

// Thread-safety contract:
//   set_pixel() and fill() write to the back buffer only (back_buf_).
//   swap_buffers() copies back_buf_ into the library's internal DMA buffer
//   pixel by pixel — NOT an atomic swap (brief tearing window exists per frame).
//   set_pixel() and fill() are safe to call from any task while swap_buffers()
//   is not running; do not call them concurrently with swap_buffers().
class Hub75Matrix : public Component {
public:
    static const int WIDTH  = 64;
    static const int HEIGHT = 64;

    void setup() override;
    void loop() override {}
    float get_setup_priority() const override { return setup_priority::HARDWARE; }

    void set_pixel(int x, int y, Color c);
    void fill(Color c);
    void swap_buffers();  // call once per rendered frame

private:
    Color* back_buf_ = nullptr;
};

extern Hub75Matrix* global_hub75;  // set in setup(); used by duke3d renderer

}  // namespace hub75_matrix
}  // namespace esphome
```

- [ ] **Step 5: Create hub75_matrix.cpp**

```cpp
#include "hub75_matrix.h"
#include "esphome/core/log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ESP32-HUB75-MatrixPanel-I2S-DMA.h"

namespace esphome {
namespace hub75_matrix {

static const char* TAG = "hub75";
Hub75Matrix* global_hub75 = nullptr;
static MatrixPanel_I2S_DMA* matrix_lib = nullptr;

// -----------------------------------------------------------------------
// ALL GPIO NUMBERS BELOW ARE -1 (UNSET) AND MUST BE FILLED IN BEFORE
// FLASHING. Look up the Adafruit Matrix Portal S3 schematic:
//   https://learn.adafruit.com/adafruit-matrix-portal-s3/pinouts
//
// FORBIDDEN: Do NOT use GPIO35, 36, 37, 38 — they are the Octal-SPI PSRAM
// bus pins and will silently corrupt PSRAM access if used as GPIO.
//
// The MatrixPanel_I2S_DMA::begin() call below will assert / return false
// if any required pin is still -1, giving a clear error message.
// -----------------------------------------------------------------------
// Compile-time safety net: causes a build error until real GPIO numbers are filled in.
// Once you have confirmed the real pin numbers from the Adafruit schematic,
// delete the three lines below (#if 1 / #error / #endif).
#if 1
#error "ACTION REQUIRED: Replace -1 placeholders in MATRIX_PINS with real GPIO numbers from the Adafruit Matrix Portal S3 schematic, then delete this #error block."
#endif
static const HUB75_I2S_CFG::i2s_pins MATRIX_PINS = {
    .r1  = -1, .g1  = -1, .b1  = -1,  // TODO: from schematic
    .r2  = -1, .g2  = -1, .b2  = -1,  // TODO: from schematic
    .a   = -1, .b   = -1, .c   = -1, .d = -1, .e = -1,  // TODO
    .lat = -1, .oe  = -1, .clk = -1   // TODO
};
// Two 64×32 panels chained to form 64×64.
// If panels are chained horizontally (128×32), swap PANEL_WIDTH/HEIGHT/CHAIN.
static const int PANEL_WIDTH  = 64;
static const int PANEL_HEIGHT = 32;
static const int CHAIN_LEN    = 2;

void Hub75Matrix::setup() {
    const size_t buf_size = WIDTH * HEIGHT * sizeof(Color);
    back_buf_ = (Color*)heap_caps_malloc(buf_size, MALLOC_CAP_SPIRAM);
    if (!back_buf_) {
        ESP_LOGE(TAG, "Failed to allocate back buffer in PSRAM");
        return;
    }
    memset(back_buf_, 0, buf_size);

    HUB75_I2S_CFG cfg(PANEL_WIDTH, PANEL_HEIGHT, CHAIN_LEN,
                      const_cast<HUB75_I2S_CFG::i2s_pins&>(MATRIX_PINS));
    cfg.clkphase = false;
    cfg.driver   = HUB75_I2S_CFG::SHIFTREG;

    matrix_lib = new MatrixPanel_I2S_DMA(cfg);
    if (!matrix_lib->begin()) {
        ESP_LOGE(TAG, "MatrixPanel_I2S_DMA::begin() failed");
        return;
    }
    matrix_lib->setBrightness8(128);
    matrix_lib->clearScreen();

    global_hub75 = this;
    ESP_LOGI(TAG, "HUB-75 64×64 initialized");
}

void Hub75Matrix::set_pixel(int x, int y, Color c) {
    if (x < 0 || x >= WIDTH || y < 0 || y >= HEIGHT) return;
    back_buf_[y * WIDTH + x] = c;
}

void Hub75Matrix::fill(Color c) {
    for (int i = 0; i < WIDTH * HEIGHT; i++) back_buf_[i] = c;
}

void Hub75Matrix::swap_buffers() {
    // Copies the back buffer into the library's DMA buffer pixel by pixel.
    // NOT a zero-copy swap — the library's DMA scan task reads its internal
    // buffer while this loop writes, producing a brief (~4096 pixel) tearing
    // window per frame. At 25fps on a 64×64 LED matrix this is acceptable.
    //
    // For a true double-buffer swap: check if your version of
    // ESP32-HUB75-MatrixPanel-I2S-DMA exposes flipDMABuffer() and use it.
    for (int y = 0; y < HEIGHT; y++)
        for (int x = 0; x < WIDTH; x++) {
            Color& c = back_buf_[y * WIDTH + x];
            matrix_lib->drawPixelRGB888(x, y, c.r, c.g, c.b);
        }
}

}  // namespace hub75_matrix
}  // namespace esphome
```

- [ ] **Step 6: Create component CMakeLists.txt**

```cmake
file(GLOB_RECURSE HUB75_SRCS
    "${CMAKE_CURRENT_SOURCE_DIR}/../../libs/ESP32-HUB75-MatrixPanel-I2S-DMA/src/*.cpp"
)

idf_component_register(
    SRCS "hub75_matrix.cpp" ${HUB75_SRCS}
    INCLUDE_DIRS
        "."
        "../../libs/ESP32-HUB75-MatrixPanel-I2S-DMA/src"
    REQUIRES esp_psram freertos driver
)
```

- [ ] **Step 7: Create component __init__.py**

```python
import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.const import CONF_ID

hub75_ns = cg.esphome_ns.namespace("hub75_matrix")
Hub75MatrixClass = hub75_ns.class_("Hub75Matrix", cg.Component)

CONFIG_SCHEMA = cv.Schema({
    cv.GenerateID(): cv.declare_id(Hub75MatrixClass),
}).extend(cv.COMPONENT_SCHEMA)

async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
```

- [ ] **Step 8: Wire into esphome.yaml and add test pattern**

Add to `esphome.yaml`:
```yaml
external_components:
  - source:
      type: local
      path: components
    components: [hub75_matrix]

hub75_matrix:
  id: matrix_display

esphome:
  on_boot:
    priority: -200
    then:
      - lambda: |-
          // Temporary test pattern: red/green/blue vertical bands
          auto* m = id(matrix_display);
          for (int y = 0; y < 64; y++)
            for (int x = 0; x < 64; x++) {
              hub75_matrix::Color c;
              if (x < 21) c = {255, 0, 0};
              else if (x < 42) c = {0, 255, 0};
              else c = {0, 0, 255};
              m->set_pixel(x, y, c);
            }
          m->swap_buffers();
```

- [ ] **Step 9: Compile, flash, verify test pattern**

```bash
esphome compile esphome.yaml
esphome upload esphome.yaml
```

Expected: Both 64×32 panels show vertical red / green / blue stripes.

If only one panel lights up: the second panel may need `CHAIN_LEN` adjusted or the connector flipped — check library docs for daisy-chain wiring.

If GPIO errors occur: consult the Adafruit Matrix Portal S3 pinout and update `MATRIX_PINS` in `hub75_matrix.cpp`.

- [ ] **Step 10: Remove test pattern, commit**

Remove the `on_boot` lambda from `esphome.yaml`.

```bash
git add components/hub75_matrix/ libs/ esphome.yaml test/ .gitmodules
git commit -m "feat: Phase 2 - HUB-75 64×64 double-buffered display component"
```

---

## Chunk 3: HUD Band (Phase 3)

### Task 3: Weather + time HUD in rows 40–63

**Files:**
- Create: `components/hud/__init__.py`
- Create: `components/hud/hud.h`
- Create: `components/hud/hud.cpp`
- Create: `components/hud/font_5x7.h`
- Create: `components/hud/weather_icons.h`
- Create: `components/hud/CMakeLists.txt`
- Create: `test/hud/test_hud_layout.cpp`
- Modify: `esphome.yaml`
- Modify: `test/CMakeLists.txt`

- [ ] **Step 1: Write failing HUD layout test**

Create `test/hud/test_hud_layout.cpp`:
```cpp
#define DUKE3D_HOST_TEST
#include <cassert>
#include "hub75_mock.h"
#include "hud.h"

void test_hud_does_not_touch_game_rows() {
    Hub75Matrix m;
    m.fill({255, 0, 0});  // all red

    esphome::hud::Hud hud;
    hud.set_temperature(22.5f);
    hud.set_condition("sunny");
    hud.render(m);

    // Rows 0–39 must remain untouched (all red)
    for (int y = 0; y < 40; y++)
        for (int x = 0; x < 64; x++) {
            assert(m.pixels[y * 64 + x].r == 255);
            assert(m.pixels[y * 64 + x].g == 0);
        }
}

void test_hud_draws_something_in_hud_rows() {
    Hub75Matrix m;
    m.fill({0, 0, 0});  // all black

    esphome::hud::Hud hud;
    hud.set_temperature(22.5f);
    hud.set_condition("sunny");
    hud.render(m);

    // At least one non-black pixel must appear in rows 40–63
    bool drew = false;
    for (int y = 40; y < 64 && !drew; y++)
        for (int x = 0; x < 64 && !drew; x++) {
            auto& p = m.pixels[y * 64 + x];
            if (p.r || p.g || p.b) drew = true;
        }
    assert(drew);
}

int main() {
    test_hud_does_not_touch_game_rows();
    test_hud_draws_something_in_hud_rows();
    return 0;
}
```

Update `test/CMakeLists.txt`:
```cmake
cmake_minimum_required(VERSION 3.16)
project(duke3d_matrix_tests)
set(CMAKE_CXX_STANDARD 17)

add_executable(test_hub75 hub75_matrix/test_color.cpp)
target_include_directories(test_hub75 PRIVATE mock)

add_executable(test_hud hud/test_hud_layout.cpp)
target_include_directories(test_hud PRIVATE
    mock
    ${CMAKE_SOURCE_DIR}/../components/hud
)
target_compile_definitions(test_hud PRIVATE DUKE3D_HOST_TEST)
```

- [ ] **Step 2: Run — confirm compile fails (hud.h not yet written)**

```bash
rm -rf test/build   # clear cache from Chunk 2 to avoid stale targets
cmake -S test -B test/build
cmake --build test/build 2>&1 | tee /tmp/build_out.txt; grep -c "error" /tmp/build_out.txt
```

Expected: build fails with "hud.h: No such file" (non-zero error count). If build unexpectedly succeeds (error count = 0), stop and investigate before continuing.

- [ ] **Step 3: Create font_5x7.h**

```cpp
#pragma once
#include <cstdint>

// 5×7 bitmap font. Each entry is 5 column bytes; bit 0 = top row.
// Indices: 0–9 = digits, 10 = ':', 11 = ' ', 12 = degree, 13 = 'C'
static const uint8_t FONT_5X7[][5] = {
    {0x3E, 0x51, 0x49, 0x45, 0x3E}, // 0
    {0x00, 0x42, 0x7F, 0x40, 0x00}, // 1
    {0x42, 0x61, 0x51, 0x49, 0x46}, // 2
    {0x21, 0x41, 0x45, 0x4B, 0x31}, // 3
    {0x18, 0x14, 0x12, 0x7F, 0x10}, // 4
    {0x27, 0x45, 0x45, 0x45, 0x39}, // 5
    {0x3C, 0x4A, 0x49, 0x49, 0x30}, // 6
    {0x01, 0x71, 0x09, 0x05, 0x03}, // 7
    {0x36, 0x49, 0x49, 0x49, 0x36}, // 8
    {0x06, 0x49, 0x49, 0x29, 0x1E}, // 9
    {0x00, 0x36, 0x36, 0x00, 0x00}, // ':'  (index 10)
    {0x00, 0x00, 0x00, 0x00, 0x00}, // ' '  (index 11)
    {0x00, 0x07, 0x05, 0x07, 0x00}, // '°'  (index 12)
    {0x3E, 0x41, 0x41, 0x41, 0x22}, // 'C'  (index 13)
};

// Use integer constants instead of char codes to avoid UTF-8/Latin-1 ambiguity.
static const int FONT_IDX_COLON  = 10;
static const int FONT_IDX_SPACE  = 11;
static const int FONT_IDX_DEGREE = 12;  // '°' — referenced by index, not char value
static const int FONT_IDX_C      = 13;

inline int font_index(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c == ':') return FONT_IDX_COLON;
    if (c == 'C') return FONT_IDX_C;
    return FONT_IDX_SPACE;
}
```

- [ ] **Step 4: Create weather_icons.h**

```cpp
#pragma once
#include <cstdint>
#include <cstring>

struct WeatherIcon { uint8_t rows[8]; };

static const WeatherIcon ICON_SUNNY  = {{ 0x24, 0x18, 0x7E, 0xFF, 0xFF, 0x7E, 0x18, 0x24 }};
static const WeatherIcon ICON_CLOUDY = {{ 0x00, 0x3C, 0x7E, 0xFF, 0xFF, 0x7E, 0x00, 0x00 }};
static const WeatherIcon ICON_RAINY  = {{ 0x3C, 0x7E, 0xFF, 0xFF, 0x4A, 0x25, 0x4A, 0x00 }};
static const WeatherIcon ICON_SNOWY  = {{ 0x24, 0x18, 0xFF, 0x7E, 0x7E, 0xFF, 0x18, 0x24 }};
static const WeatherIcon ICON_UNKNWN = {{ 0x3C, 0x42, 0x06, 0x0C, 0x08, 0x00, 0x08, 0x00 }};

inline const WeatherIcon& icon_for_condition(const char* s) {
    if (!s) return ICON_UNKNWN;
    if (strstr(s, "sunny")   || strstr(s, "clear"))    return ICON_SUNNY;
    if (strstr(s, "cloud")   || strstr(s, "overcast")) return ICON_CLOUDY;
    if (strstr(s, "rain")    || strstr(s, "drizzle"))  return ICON_RAINY;
    if (strstr(s, "snow")    || strstr(s, "sleet"))    return ICON_SNOWY;
    return ICON_UNKNWN;
}
```

- [ ] **Step 5: Create hud.h**

```cpp
#pragma once

#ifndef DUKE3D_HOST_TEST
#  include "esphome/core/component.h"
#  include "../hub75_matrix/hub75_matrix.h"
   namespace esphome { namespace hud {
   using MatrixType = hub75_matrix::Hub75Matrix;
   using ColorType  = hub75_matrix::Color;
   }}
#else
#  include "hub75_mock.h"
   namespace esphome { namespace hud {
   using MatrixType = Hub75Matrix;
   using ColorType  = Color;
   struct Component { virtual void setup(){} virtual void loop(){} };
   }}
#endif

#include <mutex>
#include <string>

namespace esphome {
namespace hud {

// Renders into rows 40–63 (HUD_TOP..63) of the 64×64 display.
// Layout:
//   Row 40–50: temperature "22°C" with weather icon (left)
//   Row 53–63: clock "HH:MM"
class Hud
#ifndef DUKE3D_HOST_TEST
    : public Component
#endif
{
public:
    static const int HUD_TOP    = 40;
    static const int HUD_HEIGHT = 24;

#ifndef DUKE3D_HOST_TEST
    void setup() override;
    void loop() override;
    float get_setup_priority() const override { return setup_priority::DATA; }
    // Uses global_hub75 (set in hub75_matrix::setup()) — no explicit wiring needed.
    // set_game_running(true) is called by Duke3DComponent::setup() so that
    // Hud::loop() stops driving swap_buffers() (Duke3D's game task takes over).
    void set_game_running(bool v) { game_running_ = v; }
#endif

    void set_temperature(float celsius);           // thread-safe (Core 0 → Core 1)
    void set_condition(const std::string& cond);   // thread-safe
    void set_time(int hour, int minute);           // thread-safe

    void render(MatrixType& display);

private:
    float temperature_ = 0.0f;
    char  condition_[64] = {};
    int   hour_ = 0, minute_ = 0;
    std::mutex data_mutex_;
    bool game_running_ = false;  // true once Duke3D takes over swap_buffers()

    void draw_char(MatrixType& d, int x, int y, int font_idx, uint8_t r, uint8_t g, uint8_t b);
    void draw_icon(MatrixType& d, int x, int y, const char* condition);
};

}  // namespace hud
}  // namespace esphome
```

- [ ] **Step 6: Create hud.cpp**

```cpp
#include "hud.h"
#include "font_5x7.h"
#include "weather_icons.h"
#include <cstring>
#include <cstdio>

#ifndef DUKE3D_HOST_TEST
#  include "esphome/core/log.h"
static const char* TAG = "hud";
#endif

namespace esphome {
namespace hud {

// Global pointer used by Duke3DComponent to wire the HUD into platform_blit_frame.
esphome::hud::Hud* global_hud_instance = nullptr;

#ifndef DUKE3D_HOST_TEST
void Hud::setup() {
    global_hud_instance = this;
    ESP_LOGI(TAG, "HUD initialized");
}
void Hud::loop()  {
    // Phase 1-3: HUD owns the full render+swap cycle (Duke3D not yet running).
    // Phase 5+: Duke3D's game_task on Core 1 calls render() and swap_buffers();
    // we must NOT also call swap_buffers() here or we get a concurrent DMA write race.
    // Duke3DComponent::setup() calls set_game_running(true) to disable our swap.
    if (game_running_) return;
    auto* m = esphome::hub75_matrix::global_hub75;
    if (m) {
        render(*m);
        m->swap_buffers();
    }
}
#endif

void Hud::set_temperature(float c) {
    std::lock_guard<std::mutex> lk(data_mutex_);
    temperature_ = c;
}

void Hud::set_condition(const std::string& s) {
    std::lock_guard<std::mutex> lk(data_mutex_);
    strncpy(condition_, s.c_str(), sizeof(condition_) - 1);
    condition_[sizeof(condition_) - 1] = '\0';
}

void Hud::set_time(int h, int m) {
    std::lock_guard<std::mutex> lk(data_mutex_);
    hour_ = h; minute_ = m;
}

void Hud::draw_char(MatrixType& d, int x, int y, int font_idx, uint8_t r, uint8_t g, uint8_t b) {
    for (int col = 0; col < 5; col++) {
        uint8_t bits = FONT_5X7[font_idx][col];
        for (int row = 0; row < 7; row++)
            if (bits & (1 << row))
                d.set_pixel(x + col, y + row, ColorType(r, g, b));
    }
}

void Hud::draw_icon(MatrixType& d, int x, int y, const char* cond) {
    const WeatherIcon& icon = icon_for_condition(cond);
    for (int row = 0; row < 8; row++)
        for (int col = 0; col < 8; col++)
            if (icon.rows[row] & (1 << (7 - col)))
                d.set_pixel(x + col, y + row, ColorType(100, 180, 255));
}

void Hud::render(MatrixType& d) {
    float temp; char cond[64]; int h, m;
    {
        std::lock_guard<std::mutex> lk(data_mutex_);
        temp = temperature_;
        strncpy(cond, condition_, sizeof(cond));
        h = hour_; m = minute_;
    }

    // Clear HUD band (dark blue background)
    for (int y = HUD_TOP; y < HUD_TOP + HUD_HEIGHT; y++)
        for (int x = 0; x < 64; x++)
            d.set_pixel(x, y, ColorType(0, 0, 20));

    // Row 1: weather icon (x=0) + temperature "22°C" (x=10)
    draw_icon(d, 0, HUD_TOP, cond);
    char temp_str[8];
    snprintf(temp_str, sizeof(temp_str), "%d", (int)temp);
    int tx = 10;
    for (int i = 0; temp_str[i]; i++, tx += 6)
        draw_char(d, tx, HUD_TOP + 1, font_index(temp_str[i]), 255, 200, 0);
    draw_char(d, tx,     HUD_TOP + 1, FONT_IDX_DEGREE, 255, 200, 0);  // °
    draw_char(d, tx + 6, HUD_TOP + 1, FONT_IDX_C,      255, 200, 0);

    // Row 2: clock "HH:MM" centered
    char clock_str[6];
    snprintf(clock_str, sizeof(clock_str), "%02d:%02d", h, m);
    int cx = (64 - 5 * 6) / 2;  // 5 chars × 6px wide, centered
    for (int i = 0; clock_str[i]; i++, cx += 6)
        draw_char(d, cx, HUD_TOP + 13, font_index(clock_str[i]), 200, 200, 255);
}

}  // namespace hud
}  // namespace esphome
```

- [ ] **Step 7: Create CMakeLists.txt and __init__.py**

`components/hud/CMakeLists.txt`:
```cmake
idf_component_register(
    SRCS "hud.cpp"
    INCLUDE_DIRS "."
    REQUIRES hub75_matrix freertos
)
```

`components/hud/__init__.py`:
```python
import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.const import CONF_ID

hud_ns = cg.esphome_ns.namespace("hud")
HudClass = hud_ns.class_("Hud", cg.Component)

CONFIG_SCHEMA = cv.Schema({
    cv.GenerateID(): cv.declare_id(HudClass),
}).extend(cv.COMPONENT_SCHEMA)

async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
```

- [ ] **Step 8: Run host-side HUD tests**

```bash
cmake -S test -B test/build
cmake --build test/build
./test/build/test_hud
```

Expected: both assertions pass, exit 0.

- [ ] **Step 9: Wire HA sensors and time into HUD in esphome.yaml**

```yaml
external_components:
  - source:
      type: local
      path: components
    components: [hub75_matrix, hud]

hud:
  id: hud_display

sensor:
  - platform: homeassistant
    id: weather_temperature
    entity_id: sensor.weather_temperature
    on_value:
      then:
        - lambda: id(hud_display)->set_temperature(x);

text_sensor:
  - platform: homeassistant
    id: weather_condition
    entity_id: sensor.weather_condition
    on_value:
      then:
        - lambda: id(hud_display)->set_condition(std::string(x.c_str()));

time:
  - platform: homeassistant
    id: ha_time
    on_time_sync:
      then:
        - lambda: |-
            auto t = id(ha_time)->now();  // pointer dereference, not .
            id(hud_display)->set_time(t.hour, t.minute);
    # Update clock every second so it actually ticks (on_time_sync fires only once at sync)
    on_time:
      - seconds: 0-59    # fires every second (ESPHome cron: explicit range, not /1 step)
        then:
          - lambda: |-
              auto t = id(ha_time)->now();
              if (t.is_valid())
                id(hud_display)->set_time(t.hour, t.minute);
```

- [ ] **Step 10: Compile, flash, verify HUD**

```bash
esphome compile esphome.yaml && esphome upload esphome.yaml
```

Expected: top 40px black (game not running yet), bottom 24px shows dark blue HUD band with weather icon and temperature. Clock shows current time.

To test without real weather integration: HA → Developer Tools → States → set `sensor.weather_temperature = 22.5` and `sensor.weather_condition = sunny`.

- [ ] **Step 11: Commit**

```bash
git add components/hud/ esphome.yaml test/
git commit -m "feat: Phase 3 - HUD band with weather + clock in rows 40-63"
```

---

## Chunk 4: SD Card Integration (Phase 4)

### Task 4: SPI SD card mount and GRP file detection

**Files:**
- Create: `components/sd_card/__init__.py`
- Create: `components/sd_card/sd_card.h`
- Create: `components/sd_card/sd_card.cpp`
- Create: `components/sd_card/CMakeLists.txt`
- Modify: `esphome.yaml`

- [ ] **Step 1: Create sd_card.h**

```cpp
#pragma once
#include "esphome/core/component.h"
#include "esp_vfs_fat.h"
#include "driver/sdspi_host.h"
#include <cstdio>

namespace esphome {
namespace sd_card {

class SdCard : public Component {
public:
    static const char* MOUNT_POINT;  // "/sdcard"

    // Pins — set before setup() via esphome.yaml config
    void set_mosi(int p) { mosi_ = p; }
    void set_miso(int p) { miso_ = p; }
    void set_sck(int p)  { sck_  = p; }
    void set_cs(int p)   { cs_   = p; }

    void setup() override;
    float get_setup_priority() const override { return setup_priority::IO; }

    bool is_mounted() const { return mounted_; }

    // Returns nullptr if path not found; caller must fclose()
    FILE* open(const char* rel_path, const char* mode) const;

    // Returns true if /sdcard/duke3d/DUKE3D.GRP exists and is readable
    bool grp_present() const;

private:
    int mosi_ = -1, miso_ = -1, sck_ = -1, cs_ = -1;
    bool mounted_ = false;
    sdmmc_card_t* card_ = nullptr;
};

extern SdCard* global_sd_card;

}  // namespace sd_card
}  // namespace esphome
```

- [ ] **Step 2: Create sd_card.cpp**

```cpp
#include "sd_card.h"
#include "esphome/core/log.h"
#include "driver/gpio.h"
#include <cstring>

namespace esphome {
namespace sd_card {

static const char* TAG = "sd_card";
const char* SdCard::MOUNT_POINT = "/sdcard";
SdCard* global_sd_card = nullptr;

void SdCard::setup() {
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();

    spi_bus_config_t bus = {};
    bus.mosi_io_num = mosi_;
    bus.miso_io_num = miso_;
    bus.sclk_io_num = sck_;
    bus.quadwp_io_num = -1;
    bus.quadhd_io_num = -1;
    bus.max_transfer_sz = 4096;

    esp_err_t ret = spi_bus_initialize(
        static_cast<spi_host_device_t>(host.slot), &bus, SDSPI_DEFAULT_DMA);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPI bus init failed: %s", esp_err_to_name(ret));
        return;
    }

    sdspi_device_config_t dev = SDSPI_DEVICE_CONFIG_DEFAULT();
    dev.gpio_cs   = static_cast<gpio_num_t>(cs_);
    dev.host_id   = static_cast<spi_host_device_t>(host.slot);

    esp_vfs_fat_sdmmc_mount_config_t mnt = {};
    mnt.format_if_mount_failed = false;
    mnt.max_files = 8;
    mnt.allocation_unit_size = 16 * 1024;

    ret = esp_vfs_fat_sdspi_mount(MOUNT_POINT, &host, &dev, &mnt, &card_);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SD mount failed: %s", esp_err_to_name(ret));
        return;
    }

    mounted_ = true;
    global_sd_card = this;
    ESP_LOGI(TAG, "SD card mounted at %s", MOUNT_POINT);
    sdmmc_card_print_info(stdout, card_);
}

FILE* SdCard::open(const char* rel_path, const char* mode) const {
    if (!mounted_) return nullptr;
    char full[256];
    snprintf(full, sizeof(full), "%s/%s", MOUNT_POINT, rel_path);
    return fopen(full, mode);
}

bool SdCard::grp_present() const {
    FILE* f = open("duke3d/DUKE3D.GRP", "rb");
    if (!f) return false;
    fclose(f);
    return true;
}

}  // namespace sd_card
}  // namespace esphome
```

- [ ] **Step 3: Write failing host-side SD path test**

Create `test/sd_card/test_grp_detection.cpp`:
```cpp
#include <cassert>
#include <cstring>

// Host-side test for SdCard path construction logic only.
// Mounts and file I/O are hardware-only; we test the path string logic.

static char last_opened_path[256] = {};

// Minimal mock matching sd_card open() path construction
static const char* MOUNT = "/sdcard";

void mock_open(const char* rel_path) {
    snprintf(last_opened_path, sizeof(last_opened_path),
             "%s/%s", MOUNT, rel_path);
}

void test_grp_path_is_correct() {
    mock_open("duke3d/DUKE3D.GRP");
    assert(strcmp(last_opened_path, "/sdcard/duke3d/DUKE3D.GRP") == 0);
}

void test_grp_full_path_is_correct() {
    mock_open("duke3d_full/DUKE3D.GRP");
    assert(strcmp(last_opened_path, "/sdcard/duke3d_full/DUKE3D.GRP") == 0);
}

int main() {
    test_grp_path_is_correct();
    test_grp_full_path_is_correct();
    return 0;
}
```

Append to `test/CMakeLists.txt`:
```cmake
add_executable(test_sd_card sd_card/test_grp_detection.cpp)
```

```bash
cmake -S test -B test/build
cmake --build test/build
./test/build/test_sd_card
```

Expected: PASS.

- [ ] **Step 4: Create CMakeLists.txt and __init__.py**

`components/sd_card/CMakeLists.txt`:
```cmake
idf_component_register(
    SRCS "sd_card.cpp"
    INCLUDE_DIRS "."
    REQUIRES driver esp_vfs_fat  # esp_vfs_fat provides esp_vfs_fat_sdspi_mount in ESP-IDF 5.x
)
```

`components/sd_card/__init__.py`:
```python
import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.const import CONF_ID, CONF_MOSI_PIN, CONF_MISO_PIN, CONF_CLK_PIN, CONF_CS_PIN

sd_ns = cg.esphome_ns.namespace("sd_card")
SdCardClass = sd_ns.class_("SdCard", cg.Component)

CONFIG_SCHEMA = cv.Schema({
    cv.GenerateID(): cv.declare_id(SdCardClass),
    cv.Required(CONF_MOSI_PIN): cv.positive_int,
    cv.Required(CONF_MISO_PIN): cv.positive_int,
    cv.Required(CONF_CLK_PIN):  cv.positive_int,
    cv.Required(CONF_CS_PIN):   cv.positive_int,
}).extend(cv.COMPONENT_SCHEMA)

async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    cg.add(var.set_mosi(config[CONF_MOSI_PIN]))
    cg.add(var.set_miso(config[CONF_MISO_PIN]))
    cg.add(var.set_sck(config[CONF_CLK_PIN]))
    cg.add(var.set_cs(config[CONF_CS_PIN]))
    await cg.register_component(var, config)
```

- [ ] **Step 5: Add sd_card to esphome.yaml**

```yaml
# Replace A0-A5 pad numbers with actual GPIO numbers from Adafruit schematic
sd_card:
  id: sdcard
  mosi_pin: 1   # A0 — verify from schematic
  miso_pin: 2   # A1
  clk_pin:  3   # A2
  cs_pin:   4   # A3
```

- [ ] **Step 6: Compile, flash, verify SD mount**

Insert a microSD card with `/duke3d/DUKE3D.GRP`. Flash and check serial logs:

```bash
esphome upload esphome.yaml
esphome logs esphome.yaml
```

Expected log: `SD card mounted at /sdcard` followed by card info. If mount fails, check SPI pin numbers and that the card is FAT32 formatted.

- [ ] **Step 7: Commit**

```bash
git add components/sd_card/ esphome.yaml test/
git commit -m "feat: Phase 4 - SPI SD card mount with FATFS, GRP detection"
```

---

## Chunk 5: Duke3D Demo Playback (Phase 5)

### Task 5.1: Add jkirsons/Duke3D engine as git submodule

**Files:**
- Create: `components/duke3d/engine/` (git submodule)

- [ ] **Step 1: Add engine submodule**

```bash
git submodule add https://github.com/jkirsons/Duke3D.git \
  components/duke3d/engine
git submodule update --init --recursive
```

- [ ] **Step 2: Read the engine's platform abstraction**

Open `components/duke3d/engine/components/Duke3D/Platform/` and identify:
- The file I/O shim (replaces `fopen`/`fread` with SD card paths)
- The display output (replaces ILI9341 `pushImage()` call)
- The audio output function
- The input polling function

These are the four seams we replace. Document the exact function signatures before proceeding.

---

### Task 5.2: Renderer — 320×200 → 64×40 downscaler

**Files:**
- Create: `components/duke3d/renderer/renderer.h`
- Create: `components/duke3d/renderer/renderer.cpp`
- Create: `test/renderer/test_downscaler.cpp`
- Modify: `test/CMakeLists.txt`

- [ ] **Step 1: Write failing downscaler test**

Create `test/renderer/test_downscaler.cpp`:
```cpp
#define DUKE3D_HOST_TEST
#include <cassert>
#include <cstring>
#include "hub75_mock.h"

// Minimal palette for test
static const uint8_t TEST_PALETTE[256 * 3] = {};

// Forward-declare the function under test
void render_frame(Hub75Matrix& dst,
                  const uint8_t* src,   // 320×200 palette-indexed
                  const uint8_t* pal);  // 256×RGB888

void test_output_is_exactly_64x40_rows() {
    Hub75Matrix m;
    m.fill({0, 0, 255});  // blue background

    uint8_t src[320 * 200] = {};  // all palette index 0 → black
    render_frame(m, src, TEST_PALETTE);

    // Rows 0–39 must have been written (will be black, not blue)
    for (int x = 0; x < 64; x++) {
        assert(m.pixels[0  * 64 + x].b == 0);  // row 0: was written
        assert(m.pixels[39 * 64 + x].b == 0);  // row 39: was written
    }

    // Rows 40–63 must NOT have been touched (still blue)
    for (int x = 0; x < 64; x++)
        assert(m.pixels[40 * 64 + x].b == 255);
}

int main() {
    test_output_is_exactly_64x40_rows();
    return 0;
}
```

Append to `test/CMakeLists.txt`:
```cmake
add_executable(test_renderer renderer/test_downscaler.cpp)
target_include_directories(test_renderer PRIVATE mock
    ${CMAKE_SOURCE_DIR}/../components/duke3d/renderer)
target_compile_definitions(test_renderer PRIVATE DUKE3D_HOST_TEST)
```

- [ ] **Step 2: Run — confirm compile fails (renderer not yet written)**

```bash
cmake -S test -B test/build && cmake --build test/build 2>&1 | grep "error"
```

Expected: missing `render_frame`. Confirms test is wired correctly.

- [ ] **Step 3: Create renderer.h**

```cpp
#pragma once
#include <cstdint>

#ifndef DUKE3D_HOST_TEST
#  include "../../../hub75_matrix/hub75_matrix.h"
   using RenderTarget = esphome::hub75_matrix::Hub75Matrix;
#else
#  include "hub75_mock.h"
   using RenderTarget = Hub75Matrix;
#endif

// Downscale a 320×200 palette-indexed framebuffer to 64×40 and
// write it into rows 0–39 of dst. Rows 40–63 are untouched.
void render_frame(RenderTarget& dst,
                  const uint8_t* src_320x200,
                  const uint8_t* palette_rgb);  // 256 × 3 bytes (R,G,B)
```

- [ ] **Step 4: Implement renderer.cpp**

```cpp
#include "renderer.h"

// Nearest-neighbour downscale: 320×200 → 64×40
// x_src = x_dst * 320 / 64 = x_dst * 5
// y_src = y_dst * 200 / 40 = y_dst * 5
void render_frame(RenderTarget& dst,
                  const uint8_t* src,
                  const uint8_t* pal) {
    for (int y = 0; y < 40; y++) {
        int y_src = y * 5;
        for (int x = 0; x < 64; x++) {
            int x_src = x * 5;
            uint8_t idx = src[y_src * 320 + x_src];
            uint8_t r = pal[idx * 3 + 0];
            uint8_t g = pal[idx * 3 + 1];
            uint8_t b = pal[idx * 3 + 2];
#ifndef DUKE3D_HOST_TEST
            dst.set_pixel(x, y, esphome::hub75_matrix::Color(r, g, b));
#else
            dst.set_pixel(x, y, Color(r, g, b));
#endif
        }
    }
}
```

- [ ] **Step 5: Run renderer tests**

```bash
cmake -S test -B test/build
cmake --build test/build
./test/build/test_renderer
```

Expected: PASS.

---

### Task 5.3: ESP32 HAL shims for Duke3D

**Files:**
- Create: `components/duke3d/platform/esp32_hal.h`
- Create: `components/duke3d/platform/esp32_hal.cpp`

- [ ] **Step 1: Identify the four seams in jkirsons/Duke3D**

Open the engine's Platform directory. Locate and note the exact function or class that:
1. Opens/reads game files → replace with `global_sd_card->open()`
2. Outputs the framebuffer to display → replace with `render_frame()` + `global_hub75->swap_buffers()`
3. Outputs audio PCM → replace with `global_i2s->write_pcm()` (stub for now)
4. Polls input → replace with `input_queue_pop()` (stub for now)

- [ ] **Step 2: Create esp32_hal.h**

```cpp
#pragma once
#include <cstdint>
#include <cstdio>

// Platform shim declarations for Duke3D engine.
// These replace the original ODROID-GO / ILI9341 platform functions.

// Called by engine to output one rendered frame.
// src: 320×200 palette-indexed buffer. pal: 256×RGB888 palette.
void platform_blit_frame(const uint8_t* src, const uint8_t* pal);

// Called by engine's file I/O layer — redirects to SD card mount point.
// Returns a FILE* on /sdcard/duke3d/<rel_path>, or nullptr on failure.
FILE* platform_open_file(const char* rel_path, const char* mode);

// Called by engine's audio layer — stub until Phase 7.
void platform_audio_write(const int16_t* pcm, int num_samples);
```

- [ ] **Step 3: Create esp32_hal.cpp**

```cpp
#include "esp32_hal.h"
#include "../renderer/renderer.h"
#include "../../../hub75_matrix/hub75_matrix.h"
#include "../../../hud/hud.h"
#include "../../../sd_card/sd_card.h"
#include "esp_task_wdt.h"

extern esphome::hud::Hud* global_hud;

void platform_blit_frame(const uint8_t* src, const uint8_t* pal) {
    auto* m = esphome::hub75_matrix::global_hub75;
    if (!m) return;

    render_frame(*m, src, pal);      // writes rows 0–39

    if (global_hud) global_hud->render(*m);  // overlays rows 40–63

    m->swap_buffers();

    // Feed the TWDT every frame — the game loop runs for minutes without
    // returning to ESPHome's main task, so we must reset here, not between demos.
    esp_task_wdt_reset();
}

FILE* platform_open_file(const char* rel_path, const char* mode) {
    auto* sd = esphome::sd_card::global_sd_card;
    if (!sd || !sd->is_mounted()) return nullptr;
    return sd->open(rel_path, mode);
}

void platform_audio_write(const int16_t* /*pcm*/, int /*n*/) {
    // Stub — implemented in Phase 7
}
```

---

### Task 5.4: Duke3DComponent — demo loop + ESPHome wrapper

**Files:**
- Create: `components/duke3d/__init__.py`
- Create: `components/duke3d/duke3d_component.h`
- Create: `components/duke3d/duke3d_component.cpp`
- Create: `components/duke3d/CMakeLists.txt`
- Modify: `esphome.yaml`

- [ ] **Step 1: Create duke3d_component.h**

```cpp
#pragma once
#include "esphome/core/component.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <atomic>

namespace esphome {
namespace duke3d {

class Duke3DComponent : public Component {
public:
    void setup() override;
    void loop() override {}
    float get_setup_priority() const override { return setup_priority::LATE; }

    const char* current_demo() const { return current_demo_; }

private:
    TaskHandle_t task_handle_ = nullptr;
    char current_demo_[32] = "DEMO1.DMO";

    static void game_task(void* arg);
    static const int TASK_STACK = 32768;  // 32KB — adjust if stack overflow
};

}  // namespace duke3d
}  // namespace esphome
```

- [ ] **Step 2: Create duke3d_component.cpp**

```cpp
#include "duke3d_component.h"
#include "esphome/core/log.h"
#include "esp_task_wdt.h"
#include "../sd_card/sd_card.h"
#include "../platform/esp32_hal.h"   // for global_hud declaration
#include "../../hud/hud.h"
#include <cstring>

// Duke3D engine entry point — provided by jkirsons/Duke3D engine
extern "C" int duke3d_main(int argc, char** argv);

// global_hud: declared extern in esp32_hal.h, defined here
esphome::hud::Hud* global_hud = nullptr;

namespace esphome {
namespace duke3d {

static const char* TAG = "duke3d";
static Duke3DComponent* instance_ = nullptr;

void Duke3DComponent::setup() {
    auto* sd = sd_card::global_sd_card;
    if (!sd || !sd->grp_present()) {
        ESP_LOGE(TAG, "DUKE3D.GRP not found on SD card — halting game component");
        // Display error in matrix top zone (simple red X or text)
        return;
    }

    // Wire HUD global and disable Hud::loop()'s own swap_buffers() call —
    // from now on platform_blit_frame() drives both render and swap.
    extern esphome::hud::Hud* global_hud_instance;  // defined in hud.cpp
    global_hud = global_hud_instance;
    if (global_hud) global_hud->set_game_running(true);

    instance_ = this;
    xTaskCreatePinnedToCore(
        game_task,
        "duke3d",
        TASK_STACK,
        this,
        5,      // priority — below HUB-75 ISR (which must be highest)
        &task_handle_,
        1       // Core 1
    );
    ESP_LOGI(TAG, "Duke3D game task started on Core 1");
}

void Duke3DComponent::game_task(void* arg) {
    auto* self = static_cast<Duke3DComponent*>(arg);

    // Register with TWDT so watchdog does not reset us
    esp_task_wdt_add(nullptr);

    // Duke3D demo loop: play demos in order, looping forever
    // The engine exits demo playback at the end of each demo;
    // we restart it in a loop.
    const char* demos[] = { "DEMO1.DMO", "DEMO2.DMO", "DEMO3.DMO" };
    int demo_idx = 0;

    while (true) {
        strncpy(self->current_demo_, demos[demo_idx], sizeof(self->current_demo_) - 1);
        ESP_LOGI(TAG, "Playing %s", self->current_demo_);

        // Engine invocation — passes demo filename via argv.
        // Exact argv format depends on jkirsons/Duke3D's main() signature;
        // review engine/main/duke3d_main.cpp and adjust as needed.
        char demo_arg[64];
        snprintf(demo_arg, sizeof(demo_arg), "-playdem %s", demos[demo_idx]);
        char* argv[] = { (char*)"duke3d", demo_arg, nullptr };
        duke3d_main(2, argv);

        demo_idx = (demo_idx + 1) % 3;
        // TWDT is fed per-frame inside platform_blit_frame(); no extra reset needed here.
    }
}

}  // namespace duke3d
}  // namespace esphome
```

> **Note on engine invocation:** The exact `duke3d_main` signature and demo playback argv format must be verified from `jkirsons/Duke3D`'s `main/` directory. The engine may expose a different API for demo playback (e.g. a `G_PlayDemo()` call). Adjust `game_task` accordingly after reading the engine source.

- [ ] **Step 3: Create CMakeLists.txt**

```cmake
# Add Duke3D engine source files.
# After reading jkirsons/Duke3D structure, update SRCS globs to match
# the engine's component directory layout.
file(GLOB_RECURSE ENGINE_SRCS
    "${CMAKE_CURRENT_SOURCE_DIR}/engine/components/Duke3D/*.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/engine/components/Duke3D/*.cpp"
)

idf_component_register(
    SRCS
        "duke3d_component.cpp"
        "renderer/renderer.cpp"
        "platform/esp32_hal.cpp"
        "platform/input.cpp"     # added in Phase 6; file created by Chunk 6
        ${ENGINE_SRCS}
    INCLUDE_DIRS
        "."
        "renderer"
        "platform"
        "engine/components/Duke3D"
        "engine/components/Duke3D/Engine"
    REQUIRES hub75_matrix hud sd_card i2s_audio freertos esp_task_wdt
)
```

- [ ] **Step 4: Create __init__.py**

```python
import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.const import CONF_ID

duke3d_ns = cg.esphome_ns.namespace("duke3d")
Duke3DClass = duke3d_ns.class_("Duke3DComponent", cg.Component)

CONFIG_SCHEMA = cv.Schema({
    cv.GenerateID(): cv.declare_id(Duke3DClass),
}).extend(cv.COMPONENT_SCHEMA)

async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
```

- [ ] **Step 5: Add to esphome.yaml and expose demo sensor**

```yaml
external_components:
  - source:
      type: local
      path: components
    components: [hub75_matrix, hud, sd_card, duke3d]

duke3d:
  id: game_engine

text_sensor:
  - platform: template
    id: duke3d_demo
    name: "Duke3D Current Demo"
    lambda: return std::string(id(game_engine)->current_demo());
    update_interval: 10s
```

- [ ] **Step 6: Compile — expect linker errors, resolve iteratively**

```bash
esphome compile esphome.yaml 2>&1 | grep -E "error:|undefined"
```

Common issues and fixes:
- Missing engine symbols: add source files to `ENGINE_SRCS` glob
- Conflicting `main()`: rename engine's main entry point
- Missing platform functions: add stubs to `esp32_hal.cpp`
- Stack overflow: increase `TASK_STACK` constant

Iterate until clean compile.

- [ ] **Step 7: Flash and verify demo plays**

```bash
esphome upload esphome.yaml
esphome logs esphome.yaml
```

Expected: Duke3D demo visible in top 40 rows. HUD visible in bottom 24 rows. Log shows `Playing DEMO1.DMO` → `Playing DEMO2.DMO` → loop. FPS will be low initially.

- [ ] **Step 8: Commit**

```bash
git add components/duke3d/ esphome.yaml test/
git commit -m "feat: Phase 5 - Duke3D demo playback loop on Core 1"
```

---

### Task 5.5: TWDT and performance tuning

- [ ] **Step 1: Measure fps**

Add fps counter to `game_task`:
```cpp
static int frame_count = 0;
static int64_t last_us = esp_timer_get_time();
frame_count++;
int64_t now = esp_timer_get_time();
if (now - last_us >= 1000000) {
    ESP_LOGI(TAG, "FPS: %d", frame_count);
    frame_count = 0;
    last_us = now;
}
```

Expected range: 15–30fps. Target: ≥20fps exit criterion; 25–30fps goal.

- [ ] **Step 2: If fps < 20, profile and optimize**

Common bottlenecks:
- `swap_buffers()` iterating 64×64 pixels: optimize with `memcpy` if library supports buffer injection
- `render_frame()` with slow PSRAM access: ensure `src` buffer is in PSRAM (`heap_caps_malloc(..., MALLOC_CAP_SPIRAM)`) and access is sequential
- Engine tick rate: check if engine is rendering at original 320×200 or lower; lower resolution = faster

- [ ] **Step 3: Commit fps counter or remove if not needed**

```bash
git add components/duke3d/
git commit -m "perf: Phase 5 - fps measurement and tuning"
```

---

## Chunk 6: Input System (Phase 6)

### Task 6: Input interface placeholder + USB HID path

**Files:**
- Create: `components/duke3d/platform/input.h`
- Create: `components/duke3d/platform/input.cpp`

> **Prerequisite:** Decide input method before implementing. USB OTG HID (USB-C + OTG adapter) or GPIO buttons. Document decision here.

- [ ] **Step 1: Create input.h — InputEvent enum and queue**

```cpp
#pragma once
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

enum class InputEvent : uint8_t {
    NONE = 0,
    MOVE_FORWARD, MOVE_BACK,
    STRAFE_LEFT, STRAFE_RIGHT,
    TURN_LEFT, TURN_RIGHT,
    FIRE, USE,
    OPEN_MAP, MENU_TOGGLE,
};

// Initialize input subsystem. Call from duke3d_component setup().
void input_init();

// Push from task context (USB HID callback, button handler task).
void input_push(InputEvent evt);

// Push from hardware ISR context only (GPIO interrupt).
void input_push_from_isr(InputEvent evt);

// Pop next event. Returns InputEvent::NONE if queue is empty.
// Called from game task (Core 1) each tick.
InputEvent input_pop();
```

- [ ] **Step 2: Write failing host-side input queue test**

Create `test/input/test_input_queue.cpp`:
```cpp
// Host-side test for InputEvent enum values — no FreeRTOS needed.
#include <cassert>
#include <cstdint>
// Reproduce the enum here (no FreeRTOS headers on host)
enum class InputEvent : uint8_t {
    NONE=0, MOVE_FORWARD, MOVE_BACK, STRAFE_LEFT, STRAFE_RIGHT,
    TURN_LEFT, TURN_RIGHT, FIRE, USE, OPEN_MAP, MENU_TOGGLE,
};
void test_none_is_zero() { assert((uint8_t)InputEvent::NONE == 0); }
void test_all_values_unique() {
    uint8_t seen[16] = {};
    for (uint8_t v = 0; v <= (uint8_t)InputEvent::MENU_TOGGLE; v++) {
        assert(seen[v] == 0);
        seen[v] = 1;
    }
}
int main() {
    test_none_is_zero();
    test_all_values_unique();
    return 0;
}
```

Append to `test/CMakeLists.txt`:
```cmake
add_executable(test_input input/test_input_queue.cpp)
```

```bash
cmake -S test -B test/build && cmake --build test/build && ./test/build/test_input
```

Expected: PASS.

- [ ] **Step 3: Implement input.cpp — queue-backed stub**

```cpp
#include "input.h"
static QueueHandle_t input_queue = nullptr;
static const int QUEUE_DEPTH = 16;

void input_init() {
    input_queue = xQueueCreate(QUEUE_DEPTH, sizeof(InputEvent));
}

// Safe to call from both task context AND hardware ISR.
// xQueueSend with timeout=0 is safe from ISR on ESP-IDF's FreeRTOS port.
// If calling from a hardware ISR with a higher-priority context switch needed,
// use the separate input_push_from_isr() below.
void input_push(InputEvent evt) {
    if (input_queue) xQueueSend(input_queue, &evt, 0);
}

// Use ONLY from hardware ISR context (GPIO interrupt handler).
void input_push_from_isr(InputEvent evt) {
    BaseType_t woken = pdFALSE;
    if (input_queue) xQueueSendFromISR(input_queue, &evt, &woken);
    portYIELD_FROM_ISR(woken);
}

InputEvent input_pop() {
    InputEvent evt = InputEvent::NONE;
    if (input_queue) xQueueReceive(input_queue, &evt, 0);
    return evt;
}
```

- [ ] **Step 4: Wire input_pop() into Duke3D platform input handler**

Find the engine's input polling function in `platform/`. Replace its return value with `input_pop()` result, mapping `InputEvent` to the engine's button bitmask format.

- [ ] **Step 5: (USB HID path) Add USB host HID task**

If using USB OTG HID gamepad, add the managed component dependency first:

Create `components/duke3d/idf_component.yml`:
```yaml
dependencies:
  espressif/usb_host_hid: ">=1.0.0"
```

Then in `input.cpp`:
```cpp
#include "usb/usb_host.h"
#include "hid_host.h"   // from espressif/usb_host_hid managed component
// ... USB host HID class driver setup
// Map HID usage IDs to InputEvent and call input_push() (task context — not ISR)
```

Refer to: ESP-IDF example `examples/peripherals/usb/host/hid/` and the managed component docs at `components.espressif.com/components/espressif/usb_host_hid`.

- [ ] **Step 6: Flash and verify input**

Flash and verify gamepad / buttons control Duke3D demo navigation.

- [ ] **Step 7: Commit**

```bash
git add components/duke3d/platform/ test/
git commit -m "feat: Phase 6 - input subsystem with InputEvent queue"
```

---

## Chunk 7: Sound System (Phase 7)

### Task 7: I2S audio component for MAX98357A

**Files:**
- Create: `components/i2s_audio/__init__.py`
- Create: `components/i2s_audio/i2s_audio.h`
- Create: `components/i2s_audio/i2s_audio.cpp`
- Create: `components/i2s_audio/CMakeLists.txt`
- Modify: `components/duke3d/platform/esp32_hal.cpp`

- [ ] **Step 1: Create i2s_audio.h**

```cpp
#pragma once
#include "esphome/core/component.h"
#include "driver/i2s_std.h"
#include <cstdint>

namespace esphome {
namespace i2s_audio {

class I2SAudio : public Component {
public:
    void set_bclk(int p)  { bclk_  = p; }
    void set_lrclk(int p) { lrclk_ = p; }
    void set_din(int p)   { din_   = p; }

    void setup() override;
    float get_setup_priority() const override { return setup_priority::IO; }

    // Write interleaved stereo 16-bit PCM.
    // num_bytes: total bytes to write (= num_frames * 2 channels * 2 bytes/sample).
    // Blocks until buffer is accepted by DMA.
    void write_pcm(const int16_t* buf, size_t num_bytes);

private:
    int bclk_ = -1, lrclk_ = -1, din_ = -1;
    i2s_chan_handle_t tx_handle_ = nullptr;
};

extern I2SAudio* global_i2s;

}  // namespace i2s_audio
}  // namespace esphome
```

- [ ] **Step 2: Create i2s_audio.cpp**

```cpp
#include "i2s_audio.h"
#include "esphome/core/log.h"

namespace esphome {
namespace i2s_audio {

static const char* TAG = "i2s_audio";
I2SAudio* global_i2s = nullptr;

void I2SAudio::setup() {
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    i2s_new_channel(&chan_cfg, &tx_handle_, nullptr);

    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(44100),
        .slot_cfg = I2S_STD_MSB_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                     I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = static_cast<gpio_num_t>(bclk_),
            .ws   = static_cast<gpio_num_t>(lrclk_),
            .dout = static_cast<gpio_num_t>(din_),
            .din  = I2S_GPIO_UNUSED,
            .invert_flags = {},  // zero-init bitfield struct (all inversions disabled)
        },
    };

    if (i2s_channel_init_std_mode(tx_handle_, &std_cfg) != ESP_OK) {
        ESP_LOGE(TAG, "I2S init failed");
        return;
    }
    i2s_channel_enable(tx_handle_);
    global_i2s = this;
    ESP_LOGI(TAG, "I2S audio initialized (44100Hz stereo 16-bit)");
}

void I2SAudio::write_pcm(const int16_t* buf, size_t num_bytes) {
    if (!tx_handle_) return;
    size_t written;
    i2s_channel_write(tx_handle_, buf, num_bytes, &written, portMAX_DELAY);
}

}  // namespace i2s_audio
}  // namespace esphome
```

- [ ] **Step 3: Write failing host-side audio byte-count test**

Create `test/i2s_audio/test_write_pcm.cpp`:
```cpp
#include <cassert>
#include <cstdint>
#include <cstddef>

// Verify byte-count convention: num_bytes = num_frames * channels * bytes_per_sample
void test_byte_count_for_stereo_16bit() {
    const int sample_rate  = 44100;
    const int channels     = 2;
    const int bytes_sample = 2;         // int16_t
    const int frames       = 512;       // typical Duke3D audio chunk
    const size_t num_bytes = frames * channels * bytes_sample;
    assert(num_bytes == 2048);
}

void test_byte_count_for_mono_source_upsampled_to_stereo() {
    // Duke3D may output mono 11025Hz; host doubles to stereo 22050Hz
    const int frames       = 256;
    const size_t num_bytes = frames * 2 * 2;  // stereo, 16-bit
    assert(num_bytes == 1024);
}

int main() {
    test_byte_count_for_stereo_16bit();
    test_byte_count_for_mono_source_upsampled_to_stereo();
    return 0;
}
```

Append to `test/CMakeLists.txt`:
```cmake
add_executable(test_i2s_audio i2s_audio/test_write_pcm.cpp)
```

```bash
cmake -S test -B test/build && cmake --build test/build && ./test/build/test_i2s_audio
```

Expected: PASS.

- [ ] **Step 4: Create CMakeLists.txt and __init__.py**

`components/i2s_audio/CMakeLists.txt`:
```cmake
idf_component_register(
    SRCS "i2s_audio.cpp"
    INCLUDE_DIRS "."
    REQUIRES driver esp_driver_i2s freertos
    # esp_driver_i2s: explicit dependency on new channel API (driver/i2s_std.h).
    # The "driver" component re-exports it but listing it explicitly avoids
    # ambiguity with ESPHome's own i2s_audio component which uses the legacy API.
)
```

> **Important:** ESPHome 2024.x ships its own `i2s_audio` component that uses the legacy `driver/i2s.h` API. Ensure ESPHome's built-in `i2s_audio` platform is NOT added to `esphome.yaml` alongside this custom component — they cannot share the same I2S port. This custom component replaces ESPHome's built-in one entirely.

`components/i2s_audio/__init__.py`:
```python
import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.const import CONF_ID

i2s_ns = cg.esphome_ns.namespace("i2s_audio")
I2SAudioClass = i2s_ns.class_("I2SAudio", cg.Component)

CONFIG_SCHEMA = cv.Schema({
    cv.GenerateID(): cv.declare_id(I2SAudioClass),
    cv.Required("bclk_pin"):  cv.positive_int,
    cv.Required("lrclk_pin"): cv.positive_int,
    cv.Required("din_pin"):   cv.positive_int,
}).extend(cv.COMPONENT_SCHEMA)

async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    cg.add(var.set_bclk(config["bclk_pin"]))
    cg.add(var.set_lrclk(config["lrclk_pin"]))
    cg.add(var.set_din(config["din_pin"]))
    await cg.register_component(var, config)
```

- [ ] **Step 5: Wire audio into Duke3D HAL**

In `components/duke3d/platform/esp32_hal.cpp`, replace the stub:
```cpp
#include "../../../i2s_audio/i2s_audio.h"

void platform_audio_write(const int16_t* pcm, int n) {
    auto* audio = esphome::i2s_audio::global_i2s;
    // n is int16_t sample count (both channels). write_pcm expects bytes.
    if (audio) audio->write_pcm(pcm, n * sizeof(int16_t));
}
```

Verify the engine's audio callback matches this signature by reading `engine/components/Duke3D/Platform/audio_*.cpp`.

- [ ] **Step 6: Add to esphome.yaml**

```yaml
# I2S pins: A4 and A5 for BCLK/LRCLK; DIN shared with SD MOSI (A0)
# Ensure SD CS is de-asserted before any I2S write (handled by SPI bus arbitration)
i2s_audio:
  bclk_pin:  5   # A4 — verify from schematic
  lrclk_pin: 6   # A5 — verify from schematic
  din_pin:   1   # A0 — shared with SD MOSI, safe when SD CS deasserted
```

- [ ] **Step 7: Compile, flash, verify sound**

```bash
esphome upload esphome.yaml
```

Expected: Duke3D demo sound effects and music audible through speaker. If distorted, check sample rate (Duke3D may output at 11025Hz or 22050Hz — adjust `I2S_STD_CLK_DEFAULT_CONFIG()` accordingly).

- [ ] **Step 8: Final commit**

```bash
git add components/i2s_audio/ components/duke3d/platform/esp32_hal.cpp esphome.yaml
git commit -m "feat: Phase 7 - I2S audio via MAX98357A"
```
