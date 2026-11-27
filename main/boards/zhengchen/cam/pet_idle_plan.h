/*
 * pet_idle_plan.h —— my-bot 兼容桩（stub）
 * 原 S3-car 的待机随机动作编排，依赖角色 GIF 资源。这里保留 API 的空实现。
 */
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

static inline void pet_idle_plan_init(void) {}
static inline void pet_idle_plan_enable(bool enable) { (void)enable; }
static inline void pet_idle_plan_tick(uint32_t now, int hour) { (void)now; (void)hour; }

#ifdef __cplusplus
}
#endif
