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
// AUDIO FUNCTIONS
// ---------------------------------------------------------------------
void play_beep(void)
{
    if (!spk_codec_dev) return;
    size_t num_samples = (SAMPLE_RATE * BEEP_DURATION) / 1000;
    int16_t *audio_buffer = malloc(num_samples * sizeof(int16_t));
    if (!audio_buffer) return;

    int half_period = SAMPLE_RATE / BEEP_FREQ_HZ / 2;
    for (size_t i = 0; i < num_samples; i++) {
        audio_buffer[i] = ((i / half_period) % 2) ? 10000 : -10000;
    }
    esp_codec_dev_write(spk_codec_dev, audio_buffer, num_samples * sizeof(int16_t));
    free(audio_buffer);
}

void play_boop(void)
{
    if (!spk_codec_dev) return;
    size_t num_samples = (SAMPLE_RATE * BOOP_DURATION) / 1000;
    int16_t *audio_buffer = malloc(num_samples * sizeof(int16_t));
    if (!audio_buffer) return;

    int half_period = SAMPLE_RATE / BOOP_FREQ_HZ / 2;
    for (size_t i = 0; i < num_samples; i++) {
        audio_buffer[i] = ((i / half_period) % 2) ? 10000 : -10000;
    }
    esp_codec_dev_write(spk_codec_dev, audio_buffer, num_samples * sizeof(int16_t));
    free(audio_buffer);
}

// ---------------------------------------------------------------------
// LVGL CALLBACKS
// ---------------------------------------------------------------------

// 1. Rainbow Background Timer
static void rainbow_bg_timer_cb(lv_timer_t * timer)
{
    lv_obj_t * scr = lv_timer_get_user_data(timer);
    static uint16_t bg_hue = 0;

    bg_hue++;
    if (bg_hue >= 360) bg_hue = 0;

    lv_color_t bg_color = lv_color_hsv_to_rgb(bg_hue, 50, 30);
    lv_obj_set_style_bg_color(scr, bg_color, 0);
}

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

// 3. Slider Volume Control
static void volume_slider_event_cb(lv_event_t * e)
{
    lv_obj_t * slider = lv_event_get_target(e);
    int volume = lv_slider_get_value(slider);

    if (spk_codec_dev) {
        esp_codec_dev_set_out_vol(spk_codec_dev, volume);
    }
}

// 4. Circle Boop Interaction
static void circle_touch_event_cb(lv_event_t * e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if(code == LV_EVENT_PRESSED) {
        play_boop();
    }
}

// 5. Background Touch Interaction (Color Picker & Beep)
static void screen_touch_event_cb(lv_event_t * e)
{
    lv_event_code_t code = lv_event_get_code(e);

    if(code == LV_EVENT_PRESSED) {
        play_beep();
    }

    if(code == LV_EVENT_PRESSED || code == LV_EVENT_PRESSING) {
        lv_obj_t * circle = lv_event_get_user_data(e);
        lv_indev_t * indev = lv_indev_active();
        if(!indev) return;

        lv_point_t p;
        lv_indev_get_point(indev, &p);

        int32_t x = p.x < 0 ? 0 : (p.x >= LCD_H_RES ? LCD_H_RES - 1 : p.x);
        int32_t y = p.y < 0 ? 0 : (p.y >= LCD_V_RES ? LCD_V_RES - 1 : p.y);

        uint16_t hue = (x * 360) / LCD_H_RES;
        uint8_t sat = 100 - ((y * 100) / LCD_V_RES);

        lv_color_t touch_color = lv_color_hsv_to_rgb(hue, sat, 100);
        lv_obj_set_style_bg_color(circle, touch_color, 0);
    }
}

// ---------------------------------------------------------------------
// UI SETUP
// ---------------------------------------------------------------------
void create_interactive_ui(void)
{
    lv_obj_t * scr = lv_screen_active();
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_timer_create(rainbow_bg_timer_cb, 50, scr);

    // Circle
    lv_obj_t * circle = lv_obj_create(scr);
    lv_obj_set_size(circle, 300, 300);
    lv_obj_align(circle, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_radius(circle, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(circle, lv_palette_main(LV_PALETTE_RED), 0);
    lv_obj_set_style_border_width(circle, 0, 0);
    lv_obj_add_flag(circle, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(circle, circle_touch_event_cb, LV_EVENT_ALL, NULL);

    // Background Interaction
    lv_obj_add_flag(scr, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(scr, screen_touch_event_cb, LV_EVENT_ALL, circle);

    // Volume Slider
    lv_obj_t * slider = lv_slider_create(scr);
    lv_obj_set_size(slider, 400, 20);
    lv_obj_align(slider, LV_ALIGN_BOTTOM_MID, 0, -50);
    lv_slider_set_range(slider, 0, 100);
    lv_slider_set_value(slider, 70, LV_ANIM_OFF);
    lv_obj_add_event_cb(slider, volume_slider_event_cb, LV_EVENT_VALUE_CHANGED, NULL);

    // Clock Label
    time_label = lv_label_create(scr);
    lv_obj_set_style_text_font(time_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(time_label, lv_color_white(), 0);
    lv_obj_align(time_label, LV_ALIGN_TOP_MID, 0, 40);
    lv_label_set_text(time_label, "Waiting for Wi-Fi...");
    lv_timer_create(update_time_cb, 1000, NULL);
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

    // 5. Build the UI
    bsp_display_lock(0);
    create_interactive_ui();
    bsp_display_unlock();
}
