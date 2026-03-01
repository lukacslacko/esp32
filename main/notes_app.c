#include "notes_app.h"
#include "esp_heap_caps.h"
#include <stdio.h>
#include <string.h>
#include "nvs_flash.h"
#include "nvs.h"

#define MAX_NOTES 10
#define MAX_STROKES_PER_NOTE 100
#define LCD_H_RES 720
#define LCD_V_RES 720

typedef struct {
    lv_point_precise_t * points;
    uint32_t point_cnt;
    uint32_t point_cap;
    lv_color_t color;
    uint16_t width;
    lv_obj_t * edit_line_obj;
} note_stroke_t;

typedef struct {
    bool in_use;
    note_stroke_t strokes[MAX_STROKES_PER_NOTE];
    uint32_t stroke_cnt;
} note_data_t;

static note_data_t notes_db[MAX_NOTES];
static int target_note_idx = -1;
static int delete_note_idx = -1;

static lv_obj_t * notes_menu_scr = NULL;
static lv_obj_t * notes_edit_scr = NULL;
static lv_obj_t * notes_list_cont = NULL;
static lv_obj_t * draw_canvas_area = NULL;
static lv_obj_t * note_delete_mbox = NULL;
static lv_event_cb_t main_menu_cb_ptr = NULL;
static lv_obj_t * main_menu_scr_ptr = NULL;

static bool is_drawing = false;
static note_stroke_t * current_stroke = NULL;

static lv_color_t active_color;
static uint16_t active_width = 5;
static bool is_eraser = false;

static void render_thumbnails(void);
static void open_note_edit(int idx);

static void save_notes_to_nvs(void) {
    nvs_handle_t my_handle;
    esp_err_t err = nvs_open("notes_storage", NVS_READWRITE, &my_handle);
    if (err != ESP_OK) return;

    size_t required_size = sizeof(uint32_t); // Format version

    // First, calculate total size required
    for (int i = 0; i < MAX_NOTES; i++) {
        required_size += sizeof(bool); // in_use
        if (notes_db[i].in_use) {
            required_size += sizeof(uint32_t); // stroke_cnt
            for (uint32_t s = 0; s < notes_db[i].stroke_cnt; s++) {
                required_size += sizeof(uint32_t); // point_cnt
                required_size += sizeof(uint16_t); // width
                required_size += 3; // R, G, B colors
                required_size += notes_db[i].strokes[s].point_cnt * sizeof(lv_point_precise_t);
            }
        }
    }

    uint8_t * blob = heap_caps_malloc(required_size, MALLOC_CAP_SPIRAM);
    if (!blob) {
        nvs_close(my_handle);
        return;
    }

    uint8_t * ptr = blob;
    uint32_t version = 2;
    memcpy(ptr, &version, sizeof(uint32_t)); ptr += sizeof(uint32_t);

    for (int i = 0; i < MAX_NOTES; i++) {
        bool in_use = notes_db[i].in_use;
        memcpy(ptr, &in_use, sizeof(bool)); ptr += sizeof(bool);

        if (in_use) {
            uint32_t sc = notes_db[i].stroke_cnt;
            memcpy(ptr, &sc, sizeof(uint32_t)); ptr += sizeof(uint32_t);

            for (uint32_t s = 0; s < sc; s++) {
                uint32_t pc = notes_db[i].strokes[s].point_cnt;
                memcpy(ptr, &pc, sizeof(uint32_t)); ptr += sizeof(uint32_t);

                uint16_t w = notes_db[i].strokes[s].width;
                memcpy(ptr, &w, sizeof(uint16_t)); ptr += sizeof(uint16_t);

                lv_color_t c = notes_db[i].strokes[s].color;
                *ptr++ = c.red;
                *ptr++ = c.green;
                *ptr++ = c.blue;

                size_t points_size = pc * sizeof(lv_point_precise_t);
                memcpy(ptr, notes_db[i].strokes[s].points, points_size); ptr += points_size;
            }
        }
    }

    nvs_set_blob(my_handle, "notes_blob", blob, required_size);
    nvs_commit(my_handle);
    nvs_close(my_handle);
    heap_caps_free(blob);
}

