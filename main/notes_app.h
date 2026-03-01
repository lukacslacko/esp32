#pragma once

#include "lvgl.h"

void create_notes_screens(lv_obj_t *main_menu_scr, lv_event_cb_t go_menu_cb);
void btn_go_notes_cb(lv_event_t *e);