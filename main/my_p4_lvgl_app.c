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

// BMP280 Sensor
#define BMP280_I2C_ADDR      0x77
#define BMP280_REG_CHIP_ID   0xD0
#define BMP280_REG_RESET     0xE0
#define BMP280_REG_CTRL_MEAS 0xF4
#define BMP280_REG_CONFIG    0xF5
#define BMP280_REG_PRESS_MSB 0xF7
#define BMP280_REG_CALIB00   0x88

// ---------------------------------------------------------------------
// GLOBALS
// ---------------------------------------------------------------------
static esp_codec_dev_handle_t spk_codec_dev = NULL;
static esp_codec_dev_handle_t mic_codec_dev = NULL;

static lv_obj_t * time_label_synth;
static lv_obj_t * time_label_menu;
static lv_obj_t * time_label_record;

static lv_obj_t * main_menu_scr;
static lv_obj_t * synth_scr;
static lv_obj_t * clock_scr;
static lv_obj_t * record_scr;
static lv_obj_t * record_canvas = NULL;
static uint8_t * record_canvas_raw_buf = NULL;
static uint8_t * record_canvas_aligned_buf = NULL;

static lv_obj_t * clock_hour_hand;
static lv_obj_t * clock_min_hand;
static lv_obj_t * clock_sec_hand;

// BMP280 sensor state
static i2c_master_dev_handle_t bmp280_dev = NULL;
static volatile float bmp280_temperature  = 0.0f;
static volatile float bmp280_pressure     = 0.0f;
static volatile bool  bmp280_ok           = false;

// BMP280 calibration registers
static uint16_t bmp280_dig_T1;
static int16_t  bmp280_dig_T2, bmp280_dig_T3;
static uint16_t bmp280_dig_P1;
static int16_t  bmp280_dig_P2, bmp280_dig_P3, bmp280_dig_P4;
static int16_t  bmp280_dig_P5, bmp280_dig_P6, bmp280_dig_P7;
static int16_t  bmp280_dig_P8, bmp280_dig_P9;
static int32_t  bmp280_t_fine;

// Weather screen widgets
static lv_obj_t * weather_scr         = NULL;
static lv_obj_t * weather_temp_label  = NULL;
static lv_obj_t * weather_press_label = NULL;
static lv_obj_t * weather_status_label= NULL;
static lv_obj_t * time_label_weather  = NULL;