static void load_notes_from_nvs(void) {
    nvs_handle_t my_handle;
    esp_err_t err = nvs_open("notes_storage", NVS_READONLY, &my_handle);
    if (err != ESP_OK) return;

    size_t required_size = 0;
    err = nvs_get_blob(my_handle, "notes_blob", NULL, &required_size);
    if (err != ESP_OK || required_size == 0) {
        nvs_close(my_handle);
        return;
    }

    uint8_t * blob = heap_caps_malloc(required_size, MALLOC_CAP_SPIRAM);
    if (!blob) {
        nvs_close(my_handle);
        return;
    }

    err = nvs_get_blob(my_handle, "notes_blob", blob, &required_size);
    if (err != ESP_OK) {
        heap_caps_free(blob);
        nvs_close(my_handle);
        return;
    }

    uint8_t * ptr = blob;
    uint32_t version = 0;
    memcpy(&version, ptr, sizeof(uint32_t)); ptr += sizeof(uint32_t);

    if (version == 1 || version == 2) {
        for (int i = 0; i < MAX_NOTES; i++) {
            bool in_use = false;
            memcpy(&in_use, ptr, sizeof(bool)); ptr += sizeof(bool);
            notes_db[i].in_use = in_use;

            if (in_use) {
                uint32_t sc = 0;
                memcpy(&sc, ptr, sizeof(uint32_t)); ptr += sizeof(uint32_t);
                notes_db[i].stroke_cnt = sc;

                for (uint32_t s = 0; s < sc; s++) {
                    uint32_t pc = 0;
                    memcpy(&pc, ptr, sizeof(uint32_t)); ptr += sizeof(uint32_t);
                    notes_db[i].strokes[s].point_cnt = pc;
                    notes_db[i].strokes[s].point_cap = pc;

                    uint16_t w = 0;
                    memcpy(&w, ptr, sizeof(uint16_t)); ptr += sizeof(uint16_t);
                    notes_db[i].strokes[s].width = w;
                    
                    if (version == 2) {
                        uint8_t cr = *ptr++;
                        uint8_t cg = *ptr++;
                        uint8_t cb = *ptr++;
                        notes_db[i].strokes[s].color = lv_color_make(cr, cg, cb);
                    } else {
                        notes_db[i].strokes[s].color = lv_color_black();
                    }
                    
                    notes_db[i].strokes[s].edit_line_obj = NULL;

                    size_t points_size = pc * sizeof(lv_point_precise_t);
                    notes_db[i].strokes[s].points = heap_caps_malloc(points_size, MALLOC_CAP_SPIRAM);
                    memcpy(notes_db[i].strokes[s].points, ptr, points_size); ptr += points_size;
                }
            } else {
                notes_db[i].stroke_cnt = 0;
            }
        }
    }

    heap_caps_free(blob);
    nvs_close(my_handle);
}

static void btn_go_notes_cb_internal(lv_event_t * e) {
    if (notes_menu_scr) {
        render_thumbnails();
        lv_scr_load(notes_menu_scr);
    }
}

void btn_go_notes_cb(lv_event_t * e) {
    btn_go_notes_cb_internal(e);
}

static void btn_save_note_cb(lv_event_t * e) {
    if (target_note_idx >= 0 && target_note_idx < MAX_NOTES) {
        for (uint32_t i = 0; i < notes_db[target_note_idx].stroke_cnt; i++) {
            notes_db[target_note_idx].strokes[i].edit_line_obj = NULL;
        }
    }

    save_notes_to_nvs();

    lv_obj_clean(draw_canvas_area);
    render_thumbnails();
    lv_scr_load(notes_menu_scr);
}

static void free_points_cb(lv_event_t * e) {
    lv_point_precise_t * pts = lv_event_get_user_data(e);
    if(pts) heap_caps_free(pts);
}

