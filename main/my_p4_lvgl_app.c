#include <stdio.h>
#include <time.h>
#include <sys/time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "bsp/esp-bsp.h"
#include "lvgl.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"

// Networking & Time Includes
#include "nvs_flash.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_sntp.h"

// Check if the secrets file exists before trying to include it
#if __has_include("secrets.h")
    #include "secrets.h"
#else
    // If it's missing, stop the build and print this exact message to the terminal!
    #error "Missing 'secrets.h'! Please copy 'main/secrets.h.example' to 'main/secrets.h' and enter your Wi-Fi credentials."
#endif
// ---------------------------------------------------------------------
// CONFIGURATION
// ---------------------------------------------------------------------
#define LCD_H_RES       720
#define LCD_V_RES       720

#define SAMPLE_RATE     16000
#define BEEP_FREQ_HZ    1000
#define BEEP_DURATION   50
#define BOOP_FREQ_HZ    400
#define BOOP_DURATION   100

// ---------------------------------------------------------------------
// GLOBALS
// ---------------------------------------------------------------------
static esp_codec_dev_handle_t spk_codec_dev = NULL;
static lv_obj_t * time_label;

// ---------------------------------------------------------------------
// SYNTHESIS & AUDIO (POLYPHONIC + ADSR)
// ---------------------------------------------------------------------
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define MAX_VOICES 5

typedef enum {
    ENV_IDLE = 0,
    ENV_ATTACK,
    ENV_DECAY,
    ENV_SUSTAIN,
    ENV_RELEASE
} env_state_t;

typedef struct {
    float phase;
    float freq;
    int note_idx;
    env_state_t env_state;
    float env_val;
} voice_t;

static voice_t voices[MAX_VOICES];

volatile int synth_waveform = 1; // 0=Sine, 1=Square, 2=Saw
volatile float synth_volume = 0.4f;

// ADSR parameters
volatile float env_a_time = 0.1f;  // 0.01 to 2.0s
volatile float env_d_time = 0.1f;  // 0.01 to 2.0s
volatile float env_s_level = 0.5f; // 0.0 to 1.0
volatile float env_r_time = 0.3f;  // 0.01 to 2.0s

static void note_on(int note_idx, float freq) {
    int target_v = -1;
    // Try to find a free voice
    for (int v = 0; v < MAX_VOICES; v++) {
        if (voices[v].env_state == ENV_IDLE) {
            target_v = v; break;
        }
    }
    // If none free, find one in RELEASE
    if (target_v == -1) {
        for (int v = 0; v < MAX_VOICES; v++) {
            if (voices[v].env_state == ENV_RELEASE) {
                target_v = v; break;
            }
        }
    }
    // If still none, steal voice 0 (or oldest, but keep simple)
    if (target_v == -1) target_v = 0;

    voices[target_v].freq = freq;
    voices[target_v].note_idx = note_idx;
    voices[target_v].env_state = ENV_ATTACK;
    voices[target_v].phase = 0.0f;
    // Don't reset env_val to 0 to avoid clicking, let it rise from current
}

static void note_off(int note_idx) {
    for (int v = 0; v < MAX_VOICES; v++) {
        if (voices[v].note_idx == note_idx &&
            voices[v].env_state != ENV_IDLE &&
            voices[v].env_state != ENV_RELEASE) {
            voices[v].env_state = ENV_RELEASE;
        }
    }
}

