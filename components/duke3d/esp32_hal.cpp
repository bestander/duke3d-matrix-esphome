#include "esp32_hal.h"
#include "renderer.h"
#include "esphome/components/hub75_matrix/hub75_matrix.h"
#include "esphome/components/hud/hud.h"
#include "esphome/components/sd_card/sd_card.h"
#include "esphome/components/i2s_audio/i2s_audio.h"
#include "esp_task_wdt.h"

// global_hud is defined in duke3d_component.cpp and set during Duke3DComponent::setup().
// It is declared extern here so platform_blit_frame can overlay the HUD each frame.
extern esphome::hud::Hud* global_hud;

void platform_blit_frame(const uint8_t* src, const uint8_t* pal) {
    auto* m = esphome::hub75_matrix::global_hub75;
    if (!m) return;

    render_frame(*m, src, pal);      // writes rows 0-39

    if (global_hud) global_hud->render(*m);  // overlays rows 40-63

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

void platform_audio_write(const int16_t* pcm, int n) {
    auto* audio = esphome::i2s_audio::global_i2s;
    // n is int16_t sample count (both channels). write_pcm expects bytes.
    if (audio) audio->write_pcm(pcm, n * sizeof(int16_t));
}