static void btn_delete_yes_cb(lv_event_t * e) {
    if (delete_note_idx >= 0 && delete_note_idx < MAX_NOTES) {
        note_data_t * note = &notes_db[delete_note_idx];
        for (uint32_t i = 0; i < note->stroke_cnt; i++) {
            if (note->strokes[i].points) {
                heap_caps_free(note->strokes[i].points);
                note->strokes[i].points = NULL;
            }
        }
        note->stroke_cnt = 0;
        note->in_use = false;

        save_notes_to_nvs();

        render_thumbnails();
    }
    if (note_delete_mbox) {
        lv_msgbox_close(note_delete_mbox);
        note_delete_mbox = NULL;
    }
}

static void btn_delete_no_cb(lv_event_t * e) {
    if (note_delete_mbox) {
        lv_msgbox_close(note_delete_mbox);
        note_delete_mbox = NULL;
    }
}

static void thumb_event_cb(lv_event_t * e) {
    lv_event_code_t code = lv_event_get_code(e);
    intptr_t idx = (intptr_t)lv_event_get_user_data(e);

    if (code == LV_EVENT_SHORT_CLICKED) {
        open_note_edit((int)idx);
    } else if (code == LV_EVENT_LONG_PRESSED) {
        delete_note_idx = (int)idx;
        note_delete_mbox = lv_msgbox_create(NULL);
        lv_msgbox_add_title(note_delete_mbox, "Delete Note?");
        lv_msgbox_add_text(note_delete_mbox, "Are you sure you want to delete this note?");
        lv_obj_t * btn_yes = lv_msgbox_add_footer_button(note_delete_mbox, "Yes");
        lv_obj_t * btn_no = lv_msgbox_add_footer_button(note_delete_mbox, "No");
        lv_obj_add_event_cb(btn_yes, btn_delete_yes_cb, LV_EVENT_CLICKED, NULL);
        lv_obj_add_event_cb(btn_no, btn_delete_no_cb, LV_EVENT_CLICKED, NULL);
    }
}

static void open_note_edit(int idx) {
    target_note_idx = idx;
    lv_obj_clean(draw_canvas_area);

    if (!notes_db[idx].in_use) {
        notes_db[idx].in_use = true;
        notes_db[idx].stroke_cnt = 0;
    } else {
        for (uint32_t i = 0; i < notes_db[idx].stroke_cnt; i++) {
            note_stroke_t * st = &notes_db[idx].strokes[i];
            st->edit_line_obj = lv_line_create(draw_canvas_area);
            lv_obj_align(st->edit_line_obj, LV_ALIGN_TOP_LEFT, 0, 0);
            lv_obj_set_style_line_color(st->edit_line_obj, st->color, 0);
            lv_obj_set_style_line_width(st->edit_line_obj, st->width, 0);
            lv_obj_set_style_line_rounded(st->edit_line_obj, true, 0);
            lv_obj_add_flag(st->edit_line_obj, LV_OBJ_FLAG_EVENT_BUBBLE);
            lv_line_set_points(st->edit_line_obj, st->points, st->point_cnt);
        }
    }

    lv_scr_load(notes_edit_scr);
}

static void btn_create_note_cb(lv_event_t * e) {
    for (int i = 0; i < MAX_NOTES; i++) {
        if (!notes_db[i].in_use) {
            open_note_edit(i);
            return;
        }
    }
    note_delete_mbox = lv_msgbox_create(NULL);
    lv_msgbox_add_title(note_delete_mbox, "Error");
    lv_msgbox_add_text(note_delete_mbox, "Maximum number of notes reached!");
    lv_obj_t * btn_ok = lv_msgbox_add_footer_button(note_delete_mbox, "OK");
    lv_obj_add_event_cb(btn_ok, btn_delete_no_cb, LV_EVENT_CLICKED, NULL);
}

