#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "bsp/esp-bsp.h"
#include "lvgl.h"


#include <math.h>
#include "driver/i2s_std.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"

// Audio configuration
#define SAMPLE_RATE     16000
#define BEEP_FREQ_HZ    1000
#define BEEP_DURATION   50 // milliseconds

// We will use the Codec Device handle provided by the BSP
static esp_codec_dev_handle_t spk_codec_dev = NULL;

void play_beep(void)
{
    if (!spk_codec_dev) return; // Guard in case audio isn't initialized

    // Calculate how many samples we need for a 50ms beep
    size_t num_samples = (SAMPLE_RATE * BEEP_DURATION) / 1000;
    int16_t *audio_buffer = malloc(num_samples * sizeof(int16_t));

    if (!audio_buffer) return;

    // Generate a simple square wave at 1000Hz
    int half_period = SAMPLE_RATE / BEEP_FREQ_HZ / 2;
    for (size_t i = 0; i < num_samples; i++) {
        // Alternate between high and low amplitude
        audio_buffer[i] = ((i / half_period) % 2) ? 10000 : -10000;
    }

    // Push the generated audio to the Codec
    esp_codec_dev_write(spk_codec_dev, audio_buffer, num_samples * sizeof(int16_t));

    free(audio_buffer);}

// Hardware specific definitions
#define LCD_H_RES 720
#define LCD_V_RES 720

static void volume_slider_event_cb(lv_event_t * e)
{
    // Grab the slider object that triggered the event
    lv_obj_t * slider = lv_event_get_target(e);

    // Get the current value of the slider (0-100)
    int volume = lv_slider_get_value(slider);

    // If our audio codec is initialized, update the hardware volume!
    if (spk_codec_dev) {
        esp_codec_dev_set_out_vol(spk_codec_dev, volume);
        printf("Volume set to: %d%%\n", volume);
    }
}

// ---------------------------------------------------------------------
// TOUCH EVENT HANDLER
// ---------------------------------------------------------------------
static void screen_touch_event_cb(lv_event_t * e)
{
    // Get the specific event type
    lv_event_code_t code = lv_event_get_code(e);

    if (code == LV_EVENT_PRESSED) {
        play_beep(); // Play a beep sound when the user first touches the screen
    }

    // We want to trigger when the user first touches AND when they drag their finger
    if(code == LV_EVENT_PRESSED || code == LV_EVENT_PRESSING) {

        // Retrieve the circle object we passed in via user_data
        lv_obj_t * circle = lv_event_get_user_data(e);

        // Get the active input device (the touch screen)
        lv_indev_t * indev = lv_indev_active();
        if(!indev) return;

        // Get the current touch X and Y coordinates
        lv_point_t p;
        lv_indev_get_point(indev, &p);

        // Clamp the values just in case hardware reports slightly out of bounds
        int32_t x = p.x < 0 ? 0 : (p.x >= LCD_H_RES ? LCD_H_RES - 1 : p.x);
        int32_t y = p.y < 0 ? 0 : (p.y >= LCD_V_RES ? LCD_V_RES - 1 : p.y);

        // Map X (0-719) to Hue (0-359 degrees on the color wheel)
        uint16_t hue = (x * 360) / LCD_H_RES;

        // Map Y (0-719) to Saturation (0-100%). Top of screen = 100% vivid, Bottom = 0% (white)
        uint8_t sat = 100 - ((y * 100) / LCD_V_RES);

        // Create the new color using HSV (Hue, Saturation, Value) and apply it to the circle
        lv_color_t touch_color = lv_color_hsv_to_rgb(hue, sat, 100);
        lv_obj_set_style_bg_color(circle, touch_color, 0);
    }
}

// ---------------------------------------------------------------------
// UI SETUP
// ---------------------------------------------------------------------
void create_circle_ui(void)
{
lv_obj_t * scr = lv_screen_active();

    // 1. Create the Circle (same as before)
    lv_obj_t * circle = lv_obj_create(scr);
    lv_obj_set_size(circle, 300, 300);
    lv_obj_align(circle, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_radius(circle, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(circle, lv_palette_main(LV_PALETTE_RED), 0);
    lv_obj_set_style_border_width(circle, 0, 0);

    // Attach the background color-changing event
    lv_obj_add_flag(scr, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(scr, screen_touch_event_cb, LV_EVENT_ALL, circle);

    // 2. Create the Volume Slider
    lv_obj_t * slider = lv_slider_create(scr);

    // Make it nice and wide, but thin
    lv_obj_set_size(slider, 400, 20);

    // Position it at the bottom middle, 50 pixels up from the very bottom edge
    lv_obj_align(slider, LV_ALIGN_BOTTOM_MID, 0, -50);

    // Set the volume range (0 to 100)
    lv_slider_set_range(slider, 0, 100);

    // Set the default slider position to match our default audio init volume (70%)
    lv_slider_set_value(slider, 70, LV_ANIM_OFF);

    // Attach our volume callback to trigger whenever the value changes
    lv_obj_add_event_cb(slider, volume_slider_event_cb, LV_EVENT_VALUE_CHANGED, NULL);
}

void app_main(void)
{
    printf("Starting ESP32-P4 LVGL Touch Application...\n");

    // Initialize MIPI DSI, LCD, Touch, PSRAM, and LVGL tasks
    bsp_display_start();
    bsp_display_backlight_on();

    // 1. Initialize the I2S bus (passing NULL uses standard 16-bit, 16kHz settings)
    if (bsp_audio_init(NULL) == ESP_OK) {

        // 2. Initialize the specific ES8311 codec chip via I2C and get a handle
        spk_codec_dev = bsp_audio_codec_speaker_init();

        if (spk_codec_dev) {
            // 3. Configure the codec for our beep's sample rate and set the volume
            esp_codec_dev_sample_info_t fs = {
                .sample_rate = SAMPLE_RATE,
                .channel = 1,
                .bits_per_sample = 16,
            };
            esp_codec_dev_open(spk_codec_dev, &fs);
            esp_codec_dev_set_out_vol(spk_codec_dev, 70); // Set volume to 70%
            printf("Audio Codec Initialized Successfully!\n");
        } else {
            printf("WARNING: Codec handle is NULL.\n");
        }
    } else {
        printf("WARNING: I2S Bus failed to initialize.\n");
    }

    // Lock the display/LVGL port before interacting with the UI
    bsp_display_lock(0);
    create_circle_ui();
    bsp_display_unlock();

    printf("UI Setup Complete. Try touching and dragging on the screen!\n");
}