// ---------------------------------------------------------------------
// SYNTHESIS & AUDIO & RECORDING
// ---------------------------------------------------------------------
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// Recording State
#define REC_MAX_SEC 5
#define REC_BUFFER_SAMPLES (SAMPLE_RATE * REC_MAX_SEC)
static int16_t * rec_buffer = NULL;
static volatile int rec_sample_count = 0;
static volatile bool is_recording = false;
static volatile bool is_playing_reverse = false;
static volatile int rec_play_idx = 0;
static volatile float rec_multiplier = 1.0f;

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
        if (is_recording) {
            if (mic_codec_dev && rec_buffer) {
                esp_codec_dev_read(mic_codec_dev, audio_buffer, num_samples * sizeof(int16_t));
                for(int i=0; i<num_samples; i++) {
                    if (rec_sample_count < REC_BUFFER_SAMPLES) {
                        rec_buffer[rec_sample_count++] = audio_buffer[i];
                    }
                }
            } else {
                vTaskDelay(pdMS_TO_TICKS(10));
            }
            continue;
        }

        if (is_playing_reverse) {
            if (!spk_codec_dev) {
                vTaskDelay(pdMS_TO_TICKS(10));
                continue;
            }
            for (size_t i = 0; i < num_samples; i++) {
                if (rec_play_idx >= 0 && rec_buffer) {
                    int32_t amplified_sample = (int32_t)(rec_buffer[rec_play_idx--] * rec_multiplier);
                    if (amplified_sample > 32767) amplified_sample = 32767;
                    else if (amplified_sample < -32768) amplified_sample = -32768;
                    audio_buffer[i] = (int16_t)amplified_sample;
                } else {
                    audio_buffer[i] = 0;
                    if (i == (num_samples - 1)) {
                        is_playing_reverse = false;
                    }
                }
            }
            esp_codec_dev_write(spk_codec_dev, audio_buffer, num_samples * sizeof(int16_t));
            continue;
        }

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
        if (time_label_synth) {
            lv_label_set_text_fmt(time_label_synth, "%02d:%02d:%02d", timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
        }
        if (time_label_menu) {
            lv_label_set_text_fmt(time_label_menu, "%02d:%02d:%02d", timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
        }
        if (time_label_record) {
            lv_label_set_text_fmt(time_label_record, "%02d:%02d:%02d", timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
        }
        if (time_label_weather) {
            lv_label_set_text_fmt(time_label_weather, "%02d:%02d:%02d", timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
        }

        // Update analog clock if created
        if (clock_sec_hand) {
            int sec_angle = timeinfo.tm_sec * 60; // 360/60 * 10 = 60 (lvgl uses 0.1 degree units)
            lv_obj_set_style_transform_rotation(clock_sec_hand, sec_angle, 0);
        }
        if (clock_min_hand) {
            int min_angle = timeinfo.tm_min * 60 + timeinfo.tm_sec;
            lv_obj_set_style_transform_rotation(clock_min_hand, min_angle, 0);
        }
        if (clock_hour_hand) {
            int hour_angle = (timeinfo.tm_hour % 12) * 300 + (timeinfo.tm_min * 5);
            lv_obj_set_style_transform_rotation(clock_hour_hand, hour_angle, 0);
        }
    } else {
        if (time_label_synth)   lv_label_set_text(time_label_synth,   "Waiting for Wi-Fi...");
        if (time_label_menu)    lv_label_set_text(time_label_menu,    "Waiting for Wi-Fi...");
        if (time_label_record)  lv_label_set_text(time_label_record,  "Waiting for Wi-Fi...");
        if (time_label_weather) lv_label_set_text(time_label_weather, "Waiting for Wi-Fi...");
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

// Screen Transition Callbacks
static void btn_go_synth_cb(lv_event_t * e) {
    lv_scr_load(synth_scr);
}

static void btn_go_clock_cb(lv_event_t * e) {
    lv_scr_load(clock_scr);
}

static void btn_go_record_cb(lv_event_t * e) {
    lv_scr_load(record_scr);
}

static void btn_go_menu_cb(lv_event_t * e) {
    lv_scr_load(main_menu_scr);
}

static void btn_go_weather_cb(lv_event_t * e) {
    lv_scr_load(weather_scr);
}

static lv_color_t get_heatmap_color(float intensity) {
    if (intensity < 0.0f) intensity = 0.0f;
    if (intensity > 1.0f) intensity = 1.0f;
    uint8_t r = 0, g = 0, b = 0;

    if (intensity < 0.25f) {
        float t = intensity / 0.25f;
        b = (uint8_t)(t * 255.0f);
    } else if (intensity < 0.5f) {
        float t = (intensity - 0.25f) / 0.25f;
        r = (uint8_t)(t * 255.0f);
        b = (uint8_t)((1.0f - t) * 255.0f);
    } else if (intensity < 0.75f) {
        float t = (intensity - 0.5f) / 0.25f;
        r = 255;
        g = (uint8_t)(t * 255.0f);
    } else {
        float t = (intensity - 0.75f) / 0.25f;
        r = 255;
        g = 255;
        b = (uint8_t)(t * 255.0f);
    }
    return lv_color_make(r, g, b);
}

static void compute_fft(float *vReal, float *vImag, uint16_t n) {
    uint16_t j = 0;
    for (uint16_t i = 0; i < n - 1; i++) {
        if (i < j) {
            float tempReal = vReal[i];
            float tempImag = vImag[i];
            vReal[i] = vReal[j];
            vImag[i] = vImag[j];
            vReal[j] = tempReal;
            vImag[j] = tempImag;
        }
        uint16_t k = n / 2;
        while (k <= j) {
            j -= k;
            k /= 2;
        }
        j += k;
    }
    for (uint16_t step = 1; step < n; step *= 2) {
        float arg = M_PI / step;
        float c = cosf(arg);
        float s = -sinf(arg);
        float uReal = 1.0f;
        float uImag = 0.0f;
        for (uint16_t j2 = 0; j2 < step; j2++) {
            for (uint16_t i = j2; i < n; i += 2 * step) {
                float tReal = uReal * vReal[i + step] - uImag * vImag[i + step];
                float tImag = uReal * vImag[i + step] + uImag * vReal[i + step];
                vReal[i + step] = vReal[i] - tReal;
                vImag[i + step] = vImag[i] - tImag;
                vReal[i] += tReal;
                vImag[i] += tImag;
            }
            float tempReal = uReal * c - uImag * s;
            uImag = uReal * s + uImag * c;
            uReal = tempReal;
        }
    }
}

// ---------------------------------------------------------------------
// BMP280 DRIVER
// ---------------------------------------------------------------------

static esp_err_t bmp280_read(uint8_t reg, uint8_t *buf, size_t len)
{
    return i2c_master_transmit_receive(bmp280_dev, &reg, 1, buf, len, 100);
}

static esp_err_t bmp280_write(uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = { reg, val };
    return i2c_master_transmit(bmp280_dev, buf, 2, 100);
}

// BMP280 compensation formulas (integer, from Bosch datasheet appendix)
static float bmp280_comp_temp(int32_t adc_T)
{
    int32_t var1 = ((((adc_T >> 3) - ((int32_t)bmp280_dig_T1 << 1))) * (int32_t)bmp280_dig_T2) >> 11;
    int32_t var2 = (((((adc_T >> 4) - (int32_t)bmp280_dig_T1) *
                      ((adc_T >> 4) - (int32_t)bmp280_dig_T1)) >> 12) *
                    (int32_t)bmp280_dig_T3) >> 14;
    bmp280_t_fine = var1 + var2;
    return (float)((bmp280_t_fine * 5 + 128) >> 8) / 100.0f;
}

static float bmp280_comp_press(int32_t adc_P)
{
    int64_t var1 = (int64_t)bmp280_t_fine - 128000;
    int64_t var2 = var1 * var1 * (int64_t)bmp280_dig_P6;
    var2 += (var1 * (int64_t)bmp280_dig_P5) << 17;
    var2 += (int64_t)bmp280_dig_P4 << 35;
    var1  = ((var1 * var1 * (int64_t)bmp280_dig_P3) >> 8) + ((var1 * (int64_t)bmp280_dig_P2) << 12);
    var1  = (((int64_t)1 << 47) + var1) * (int64_t)bmp280_dig_P1 >> 33;
    if (var1 == 0) return 0.0f;
    int64_t p = 1048576 - adc_P;
    p = (((p << 31) - var2) * 3125) / var1;
    var1 = ((int64_t)bmp280_dig_P9 * (p >> 13) * (p >> 13)) >> 25;
    var2 = ((int64_t)bmp280_dig_P8 * p) >> 19;
    p = ((p + var1 + var2) >> 8) + ((int64_t)bmp280_dig_P7 << 4);
    return (float)p / 25600.0f;   // hPa
}

static void bmp280_task(void *arg)
{
    i2c_master_bus_handle_t bus = bsp_i2c_get_handle();
    if (!bus) {
        printf("BMP280: I2C bus handle not available\n");
        vTaskDelete(NULL);
        return;
    }

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = BMP280_I2C_ADDR,
        .scl_speed_hz    = 400000,
    };
    if (i2c_master_bus_add_device(bus, &dev_cfg, &bmp280_dev) != ESP_OK) {
        printf("BMP280: Failed to add device to I2C bus\n");
        vTaskDelete(NULL);
        return;
    }

    // Verify chip ID (BMP280 = 0x58, BME280 = 0x60)
    uint8_t chip_id = 0;
    if (bmp280_read(BMP280_REG_CHIP_ID, &chip_id, 1) != ESP_OK || chip_id != 0x58) {
        printf("BMP280: Unexpected chip ID 0x%02X (expected 0x58) - check wiring & address\n", chip_id);
        vTaskDelete(NULL);
        return;
    }

    // Soft reset, then wait for the sensor to come back up
    bmp280_write(BMP280_REG_RESET, 0xB6);
    vTaskDelay(pdMS_TO_TICKS(15));

    // Read 24 bytes of calibration data starting at 0x88
    uint8_t calib[24];
    if (bmp280_read(BMP280_REG_CALIB00, calib, 24) != ESP_OK) {
        printf("BMP280: Failed to read calibration data\n");
        vTaskDelete(NULL);
        return;
    }
    bmp280_dig_T1 = (uint16_t)(calib[1]  << 8 | calib[0]);
    bmp280_dig_T2 = (int16_t) (calib[3]  << 8 | calib[2]);
    bmp280_dig_T3 = (int16_t) (calib[5]  << 8 | calib[4]);
    bmp280_dig_P1 = (uint16_t)(calib[7]  << 8 | calib[6]);
    bmp280_dig_P2 = (int16_t) (calib[9]  << 8 | calib[8]);
    bmp280_dig_P3 = (int16_t) (calib[11] << 8 | calib[10]);
    bmp280_dig_P4 = (int16_t) (calib[13] << 8 | calib[12]);
    bmp280_dig_P5 = (int16_t) (calib[15] << 8 | calib[14]);
    bmp280_dig_P6 = (int16_t) (calib[17] << 8 | calib[16]);
    bmp280_dig_P7 = (int16_t) (calib[19] << 8 | calib[18]);
    bmp280_dig_P8 = (int16_t) (calib[21] << 8 | calib[20]);
    bmp280_dig_P9 = (int16_t) (calib[23] << 8 | calib[22]);

    // Normal mode: osrs_t=x2 (010), osrs_p=x16 (101), mode=normal (11) -> 0101 0111 = 0x57
    bmp280_write(BMP280_REG_CTRL_MEAS, 0x57);
    // t_sb=1000ms (101), filter=x16 (100), spi3w=0 -> 1011 0000 = 0xB0
    bmp280_write(BMP280_REG_CONFIG, 0xB0);

    bmp280_ok = true;
    printf("BMP280: Initialized OK at address 0x%02X\n", BMP280_I2C_ADDR);

    while (1) {
        // Read 6 bytes: press[2:0] then temp[2:0], all 20-bit MSB-first with 4-bit XLSB
        uint8_t data[6];
        if (bmp280_read(BMP280_REG_PRESS_MSB, data, 6) == ESP_OK) {
            int32_t adc_P = (int32_t)((data[0] << 12) | (data[1] << 4) | (data[2] >> 4));
            int32_t adc_T = (int32_t)((data[3] << 12) | (data[4] << 4) | (data[5] >> 4));
            // Temperature must be computed first to populate bmp280_t_fine for pressure
            bmp280_temperature = bmp280_comp_temp(adc_T);
            bmp280_pressure    = bmp280_comp_press(adc_P);
        }
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}

static void btn_record_event_cb(lv_event_t * e) {
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t * btn = lv_event_get_target(e);

    if (code == LV_EVENT_PRESSED) {
        lv_obj_set_style_bg_color(btn, lv_palette_main(LV_PALETTE_RED), 0);
        rec_sample_count = 0;
        is_playing_reverse = false;
        is_recording = true;
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x555555), 0);
        is_recording = false;
        if (rec_sample_count > 0 && rec_buffer != NULL) {
            // Calculate 99th percentile for volume auto-scaling
            int bins[100] = {0};
            for (int i = 0; i < rec_sample_count; i++) {
                int val = rec_buffer[i];
                if (val < 0) val = -val;
                if (val > 32767) val = 32767;
                int bin = (val * 100) / 32768; // 0 to 99
                if (bin > 99) bin = 99;
                if (bin < 0) bin = 0;
                bins[bin]++;
            }
            int target_count = (rec_sample_count * 99) / 100;
            int count = 0;
            int p99_val = 32767;
            for (int i = 0; i < 100; i++) {
                count += bins[i];
                if (count >= target_count) {
                    p99_val = (i * 32768) / 100;
                    break;
                }
            }
            if (p99_val < 50) p99_val = 50; // Prevent infinite/massive gain on silence
            rec_multiplier = 32760.0f / (float)p99_val;
            if (rec_multiplier > 100.0f) rec_multiplier = 100.0f; // Cap max boost at 100x

            rec_play_idx = rec_sample_count - 1;
            is_playing_reverse = true;

            if (record_canvas && record_canvas_aligned_buf) {
                int chart_h = 240;
                int chart_w = 640;
                lv_canvas_fill_bg(record_canvas, lv_color_hex(0x000000), LV_OPA_COVER);

                int step = rec_sample_count / chart_w;
                if (step == 0) step = 1;

                int FFT_SIZE = 1024;
                float *vReal = malloc(FFT_SIZE * sizeof(float));
                float *vImag = malloc(FFT_SIZE * sizeof(float));
                float *mags  = malloc((FFT_SIZE / 2) * sizeof(float));

                if (vReal && vImag && mags) {
                    int num_bins = FFT_SIZE / 2;
                    float log_max = logf((float)(num_bins - 1));

                    for (int x = 0; x < chart_w; x++) {
                        int start_idx = x * step;

                        // Fill FFT buffer
                        for (int i = 0; i < FFT_SIZE; i++) {
                            if (start_idx + i < rec_sample_count && start_idx + i >= 0) {
                                // Apply Hanning Window
                                float mult = 0.5f * (1.0f - cosf(2.0f * M_PI * i / (FFT_SIZE - 1)));
                                vReal[i] = (float)rec_buffer[start_idx + i] * mult;
                            } else {
                                vReal[i] = 0.0f;
                            }
                            vImag[i] = 0.0f;
                        }

                        compute_fft(vReal, vImag, FFT_SIZE);

                        float max_mag = 0.0f;

                        for (int i = 0; i < num_bins; i++) {
                            float mag = sqrtf(vReal[i]*vReal[i] + vImag[i]*vImag[i]);
                            mags[i] = mag;
                            if (i > 0 && mag > max_mag) {
                                max_mag = mag;
                            }
                        }

                        if (max_mag < 1000.0f) max_mag = 1000.0f;
                        float scale = 1.0f / (max_mag * 0.7f);

                        for (int y = 0; y < chart_h; y++) {
                            float ratio = (float)(chart_h - 1 - y) / (float)(chart_h - 1);
                            float exact_bin = expf(ratio * log_max);
                            int bin = (int)exact_bin;

                            if (bin < 1) bin = 1;
                            if (bin >= num_bins) bin = num_bins - 1;

                            float intensity = mags[bin] * scale;
                            lv_color_t color = get_heatmap_color(intensity);
                            lv_canvas_set_px(record_canvas, x, y, color, LV_OPA_COVER);
                        }
                    }
                    free(vReal);
                    free(vImag);
                    free(mags);
                }

                lv_obj_invalidate(record_canvas);
            }
        }
    }
}

// ---------------------------------------------------------------------
// UI SETUP
// ---------------------------------------------------------------------

// ---------------------------------------------------------------------
// WEATHER SCREEN
// ---------------------------------------------------------------------

static void update_weather_cb(lv_timer_t * timer)
{
    if (!weather_temp_label || !weather_press_label) return;
    if (bmp280_ok) {
        char tbuf[16], pbuf[16];
        snprintf(tbuf, sizeof(tbuf), "%.1f", (float)bmp280_temperature);
        snprintf(pbuf, sizeof(pbuf), "%.1f", (float)bmp280_pressure);
        lv_label_set_text(weather_temp_label,  tbuf);
        lv_label_set_text(weather_press_label, pbuf);
        if (weather_status_label) lv_label_set_text(weather_status_label, "");
    } else {
        lv_label_set_text(weather_temp_label,  "--.-");
        lv_label_set_text(weather_press_label, "---.-");
        if (weather_status_label) lv_label_set_text(weather_status_label, "Sensor error - check wiring (GPIO7=SDA, GPIO8=SCL)");
    }
}

static lv_obj_t * make_sensor_card(lv_obj_t *parent, int x_ofs, lv_color_t border_col,
                                   lv_color_t bg_col, const char *title_text,
                                   lv_obj_t **value_label_out, const char *unit_text)
{
    lv_obj_t * card = lv_obj_create(parent);
    lv_obj_set_size(card, 305, 290);
    lv_obj_align(card, LV_ALIGN_TOP_MID, x_ofs, 90);
    lv_obj_set_style_bg_color(card, bg_col, 0);
    lv_obj_set_style_border_color(card, border_col, 0);
    lv_obj_set_style_border_width(card, 2, 0);
    lv_obj_set_style_radius(card, 20, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    // Card title (e.g. "TEMPERATURE")
    lv_obj_t * title = lv_label_create(card);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(title, border_col, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 8);
    lv_label_set_text(title, title_text);

    // Big numeric value
    lv_obj_t * val = lv_label_create(card);
    lv_obj_set_style_text_font(val, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(val, lv_color_white(), 0);
    lv_obj_align(val, LV_ALIGN_CENTER, 0, -10);
    lv_label_set_text(val, "--.-");
    *value_label_out = val;

    // Unit label (e.g. "°C" or "hPa")
    lv_obj_t * unit = lv_label_create(card);
    lv_obj_set_style_text_font(unit, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(unit, lv_color_hex(0xaaaaaa), 0);
    lv_obj_align(unit, LV_ALIGN_BOTTOM_MID, 0, -10);
    lv_label_set_text(unit, unit_text);

    return card;
}

void create_weather_screen(void)
{
    weather_scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(weather_scr, lv_color_hex(0x0d1b2a), 0);

    // Header bar
    lv_obj_t * header = lv_obj_create(weather_scr);
    lv_obj_set_size(header, LCD_H_RES, 60);
    lv_obj_align(header, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(header, lv_color_hex(0x111111), 0);
    lv_obj_set_style_border_width(header, 0, 0);

    time_label_weather = lv_label_create(header);
    lv_obj_set_style_text_font(time_label_weather, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(time_label_weather, lv_color_white(), 0);
    lv_obj_align(time_label_weather, LV_ALIGN_LEFT_MID, 10, 0);
    lv_label_set_text(time_label_weather, "Waiting for Wi-Fi...");

    lv_obj_t * title_label = lv_label_create(header);
    lv_obj_set_style_text_font(title_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(title_label, lv_palette_main(LV_PALETTE_CYAN), 0);
    lv_obj_align(title_label, LV_ALIGN_CENTER, 0, 0);
    lv_label_set_text(title_label, "Weather Station");

    lv_obj_t * btn_back = lv_btn_create(header);
    lv_obj_set_size(btn_back, 80, 40);
    lv_obj_align(btn_back, LV_ALIGN_RIGHT_MID, -10, 0);
    lv_obj_t * lbl_back = lv_label_create(btn_back);
    lv_label_set_text(lbl_back, "Back");
    lv_obj_center(lbl_back);
    lv_obj_add_event_cb(btn_back, btn_go_menu_cb, LV_EVENT_CLICKED, NULL);

    // Temperature card (left)
    make_sensor_card(weather_scr, -183,
                     lv_palette_main(LV_PALETTE_CYAN),
                     lv_color_hex(0x0a2030),
                     "TEMPERATURE",
                     &weather_temp_label,
                     "\xc2\xb0""C");   // UTF-8 degree symbol

    // Pressure card (right)
    make_sensor_card(weather_scr, +183,
                     lv_palette_main(LV_PALETTE_GREEN),
                     lv_color_hex(0x0a2018),
                     "PRESSURE",
                     &weather_press_label,
                     "hPa");

    // Status / error label at the bottom
    weather_status_label = lv_label_create(weather_scr);
    lv_obj_set_style_text_font(weather_status_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(weather_status_label, lv_palette_main(LV_PALETTE_RED), 0);
    lv_obj_align(weather_status_label, LV_ALIGN_BOTTOM_MID, 0, -20);
    lv_label_set_text(weather_status_label, "Initializing sensor...");

    lv_timer_create(update_weather_cb, 2000, NULL);
}

void create_main_menu(void)
{
    main_menu_scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(main_menu_scr, lv_color_hex(0x111111), 0);
    lv_obj_set_style_bg_opa(main_menu_scr, LV_OPA_COVER, 0);

    lv_obj_t * title = lv_label_create(main_menu_scr);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(title, lv_palette_main(LV_PALETTE_AMBER), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 30);
    lv_label_set_text(title, "ESP32-P4 Launchpad");

    time_label_menu = lv_label_create(main_menu_scr);
    lv_obj_set_style_text_font(time_label_menu, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(time_label_menu, lv_color_white(), 0);
    lv_obj_align(time_label_menu, LV_ALIGN_TOP_MID, 0, 60);
    lv_label_set_text(time_label_menu, "Waiting for Wi-Fi...");

    // 2×2 button grid
    lv_obj_t * btn_synth = lv_btn_create(main_menu_scr);
    lv_obj_set_size(btn_synth, 200, 80);
    lv_obj_align(btn_synth, LV_ALIGN_CENTER, -155, -55);
    lv_obj_t * lbl_synth = lv_label_create(btn_synth);
    lv_label_set_text(lbl_synth, "NanoSynth");
    lv_obj_center(lbl_synth);
    lv_obj_add_event_cb(btn_synth, btn_go_synth_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t * btn_clock = lv_btn_create(main_menu_scr);
    lv_obj_set_size(btn_clock, 200, 80);
    lv_obj_align(btn_clock, LV_ALIGN_CENTER, 155, -55);
    lv_obj_t * lbl_clock = lv_label_create(btn_clock);
    lv_label_set_text(lbl_clock, "Analog Clock");
    lv_obj_center(lbl_clock);
    lv_obj_add_event_cb(btn_clock, btn_go_clock_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t * btn_record = lv_btn_create(main_menu_scr);
    lv_obj_set_size(btn_record, 200, 80);
    lv_obj_align(btn_record, LV_ALIGN_CENTER, -155, 55);
    lv_obj_t * lbl_record = lv_label_create(btn_record);
    lv_label_set_text(lbl_record, "Reverse Recorder");
    lv_obj_center(lbl_record);
    lv_obj_add_event_cb(btn_record, btn_go_record_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t * btn_weather = lv_btn_create(main_menu_scr);
    lv_obj_set_size(btn_weather, 200, 80);
    lv_obj_align(btn_weather, LV_ALIGN_CENTER, 155, 55);
    lv_obj_t * lbl_weather = lv_label_create(btn_weather);
    lv_label_set_text(lbl_weather, "Weather Station");
    lv_obj_center(lbl_weather);
    lv_obj_add_event_cb(btn_weather, btn_go_weather_cb, LV_EVENT_CLICKED, NULL);
}

void create_clock_screen(void)
{
    clock_scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(clock_scr, lv_color_hex(0x000000), 0);

    lv_obj_t * btn_back = lv_btn_create(clock_scr);
    lv_obj_set_size(btn_back, 100, 40);
    lv_obj_align(btn_back, LV_ALIGN_TOP_RIGHT, -10, 10);
    lv_obj_t * lbl_back = lv_label_create(btn_back);
    lv_label_set_text(lbl_back, "Back");
    lv_obj_center(lbl_back);
    lv_obj_add_event_cb(btn_back, btn_go_menu_cb, LV_EVENT_CLICKED, NULL);

    // Clock Face
    lv_obj_t * face = lv_obj_create(clock_scr);
    lv_obj_set_size(face, 400, 400);
    lv_obj_align(face, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_radius(face, 200, 0);
    lv_obj_set_style_bg_color(face, lv_color_hex(0x222222), 0);
    lv_obj_set_style_border_color(face, lv_palette_main(LV_PALETTE_AMBER), 0);
    lv_obj_set_style_border_width(face, 5, 0);

    // Hands
    clock_hour_hand = lv_obj_create(face);
    lv_obj_set_size(clock_hour_hand, 8, 120);
    lv_obj_set_style_bg_color(clock_hour_hand, lv_color_white(), 0);
    lv_obj_align(clock_hour_hand, LV_ALIGN_CENTER, 0, -40); // Offset upwards so bottom is at center
    lv_obj_set_style_transform_pivot_x(clock_hour_hand, 4, 0);
    lv_obj_set_style_transform_pivot_y(clock_hour_hand, 100, 0); // Pivot near the bottom
    lv_obj_set_style_border_width(clock_hour_hand, 0, 0);

    clock_min_hand = lv_obj_create(face);
    lv_obj_set_size(clock_min_hand, 6, 170);
    lv_obj_set_style_bg_color(clock_min_hand, lv_color_hex(0xcccccc), 0);
    lv_obj_align(clock_min_hand, LV_ALIGN_CENTER, 0, -65);
    lv_obj_set_style_transform_pivot_x(clock_min_hand, 3, 0);
    lv_obj_set_style_transform_pivot_y(clock_min_hand, 150, 0); // Pivot near the bottom
    lv_obj_set_style_border_width(clock_min_hand, 0, 0);

    clock_sec_hand = lv_obj_create(face);
    lv_obj_set_size(clock_sec_hand, 2, 190);
    lv_obj_set_style_bg_color(clock_sec_hand, lv_palette_main(LV_PALETTE_RED), 0);
    lv_obj_align(clock_sec_hand, LV_ALIGN_CENTER, 0, -75);
    lv_obj_set_style_transform_pivot_x(clock_sec_hand, 1, 0);
    lv_obj_set_style_transform_pivot_y(clock_sec_hand, 170, 0); // Pivot near the bottom
    lv_obj_set_style_border_width(clock_sec_hand, 0, 0);

// Center dot
    lv_obj_t * dot = lv_obj_create(face);
    lv_obj_set_size(dot, 16, 16);
    lv_obj_align(dot, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_radius(dot, 8, 0);
    lv_obj_set_style_bg_color(dot, lv_palette_main(LV_PALETTE_AMBER), 0);
    lv_obj_set_style_border_width(dot, 0, 0);
}

void create_record_screen(void)
{
    record_scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(record_scr, lv_color_hex(0x222222), 0);

    // Header container
    lv_obj_t * header = lv_obj_create(record_scr);
    lv_obj_set_size(header, LCD_H_RES, 60);
    lv_obj_align(header, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(header, lv_color_hex(0x111111), 0);
    lv_obj_set_style_border_width(header, 0, 0);

    time_label_record = lv_label_create(header);
    lv_obj_set_style_text_font(time_label_record, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(time_label_record, lv_color_white(), 0);
    lv_obj_align(time_label_record, LV_ALIGN_LEFT_MID, 10, 0);
    lv_label_set_text(time_label_record, "Waiting for Wi-Fi...");

    lv_obj_t * btn_back = lv_btn_create(header);
    lv_obj_set_size(btn_back, 80, 40);
    lv_obj_align(btn_back, LV_ALIGN_RIGHT_MID, -10, 0);
    lv_obj_t * lbl_back = lv_label_create(btn_back);
    lv_label_set_text(lbl_back, "Back");
    lv_obj_center(lbl_back);
    lv_obj_add_event_cb(btn_back, btn_go_menu_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t * title_label = lv_label_create(header);
    lv_obj_set_style_text_font(title_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(title_label, lv_palette_main(LV_PALETTE_AMBER), 0);
    lv_obj_align(title_label, LV_ALIGN_CENTER, 0, 0);
    lv_label_set_text(title_label, "Reverse Recorder");

    // Central Record Button
    lv_obj_t * btn_rec = lv_btn_create(record_scr);
    lv_obj_set_size(btn_rec, 240, 240);
    lv_obj_align(btn_rec, LV_ALIGN_TOP_MID, 0, 80);
    lv_obj_set_style_radius(btn_rec, 120, 0);
    lv_obj_set_style_shadow_width(btn_rec, 0, 0);
    lv_obj_set_style_shadow_width(btn_rec, 0, LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(btn_rec, lv_color_hex(0x555555), 0);
    lv_obj_add_event_cb(btn_rec, btn_record_event_cb, LV_EVENT_ALL, NULL);

    lv_obj_t * lbl_rec = lv_label_create(btn_rec);
    lv_obj_set_style_text_font(lbl_rec, &lv_font_montserrat_14, 0);
    lv_label_set_text(lbl_rec, "HOLD TO RECORD");
    lv_obj_center(lbl_rec);

    // Audio spectrogram canvas
    record_canvas = lv_canvas_create(record_scr);
    lv_obj_set_size(record_canvas, 640, 240);
    lv_obj_align(record_canvas, LV_ALIGN_BOTTOM_MID, 0, -40);
    lv_obj_set_style_border_color(record_canvas, lv_color_hex(0x555555), 0);
    lv_obj_set_style_border_width(record_canvas, 2, 0);

    // Allocate the draw buffer for a 640x240 RGB565 canvas from PSRAM
    size_t canvas_size = 640 * 240 * 2;
    record_canvas_raw_buf = heap_caps_malloc(canvas_size + 128, MALLOC_CAP_SPIRAM);
    if (record_canvas_raw_buf) {
        record_canvas_aligned_buf = (uint8_t *)(((uintptr_t)record_canvas_raw_buf + 63) & ~63);
        lv_canvas_set_buffer(record_canvas, record_canvas_aligned_buf, 640, 240, LV_COLOR_FORMAT_RGB565);
        lv_canvas_fill_bg(record_canvas, lv_color_hex(0x000000), LV_OPA_COVER);
    }
}

void create_synth_ui(void)
{
    synth_scr = lv_obj_create(NULL);
    lv_obj_t * scr = synth_scr;
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x222222), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    // Header container
    lv_obj_t * header = lv_obj_create(scr);
    lv_obj_set_size(header, LCD_H_RES, 60);
    lv_obj_align(header, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(header, lv_color_hex(0x111111), 0);
    lv_obj_set_style_border_width(header, 0, 0);

    time_label_synth = lv_label_create(header);
    lv_obj_set_style_text_font(time_label_synth, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(time_label_synth, lv_color_white(), 0);
    lv_obj_align(time_label_synth, LV_ALIGN_LEFT_MID, 10, 0);
    lv_label_set_text(time_label_synth, "Waiting for Wi-Fi...");

    // Add Back Button
    lv_obj_t * btn_back = lv_btn_create(header);
    lv_obj_set_size(btn_back, 80, 40);
    lv_obj_align(btn_back, LV_ALIGN_RIGHT_MID, -10, 0);
    lv_obj_t * lbl_back = lv_label_create(btn_back);
    lv_label_set_text(lbl_back, "Back");
    lv_obj_center(lbl_back);
    lv_obj_add_event_cb(btn_back, btn_go_menu_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t * title_label = lv_label_create(header);
    lv_obj_set_style_text_font(title_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(title_label, lv_palette_main(LV_PALETTE_AMBER), 0);
    lv_obj_align(title_label, LV_ALIGN_CENTER, 0, 0); // Put title in center to make room for Back
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

        mic_codec_dev = bsp_audio_codec_microphone_init();
        if (mic_codec_dev) {
            esp_codec_dev_sample_info_t fs_mic = {
                .sample_rate = SAMPLE_RATE,
                .channel = 1,
                .bits_per_sample = 16,
            };
            esp_codec_dev_open(mic_codec_dev, &fs_mic);
        }
    }

    rec_buffer = malloc(REC_BUFFER_SAMPLES * sizeof(int16_t));

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

    // Start BMP280 sensor task (I2C bus is ready after bsp_display_start)
    xTaskCreate(bmp280_task, "bmp280_task", 4096, NULL, 3, NULL);

    // 6. Build the UI
    bsp_display_lock(0);
    create_main_menu();
    create_synth_ui();
    create_clock_screen();
    create_record_screen();
    create_weather_screen();

    // Start global update timer
    lv_timer_create(update_time_cb, 1000, NULL);

    // Load initial screen
    lv_scr_load(main_menu_scr);
    bsp_display_unlock();
}