static void render_thumbnails(void) {
    lv_obj_clean(notes_list_cont);

    lv_obj_t * btn_new = lv_btn_create(notes_list_cont);
    lv_obj_set_size(btn_new, 200, 200);
    lv_obj_add_event_cb(btn_new, btn_create_note_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t * lbl_new = lv_label_create(btn_new);
    lv_label_set_text(lbl_new, "+ New Note");
    lv_obj_center(lbl_new);

    for (int i = 0; i < MAX_NOTES; i++) {
        if (notes_db[i].in_use) {
            lv_obj_t * btn = lv_btn_create(notes_list_cont);
            lv_obj_set_size(btn, 200, 200);
            lv_obj_set_style_bg_color(btn, lv_color_hex(0xffffff), 0);
            lv_obj_set_style_pad_all(btn, 0, 0);
            lv_obj_set_style_border_width(btn, 2, 0);

            lv_obj_add_event_cb(btn, thumb_event_cb, LV_EVENT_ALL, (void*)(intptr_t)i);

            for (uint32_t s = 0; s < notes_db[i].stroke_cnt; s++) {
                note_stroke_t * st = &notes_db[i].strokes[s];
                if (st->point_cnt == 0 || st->points == NULL) continue;

                lv_obj_t * l = lv_line_create(btn);
                lv_obj_align(l, LV_ALIGN_TOP_LEFT, 0, 0);
                lv_obj_set_style_line_color(l, st->color, 0);
                // thumbnail scale width
                int scaled_w = st->width / 3;
                if (scaled_w < 1) scaled_w = 1;
                lv_obj_set_style_line_width(l, scaled_w, 0);
                lv_obj_add_flag(l, LV_OBJ_FLAG_EVENT_BUBBLE);

                lv_point_precise_t * scaled_pts = heap_caps_malloc(st->point_cnt * sizeof(lv_point_precise_t), MALLOC_CAP_SPIRAM);
                if (scaled_pts) {
                    for(uint32_t p = 0; p < st->point_cnt; p++) {
                        scaled_pts[p].x = (st->points[p].x * 200) / (LCD_H_RES - 20);
                        scaled_pts[p].y = (st->points[p].y * 200) / (LCD_V_RES - 140);
                    }
                    lv_line_set_points(l, scaled_pts, st->point_cnt);
                    lv_obj_add_event_cb(l, free_points_cb, LV_EVENT_DELETE, scaled_pts);
                }
            }
        }
    }
}

static void add_point_to_current_stroke(int32_t lx, int32_t ly) {
    if (!current_stroke) return;
    if (current_stroke->point_cnt >= current_stroke->point_cap) {
        current_stroke->point_cap += 128;
        current_stroke->points = heap_caps_realloc(current_stroke->points, current_stroke->point_cap * sizeof(lv_point_precise_t), MALLOC_CAP_SPIRAM);
    }
    current_stroke->points[current_stroke->point_cnt].x = lx;
    current_stroke->points[current_stroke->point_cnt].y = ly;
    current_stroke->point_cnt++;

    if (current_stroke->edit_line_obj) {
        lv_line_set_points(current_stroke->edit_line_obj, current_stroke->points, current_stroke->point_cnt);
    }
}

static void draw_area_event_cb(lv_event_t * e) {
    lv_event_code_t code = lv_event_get_code(e);
    lv_indev_t * indev = lv_event_get_param(e);
    if (!indev || target_note_idx < 0) return;

    note_data_t * note = &notes_db[target_note_idx];

    if (code == LV_EVENT_PRESSED) {
        if (note->stroke_cnt >= MAX_STROKES_PER_NOTE) return;

        lv_point_t p;
        lv_indev_get_point(indev, &p);

        lv_area_t ca;
        lv_obj_get_coords(draw_canvas_area, &ca);
        int32_t lx = p.x - ca.x1;
        int32_t ly = p.y - ca.y1;

        current_stroke = &note->strokes[note->stroke_cnt];
        current_stroke->point_cnt = 0;
        current_stroke->point_cap = 128;
        current_stroke->points = heap_caps_malloc(128 * sizeof(lv_point_precise_t), MALLOC_CAP_SPIRAM);
        current_stroke->color = is_eraser ? lv_color_white() : active_color;
        current_stroke->width = active_width;

        current_stroke->edit_line_obj = lv_line_create(draw_canvas_area);
        lv_obj_align(current_stroke->edit_line_obj, LV_ALIGN_TOP_LEFT, 0, 0);
        lv_obj_set_style_line_color(current_stroke->edit_line_obj, current_stroke->color, 0);
        lv_obj_set_style_line_width(current_stroke->edit_line_obj, current_stroke->width, 0);
        lv_obj_set_style_line_rounded(current_stroke->edit_line_obj, true, 0);
        lv_obj_add_flag(current_stroke->edit_line_obj, LV_OBJ_FLAG_EVENT_BUBBLE);

        add_point_to_current_stroke(lx, ly);

        note->stroke_cnt++;
        is_drawing = true;
    }
    else if (code == LV_EVENT_PRESSING) {
        if (is_drawing && current_stroke) {
            lv_point_t p;
            lv_indev_get_point(indev, &p);

            lv_area_t ca;
            lv_obj_get_coords(draw_canvas_area, &ca);
            int32_t lx = p.x - ca.x1;
            int32_t ly = p.y - ca.y1;

            if (current_stroke->point_cnt > 0) {
                int32_t last_x = current_stroke->points[current_stroke->point_cnt - 1].x;
                int32_t last_y = current_stroke->points[current_stroke->point_cnt - 1].y;
                // Skip if movement is too small to save RAM
                if (abs(last_x - lx) < 2 && abs(last_y - ly) < 2) return;
            }

            add_point_to_current_stroke(lx, ly);
        }
    }
    else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        is_drawing = false;
        current_stroke = NULL;
    }
}

