# dafeiyu 表情资源（占位）

原始 `assets/dafeiyu/*.gif`（17 个，约 300KB）是上游作者 mohutubiao/S3- 的本机素材，
在 GitHub 仓库中被 `.gitignore` 的 `/assets/` 规则排除，无法从公开渠道取得。

本目录下的 17 个 GIF 是**程序生成的纯色占位图**（32x32，共 6KB），仅用于：
1. 让 `dafeiyu_display.cc` 的 `AssetName()` 资源查找不报 "Emoji not found"；
2. 让 CMake 的 assets 分区打包脚本能找到对应文件、编译通过。

**真正的屏幕表情由 `xiaozhi_face_tft.h` + `robo_face.cc`（LVGL 自绘，21 种情绪）承担，
不依赖这些 GIF。** 如果你拿到了作者的原始素材，直接同名覆盖即可获得完整角色动画。

注意：不要使用第三方"大肥鱼"表情包替代 —— 那些是 CC BY-NC-SA 非商业授权，
与本工程的 MIT 许可冲突。
