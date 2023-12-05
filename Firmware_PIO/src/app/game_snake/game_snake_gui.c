#include "game_snake_gui.h"

#include "lvgl.h"

lv_obj_t *game_snake_gui = NULL;

static lv_style_t default_style;
static lv_style_t label_style;

void game_snake_gui_init(void)
{ 
    lv_style_init(&default_style);
}

/*
 * 其他函数请根据需要添加
 */

void display_snake(const char *file_name, lv_scr_load_anim_t anim_type)
{
}

void game_snake_gui_del(void)
{
    if (NULL != game_snake_gui)
    {
        lv_obj_clean(game_snake_gui);
        game_snake_gui = NULL;
    }
    
    // 手动清除样式，防止内存泄漏
    // lv_style_reset(&default_style);
}