static void audio_task(void *pvParameters)
{
    size_t num_samples = 256;
    int16_t *audio_buffer = malloc(num_samples * sizeof(int16_t));
    float sample_rate_f = (float)SAMPLE_RATE;

    while (1) {
        if (!spk_codec_dev) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        int wav_local = synth_waveform;
        float vol_local = synth_volume;
        float a_time = env_a_time < 0.01f ? 0.01f : env_a_time;
        float d_time = env_d_time < 0.01f ? 0.01f : env_d_time;
        float s_lvl = env_s_level < 0.01f ? 0.01f : env_s_level;
        float r_time = env_r_time < 0.01f ? 0.01f : env_r_time;

        float a_rate = 1.0f / (a_time * sample_rate_f);
        float d_rate = (1.0f - s_lvl) / (d_time * sample_rate_f);
        float r_rate = s_lvl / (r_time * sample_rate_f);

        for (size_t i = 0; i < num_samples; i++) {
            float mixed_sample = 0.0f;

            for (int v = 0; v < MAX_VOICES; v++) {
                if (voices[v].env_state == ENV_IDLE) continue;

                // Process Envelope
                switch(voices[v].env_state) {
                    case ENV_ATTACK:
                        voices[v].env_val += a_rate;
                        if (voices[v].env_val >= 1.0f) {
                            voices[v].env_val = 1.0f;
                            voices[v].env_state = ENV_DECAY;
                        }
                        break;
                    case ENV_DECAY:
                        voices[v].env_val -= d_rate;
                        if (voices[v].env_val <= s_lvl) {
                            voices[v].env_val = s_lvl;
                            voices[v].env_state = ENV_SUSTAIN;
                        }
                        break;
                    case ENV_SUSTAIN:
                        // Hold level
                        voices[v].env_val = s_lvl;
                        break;
                    case ENV_RELEASE:
                        // Release from current envelope value
                        voices[v].env_val -= r_rate;
                        if (voices[v].env_val <= 0.0f) {
                            voices[v].env_val = 0.0f;
                            voices[v].env_state = ENV_IDLE;
                        }
                        break;
                    default: break;
                }

                if(voices[v].env_state == ENV_IDLE) continue;

                // Oscillator
                float sample_p = 0.0f;
                if (wav_local == 0) { // Sine
                    sample_p = sinf(2.0f * (float)M_PI * voices[v].phase);
                } else if (wav_local == 1) { // Square
                    sample_p = (voices[v].phase < 0.5f) ? 1.0f : -1.0f;
                } else if (wav_local == 2) { // Sawtooth
                    sample_p = 2.0f * voices[v].phase - 1.0f;
                }

                mixed_sample += sample_p * voices[v].env_val;

                // Advance phase
                voices[v].phase += voices[v].freq / sample_rate_f;
                if (voices[v].phase >= 1.0f) voices[v].phase -= 1.0f;
            }

            // Mix down and apply volume
            mixed_sample *= vol_local / 2.0f; // Soften to prevent clipping when multiple notes play

            // Hard clip to [-1.0, 1.0]
            if(mixed_sample > 1.0f) mixed_sample = 1.0f;
            else if(mixed_sample < -1.0f) mixed_sample = -1.0f;

            audio_buffer[i] = (int16_t)(mixed_sample * 32767.0f);
        }

        esp_codec_dev_write(spk_codec_dev, audio_buffer, num_samples * sizeof(int16_t));
    }
}

// ---------------------------------------------------------------------
// LVGL CALLBACKS
// ---------------------------------------------------------------------

