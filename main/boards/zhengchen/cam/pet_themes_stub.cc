/*
 * pet_themes_stub.cc —— my-bot 角色主题引擎的 C 链接兼容实现
 *
 * dafeiyu_display.cc 用 `extern "C"` 声明了这些函数，原实现来自
 * S3-car 的 _themes/pet_themes.c（作者本机素材，未提交 GitHub）。
 * 这里提供空实现，让链接通过；角色显示由 RoboFace（LVGL 自绘）承担。
 */
#include "pet_themes.h"

extern "C" {

void pet_theme_show(int on) { (void)on; }
void pet_theme_pause(bool pause) { (void)pause; }
void pet_theme_tick(uint32_t now) { (void)now; }
void pet_theme_set_quote(const char* quote) { (void)quote; }
void pet_theme_set_fonts(const lv_font_t* big, const lv_font_t* small) {
    (void)big;
    (void)small;
}

}  // extern "C"
