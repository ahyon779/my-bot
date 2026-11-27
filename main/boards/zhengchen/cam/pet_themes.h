/*
 * pet_themes.h —— my-bot 兼容桩（stub）
 *
 * 原项目 S3-car 的 _themes/pet_themes.c 是一套"角色立绘 + 装饰"引擎，
 * 依赖作者本机素材（_themes/ 目录，gitignored，GitHub 上不存在）。
 *
 * 这里提供一个最小可编译实现：主题 API 全部保留（dafeiyu_display.cc 会调用），
 * 但不画角色立绘 —— 表情由 xiaozhi_face_tft.h / RoboFace（LVGL 自绘，21 种情绪）
 * 承担，不需要任何 GIF/PNG 素材。
 *
 * 如果你后续拿到了作者的原始素材，把本文件替换回原版 _themes/pet_themes.h，
 * 并在 CMakeLists 里恢复 _themes/pet_themes.c 即可。
 */
#pragma once

#include <lvgl.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PET_THEME_IDLE = 0,
    PET_THEME_WALK,
    PET_THEME_CRY,
    PET_THEME_HAPPY,
    PET_THEME_THINK,
    PET_THEME_ANGRY,
    PET_THEME_EAT,
    PET_THEME_YAWN,
    PET_THEME_DANCE,
    PET_THEME_N
} pet_theme_id_t;

// 创建一个空容器作为"角色"占位（真正的表情由 RoboFace 画在 top layer）
static inline lv_obj_t* pet_theme_start(pet_theme_id_t id, lv_obj_t* parent) {
    (void)id;
    lv_obj_t* holder = lv_obj_create(parent != nullptr ? parent : lv_scr_act());
    lv_obj_remove_style_all(holder);
    lv_obj_set_size(holder, 1, 1);
    lv_obj_add_flag(holder, LV_OBJ_FLAG_HIDDEN);
    return holder;
}

static inline pet_theme_id_t pet_theme_current(void) { return PET_THEME_IDLE; }
static inline int pet_theme_action(pet_theme_id_t id) { return (int)id; }
static inline const char* pet_theme_name(pet_theme_id_t id) {
    static const char* names[] = { "待机", "走路", "哭", "开心", "思考",
                                    "生气", "吃东西", "打哈欠", "跳舞" };
    return (id >= 0 && id < PET_THEME_N) ? names[id] : "";
}
// 以下函数被 dafeiyu_display.cc 以 extern "C" 声明，需 C 链接实体符号。
// 实现在 pet_themes_stub.cc（用 extern "C" 定义）。
void pet_theme_show(int on);
void pet_theme_pause(bool pause);
void pet_theme_tick(uint32_t now);
void pet_theme_set_quote(const char* quote);
void pet_theme_set_fonts(const lv_font_t* big, const lv_font_t* small);

#ifdef __cplusplus
}
#endif