// 2. Clock Update Timer
static void update_time_cb(lv_timer_t * timer)
{
    time_t now;
    struct tm timeinfo;
    time(&now);
    localtime_r(&now, &timeinfo);

    // If year is > 100, we are past the year 2000 and NTP has synced
    if (timeinfo.tm_year > 100) {
        lv_label_set_text_fmt(time_label, "%02d:%02d:%02d",
                              timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
    } else {
        lv_label_set_text(time_label, "Waiting for Wi-Fi...");
    }
}

// Predefined frequencies for single octave C4-C5
static const float note_freqs[] = {
    261.63f, // 0: C4
    277.18f, // 1: C#4
    293.66f, // 2: D4
    311.13f, // 3: D#4
    329.63f, // 4: E4
    349.23f, // 5: F4
    369.99f, // 6: F#4
    392.00f, // 7: G4
    415.30f, // 8: G#4
    440.00f, // 9: A4
    466.16f, // 10: A#4
    493.88f, // 11: B4
    523.25f  // 12: C5
};

static void key_event_cb(lv_event_t * e)
{
    lv_event_code_t code = lv_event_get_code(e);
    int note_idx = (int)(intptr_t)lv_event_get_user_data(e);

    if (code == LV_EVENT_PRESSED) {
        note_on(note_idx, note_freqs[note_idx]);
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        note_off(note_idx);
    }
}

static void wave_dropdown_event_cb(lv_event_t * e)
{
    lv_obj_t * dropdown = lv_event_get_target(e);
    synth_waveform = lv_dropdown_get_selected(dropdown);
}

static void vol_slider_event_cb(lv_event_t * e)
{
    lv_obj_t * slider = lv_event_get_target(e);
    synth_volume = lv_slider_get_value(slider) / 100.0f;
}

static void env_slider_event_cb(lv_event_t * e)
{
    lv_obj_t * slider = lv_event_get_target(e);
    int type = (int)(intptr_t)lv_event_get_user_data(e);
    float val = (float)lv_slider_get_value(slider);

    switch(type) {
        case 0: env_a_time = val / 100.0f; break; // 0.0 to 1.0s (or 2.0s if map to 200)
        case 1: env_d_time = val / 100.0f; break;
        case 2: env_s_level = val / 100.0f; break; // 0.0 to 1.0 multiplier
        case 3: env_r_time = val / 100.0f; break;
    }
}

// ---------------------------------------------------------------------
// UI SETUP
// ---------------------------------------------------------------------
void create_synth_ui(void)
{
    lv_obj_t * scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x222222), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    // Header container
    lv_obj_t * header = lv_obj_create(scr);
    lv_obj_set_size(header, LCD_H_RES, 60);
    lv_obj_align(header, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(header, lv_color_hex(0x111111), 0);
    lv_obj_set_style_border_width(header, 0, 0);

    time_label = lv_label_create(header);
    lv_obj_set_style_text_font(time_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(time_label, lv_color_white(), 0);
    lv_obj_align(time_label, LV_ALIGN_LEFT_MID, 10, 0);
    lv_label_set_text(time_label, "Waiting for Wi-Fi...");
    lv_timer_create(update_time_cb, 1000, NULL);

    lv_obj_t * title_label = lv_label_create(header);
    lv_obj_set_style_text_font(title_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(title_label, lv_palette_main(LV_PALETTE_AMBER), 0);
    lv_obj_align(title_label, LV_ALIGN_RIGHT_MID, -20, 0);
    lv_label_set_text(title_label, "P4 NanoSynth");

    // Controls container
    lv_obj_t * controls = lv_obj_create(scr);
    lv_obj_set_size(controls, LCD_H_RES - 40, 200);
    lv_obj_align(controls, LV_ALIGN_TOP_MID, 0, 80);
    lv_obj_set_flex_flow(controls, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(controls, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_bg_color(controls, lv_color_hex(0x333333), 0);
    lv_obj_set_style_border_width(controls, 0, 0);

    // Waveform dropdown
    lv_obj_t * wave_cont = lv_obj_create(controls);
    lv_obj_set_size(wave_cont, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(wave_cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_bg_opa(wave_cont, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(wave_cont, 0, 0);

    lv_obj_t * wave_label = lv_label_create(wave_cont);
    lv_obj_set_style_text_color(wave_label, lv_color_white(), 0);
    lv_label_set_text(wave_label, "Waveform");

    lv_obj_t * wave_dd = lv_dropdown_create(wave_cont);
    lv_dropdown_set_options(wave_dd, "Sine\nSquare\nSawtooth");
    lv_dropdown_set_selected(wave_dd, 1);
    lv_obj_add_event_cb(wave_dd, wave_dropdown_event_cb, LV_EVENT_VALUE_CHANGED, NULL);

    // ADSR and Volume Sliders
    const char* sl_labels[] = {"A", "D", "S", "R", "Vol"};
    int sl_initials[] = {10, 10, 50, 30, 40}; // mapped to 0-100

    for (int s = 0; s < 5; s++) {
        lv_obj_t * s_cont = lv_obj_create(controls);
        lv_obj_set_size(s_cont, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(s_cont, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(s_cont, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_bg_opa(s_cont, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(s_cont, 0, 0);
        lv_obj_set_style_pad_all(s_cont, 0, 0);

        lv_obj_t * sl = lv_slider_create(s_cont);
        lv_obj_set_size(sl, 20, 120); // vertical slider
        lv_slider_set_range(sl, 0, 100);
        lv_slider_set_value(sl, sl_initials[s], LV_ANIM_OFF);

        if (s < 4) {
            lv_obj_add_event_cb(sl, env_slider_event_cb, LV_EVENT_VALUE_CHANGED, (void*)(intptr_t)s);
        } else {
            lv_obj_add_event_cb(sl, vol_slider_event_cb, LV_EVENT_VALUE_CHANGED, NULL);
        }

        lv_obj_t * lbl = lv_label_create(s_cont);
        lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
        lv_label_set_text(lbl, sl_labels[s]);
    }

    // Keyboard container
    lv_obj_t * kb_cont = lv_obj_create(scr);
    int kb_w = LCD_H_RES - 40;
    int kb_h = 250;
    lv_obj_set_size(kb_cont, kb_w, kb_h);
    lv_obj_align(kb_cont, LV_ALIGN_BOTTOM_MID, 0, -40);
    lv_obj_remove_flag(kb_cont, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(kb_cont, lv_color_hex(0x111111), 0);
    lv_obj_set_style_border_width(kb_cont, 0, 0);
    lv_obj_set_style_pad_all(kb_cont, 5, 0);

    const int num_white_keys = 8;
    int white_key_w = (kb_w - 10) / num_white_keys;
    int white_key_h = kb_h - 10;
    int black_key_w = (int)(white_key_w * 0.55f);
    int black_key_h = (int)(white_key_h * 0.6f);

    int white_note_idx[] = {0, 2, 4, 5, 7, 9, 11, 12};

    // Create white keys
    for(int i = 0; i < num_white_keys; i++) {
        lv_obj_t * key = lv_btn_create(kb_cont);
        lv_obj_set_size(key, white_key_w - 4, white_key_h);
        lv_obj_set_pos(key, i * white_key_w, 0);
        lv_obj_set_style_bg_color(key, lv_color_white(), 0);
        lv_obj_set_style_bg_color(key, lv_color_hex(0xcccccc), LV_STATE_PRESSED);
        lv_obj_set_style_radius(key, 4, 0);
        lv_obj_add_event_cb(key, key_event_cb, LV_EVENT_ALL, (void*)(intptr_t)white_note_idx[i]);
    }

    int black_note_idx[] = {1, 3, -1, 6, 8, 10, -1};
    for(int i = 0; i < 7; i++) {
        if(black_note_idx[i] != -1) {
            lv_obj_t * key = lv_btn_create(kb_cont);
            lv_obj_set_size(key, black_key_w, black_key_h);
            lv_obj_set_pos(key, (i + 1) * white_key_w - (black_key_w / 2), 0);
            lv_obj_set_style_bg_color(key, lv_color_black(), 0);
            lv_obj_set_style_bg_color(key, lv_color_hex(0x444444), LV_STATE_PRESSED);
            lv_obj_set_style_border_color(key, lv_color_hex(0x333333), 0);
            lv_obj_set_style_border_width(key, 2, 0);
            lv_obj_set_style_radius(key, 2, 0);
            lv_obj_add_event_cb(key, key_event_cb, LV_EVENT_ALL, (void*)(intptr_t)black_note_idx[i]);
            lv_obj_move_foreground(key);
        }
    }
}

// ---------------------------------------------------------------------
// MAIN APPLICATION
// ---------------------------------------------------------------------
void app_main(void)
{
    printf("Starting ESP32-P4 Ultimate UI Application...\n");

    // 1. Initialize NVS (Required for Wi-Fi data storage)
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // 2. Hardware (Display & Audio)
    bsp_display_start();
    bsp_display_backlight_on();

    if (bsp_audio_init(NULL) == ESP_OK) {
        spk_codec_dev = bsp_audio_codec_speaker_init();
        if (spk_codec_dev) {
            esp_codec_dev_sample_info_t fs = {
                .sample_rate = SAMPLE_RATE,
                .channel = 1,
                .bits_per_sample = 16,
            };
            esp_codec_dev_open(spk_codec_dev, &fs);
            esp_codec_dev_set_out_vol(spk_codec_dev, 70);
        }
    }

    // 3. Wi-Fi Initialization (Over SDIO to the C6)
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASS,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_connect());

    // 4. NTP Setup
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_init();
    // Central European Time. Change if you are elsewhere!
    setenv("TZ", "CET-1CEST,M3.5.0,M10.5.0/3", 1);
    tzset();

    // 5. Start Audio Task
    xTaskCreate(audio_task, "audio_task", 4096, NULL, 5, NULL);

    // 6. Build the UI
    bsp_display_lock(0);
    create_synth_ui();
    bsp_display_unlock();
}