static void color_btn_cb(lv_event_t * e) {
    lv_obj_t * btn = lv_event_get_target(e);
    active_color = lv_obj_get_style_bg_color(btn, 0);
    is_eraser = false;
}

static void eraser_btn_cb(lv_event_t * e) {
    is_eraser = true;
}

static void slider_width_cb(lv_event_t * e) {
    lv_obj_t * slider = lv_event_get_target(e);
    active_width = lv_slider_get_value(slider);
}

void create_notes_screens(lv_obj_t * main_menu_scr, lv_event_cb_t go_menu_cb) {
    main_menu_scr_ptr = main_menu_scr;
    main_menu_cb_ptr = go_menu_cb;

    memset(notes_db, 0, sizeof(notes_db));

    // Load notes from NVS
    load_notes_from_nvs();

    notes_menu_scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(notes_menu_scr, lv_color_hex(0x222222), 0);

    lv_obj_t * header = lv_obj_create(notes_menu_scr);
    lv_obj_set_size(header, LCD_H_RES, 60);
    lv_obj_align(header, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(header, lv_color_hex(0x111111), 0);
    lv_obj_set_style_border_width(header, 0, 0);
    lv_obj_clear_flag(header, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t * title = lv_label_create(header);
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_align(title, LV_ALIGN_CENTER, 0, 0);
    lv_label_set_text(title, "Quick Notes");

    lv_obj_t * btn_back = lv_btn_create(header);
    lv_obj_set_size(btn_back, 80, 40);
    lv_obj_align(btn_back, LV_ALIGN_RIGHT_MID, -10, 0);
    lv_obj_t * lbl_back = lv_label_create(btn_back);
    lv_label_set_text(lbl_back, "Back");
    lv_obj_center(lbl_back);
    lv_obj_add_event_cb(btn_back, main_menu_cb_ptr, LV_EVENT_CLICKED, NULL);

    notes_list_cont = lv_obj_create(notes_menu_scr);
    lv_obj_set_size(notes_list_cont, LCD_H_RES, LCD_V_RES - 60);
    lv_obj_align(notes_list_cont, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_flex_flow(notes_list_cont, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(notes_list_cont, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(notes_list_cont, 20, 0);
    lv_obj_set_style_pad_column(notes_list_cont, 20, 0);
    lv_obj_set_style_pad_top(notes_list_cont, 20, 0);
    lv_obj_set_style_bg_color(notes_list_cont, lv_color_hex(0x222222), 0);
    lv_obj_set_style_border_width(notes_list_cont, 0, 0);

    notes_edit_scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(notes_edit_scr, lv_color_hex(0x333333), 0);

    lv_obj_t * e_header = lv_obj_create(notes_edit_scr);
    lv_obj_set_size(e_header, LCD_H_RES, 60);
    lv_obj_align(e_header, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(e_header, lv_color_hex(0x111111), 0);
    lv_obj_set_style_border_width(e_header, 0, 0);
    lv_obj_clear_flag(e_header, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t * e_title = lv_label_create(e_header);
    lv_obj_set_style_text_color(e_title, lv_color_white(), 0);
    lv_obj_align(e_title, LV_ALIGN_CENTER, 0, 0);
    lv_label_set_text(e_title, "Draw Note");

    lv_obj_t * btn_done = lv_btn_create(e_header);
    lv_obj_set_size(btn_done, 80, 40);
    lv_obj_align(btn_done, LV_ALIGN_RIGHT_MID, -10, 0);
    lv_obj_t * lbl_done = lv_label_create(btn_done);
    lv_label_set_text(lbl_done, "Done");
    lv_obj_center(lbl_done);
    lv_obj_add_event_cb(btn_done, btn_save_note_cb, LV_EVENT_CLICKED, NULL);

    active_color = lv_color_black();
    active_width = 5;
    is_eraser = false;

    lv_obj_t * tools_cont = lv_obj_create(notes_edit_scr);
    lv_obj_set_size(tools_cont, LCD_H_RES, 70);
    lv_obj_align(tools_cont, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(tools_cont, lv_color_hex(0x222222), 0);
    lv_obj_set_style_border_width(tools_cont, 0, 0);
    lv_obj_set_style_radius(tools_cont, 0, 0);
    lv_obj_set_flex_flow(tools_cont, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(tools_cont, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(tools_cont, LV_OBJ_FLAG_SCROLLABLE);

    lv_color_t colors[] = { lv_color_black(), lv_color_make(255, 0, 0), lv_color_make(0, 255, 0), lv_color_make(0, 0, 255) };
    for (int i = 0; i < 4; i++) {
        lv_obj_t * c_btn = lv_btn_create(tools_cont);
        lv_obj_set_size(c_btn, 40, 40);
        lv_obj_set_style_bg_color(c_btn, colors[i], 0);
        lv_obj_set_style_radius(c_btn, 20, 0);
        lv_obj_add_event_cb(c_btn, color_btn_cb, LV_EVENT_CLICKED, NULL);
    }

    lv_obj_t * btn_eraser = lv_btn_create(tools_cont);
    lv_obj_set_size(btn_eraser, 100, 40);
    lv_obj_add_event_cb(btn_eraser, eraser_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t * lbl_eraser = lv_label_create(btn_eraser);
    lv_label_set_text(lbl_eraser, "Eraser");
    lv_obj_center(lbl_eraser);

    lv_obj_t * width_cont = lv_obj_create(tools_cont);
    lv_obj_set_size(width_cont, 200, 50);
    lv_obj_set_style_bg_opa(width_cont, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(width_cont, 0, 0);
    lv_obj_clear_flag(width_cont, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t * w_slider = lv_slider_create(width_cont);
    lv_slider_set_range(w_slider, 2, 20);
    lv_slider_set_value(w_slider, active_width, LV_ANIM_OFF);
    lv_obj_set_size(w_slider, 160, 10);
    lv_obj_align(w_slider, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_add_event_cb(w_slider, slider_width_cb, LV_EVENT_VALUE_CHANGED, NULL);

    draw_canvas_area = lv_obj_create(notes_edit_scr);
    lv_obj_set_size(draw_canvas_area, LCD_H_RES - 20, LCD_V_RES - 140);
    lv_obj_align(draw_canvas_area, LV_ALIGN_TOP_MID, 0, 65);
    lv_obj_clear_flag(draw_canvas_area, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(draw_canvas_area, draw_area_event_cb, LV_EVENT_ALL, NULL);
}
