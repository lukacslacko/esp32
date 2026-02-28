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
// SYNTHESIS & AUDIO
// ---------------------------------------------------------------------
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

volatile float synth_freq = 0.0f;
volatile int synth_waveform = 1; // 0=Sine, 1=Square, 2=Saw
volatile float synth_volume = 0.4f;
volatile bool note_playing = false;

static void audio_task(void *pvParameters)
{
    size_t num_samples = 256;
    int16_t *audio_buffer = malloc(num_samples * sizeof(int16_t));
    float phase = 0.0f;

    while (1) {
        if (!spk_codec_dev) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        float current_freq_local = synth_freq;
        int wav_local = synth_waveform;
        bool playing_local = note_playing;
        float vol_local = synth_volume;

        if (playing_local && current_freq_local > 0) {
            float phase_inc = current_freq_local / SAMPLE_RATE;
            for (size_t i = 0; i < num_samples; i++) {
                float sample_p = 0.0f;
                if (wav_local == 0) { // Sine
                    sample_p = sinf(2.0f * (float)M_PI * phase);
                } else if (wav_local == 1) { // Square
                    sample_p = (phase < 0.5f) ? 1.0f : -1.0f;
                } else if (wav_local == 2) { // Sawtooth
                    sample_p = 2.0f * phase - 1.0f;
                }

                audio_buffer[i] = (int16_t)(sample_p * 32767.0f * vol_local);

                phase += phase_inc;
                if (phase >= 1.0f) phase -= 1.0f;
            }
        } else {
            for (size_t i = 0; i < num_samples; i++) {
                audio_buffer[i] = 0;
            }
            phase = 0.0f;
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
        synth_freq = note_freqs[note_idx];
        note_playing = true;
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        if (synth_freq == note_freqs[note_idx]) {
            note_playing = false;
        }
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

    // Master volume slider
    lv_obj_t * vol_cont = lv_obj_create(controls);
    lv_obj_set_size(vol_cont, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(vol_cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_bg_opa(vol_cont, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(vol_cont, 0, 0);

    lv_obj_t * vol_label = lv_label_create(vol_cont);
    lv_obj_set_style_text_color(vol_label, lv_color_white(), 0);
    lv_label_set_text(vol_label, "Volume");

    lv_obj_t * vol_slider = lv_slider_create(vol_cont);
    lv_obj_set_size(vol_slider, 200, 20);
    lv_slider_set_range(vol_slider, 0, 100);
    lv_slider_set_value(vol_slider, 40, LV_ANIM_OFF);
    lv_obj_add_event_cb(vol_slider, vol_slider_event_cb, LV_EVENT_VALUE_CHANGED, NULL);

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
