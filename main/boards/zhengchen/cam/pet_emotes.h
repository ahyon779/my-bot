/*
 * pet_emotes.h —— my-bot 兼容桩（stub）
 *
 * 原 S3-car 的 _emotes/pet_emotes.c 是角色 GIF 动画引擎，依赖作者本机素材。
 * 这里提供最小可编译实现：API 保留，但不播 GIF —— 表情由 RoboFace
 * （xiaozhi_face_tft.h，21 种情绪，LVGL 自绘）承担。
 */
#pragma once

#include <lvgl.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef int pet_emote_t;

static inline void pet_emotes_init(lv_obj_t* parent) { (void)parent; }
static inline void pet_emotes_set(pet_emote_t emote) { (void)emote; }
static inline void pet_emotes_rebase(void) {}
static inline void pet_emotes_tick(uint32_t now) { (void)now; }

#ifdef __cplusplus
}
#endif
