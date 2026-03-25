#include "duke3d_component.h"
#include "esphome/core/log.h"
#include "esp_task_wdt.h"
#include "esphome/components/sd_card/sd_card.h"
#include "esphome/components/hud/hud.h"
#include "input.h"
#include <cstring>

// Duke3D engine entry point — provided by jkirsons/Duke3D engine.
// NOTE: After adding the engine submodule (Task 5.1), verify the actual
// entry point name and signature from engine/main/. Adjust if needed.
#ifndef DUKE3D_ENGINE_PRESENT
// Stub — remove once the engine submodule is wired in (Task 5.1).
extern "C" int duke3d_main(int /*argc*/, char** /*argv*/) { return 0; }
#else
extern "C" int duke3d_main(int argc, char** argv);
#endif

// Forward-declare the HUD instance pointer from the hud component.
// Defined in hud.cpp (esphome::hud namespace); accessed in setup() below.
namespace esphome { namespace hud { extern Hud* global_hud_instance; } }

// global_hud: wires the HUD into platform_blit_frame() for per-frame overlay.
// Declared extern in esp32_hal.cpp; defined here.
esphome::hud::Hud* global_hud = nullptr;

namespace esphome {
namespace duke3d {

static const char* TAG = "duke3d";
static Duke3DComponent* instance_ = nullptr;

void Duke3DComponent::setup() {
    auto* sd = sd_card::global_sd_card;
    if (!sd || !sd->grp_present()) {
        ESP_LOGE(TAG, "DUKE3D.GRP not found on SD card — halting game component");
        mark_failed();
        return;
    }

    // Wire HUD global and disable Hud::loop()'s own swap_buffers() call.
    // From now on platform_blit_frame() drives both render and swap.
    // Access via fully qualified name — local extern inside duke3d namespace would
    // resolve to esphome::duke3d::global_hud_instance instead of esphome::hud::.
    global_hud = esphome::hud::global_hud_instance;
    if (global_hud) global_hud->set_game_running(true);

    instance_ = this;
    input_init();

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

    // Duke3D demo loop: play demos in order, looping forever.
    // The engine exits demo playback at the end of each demo;
    // we restart it in a loop.
    const char* demos[] = { "DEMO1.DMO", "DEMO2.DMO", "DEMO3.DMO" };
    int demo_idx = 0;

    while (true) {
        strncpy(self->current_demo_, demos[demo_idx], sizeof(self->current_demo_) - 1);
        ESP_LOGI(TAG, "Playing %s", self->current_demo_);

        // Engine invocation — passes demo filename via argv.
        // NOTE: Verify the exact argv format from jkirsons/Duke3D's main() after
        // adding the submodule in Task 5.1. Adjust -playdem flag if needed.
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
