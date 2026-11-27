#include <cJSON.h>
#include <esp_log.h>
#include <wifi_manager.h>

#include <cstring>
#include <cstdlib>
#include <cctype>
#include <string>

#include "application.h"
#include "board.h"
#include "config.h"
#include "mcp_server.h"
#include "sdkconfig.h"
#include "settings.h"
#include "display.h"
#include "esp_now_link.h" // self.car.distance：距离保护阈值下发（ESP-NOW → C3 从机）
#include "ws2812_led.h"   // self.led.control：板载彩灯（语音开关/颜色/律动）
#include "audio/audio_service.h"  // my-bot: 音乐播放

#define TAG "MCPController"

// ===== my-bot: Meting 点歌 helper（从 xiaozhimusic 移植，纯 HTTP+JSON，无 ADF）=====
// URL编码函数（静态函数，避免重复定义）
static std::string url_encode(const std::string& str) {
    std::string result;
    for (char c : str) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            result += c;
        } else {
            char hex[4];
            snprintf(hex, sizeof(hex), "%%%02X", (unsigned char)c);
            result += hex;
        }
    }
    return result;
}

// ============ Meting 公共音源（网易云，返回完整版 MP3）============
// 分两个职责，实测各有所长：
//   1) 搜索用 met.liiiu.cn：稳定（实测 8/8 命中，含歌手名），无速率限制。
//      但它的直链对部分歌曲只给 30 秒试听，且忽略 br 参数。
//   2) 直链/歌词用 api.qijieya.cn：返回完整版（320kbps/200+秒），br 有效。
//      但它的 search 接口有速率限制，连续请求几次后返回空数组（这就是"搜不到歌"的根因）。
// 因此：用 liiiu 搜索拿到网易云歌曲 id，再用 qijieya 取完整版直链和歌词。
static const char* METING_SEARCH_BASE   = "https://met.liiiu.cn/meting/api";
static const char* METING_SEARCH_BACKUP = "https://api.qijieya.cn/meting/";
static const char* METING_STREAM_BASE   = "https://api.qijieya.cn/meting/";

// 从 Meting 的 url 字段提取歌曲 id，形如 ...server=netease&type=url&id=259138[&br=320]
static std::string extract_meting_id(const char* url) {
    if (url == nullptr) return "";
    const char* p = strstr(url, "id=");
    if (p == nullptr) return "";
    p += 3;
    std::string id;
    while (*p >= '0' && *p <= '9') {
        id.push_back(*p);
        ++p;
    }
    return id;
}

// 兼容不同 Meting 实例的字段名：qijieya 用 name/artist，liiiu 用 title/author
static std::string json_first_string(cJSON* item, const char* key1, const char* key2) {
    cJSON* v = cJSON_GetObjectItem(item, key1);
    if (cJSON_IsString(v) && v->valuestring && v->valuestring[0] != '\0') return v->valuestring;
    if (key2 != nullptr) {
        v = cJSON_GetObjectItem(item, key2);
        if (cJSON_IsString(v) && v->valuestring && v->valuestring[0] != '\0') return v->valuestring;
    }
    return "";
}

// 请求某个 Meting 实例的搜索接口，返回 cJSON 数组（调用方负责 cJSON_Delete）；失败或空返回 nullptr
static cJSON* meting_search(NetworkInterface* network, const char* base, const std::string& encoded_keyword) {
    std::string search_url = std::string(base) + "?server=netease&type=search&id=" +
                             encoded_keyword + "&limit=5";
    auto http = network->CreateHttp(0);
    if (!http->Open("GET", search_url)) {
        http->Close();
        ESP_LOGW(TAG, "Meting search open failed: %s", base);
        return nullptr;
    }
    std::string response = http->ReadAll();
    http->Close();
    cJSON* root = cJSON_Parse(response.c_str());
    if (root == nullptr || !cJSON_IsArray(root)) {
        if (root) cJSON_Delete(root);
        ESP_LOGW(TAG, "Meting search non-array from %s: %.80s", base, response.c_str());
        return nullptr;
    }
    int n = cJSON_GetArraySize(root);
    if (n <= 0) {
        cJSON_Delete(root);
        ESP_LOGW(TAG, "Meting search empty from %s", base);
        return nullptr;
    }
    ESP_LOGI(TAG, "Meting search via %s -> %d results", base, n);
    return root;
}

// 通过 Meting 的 type=url 接口解析出真实可播放的 MP3 直链。
// api_url 形如：https://api.qijieya.cn/meting/?server=netease&type=url&id=259138
// 追加 &br=320 后请求（高码率提升音质），读取 302 的 Location 头得到最终直链。失败返回空串。
static std::string resolve_meting_url(NetworkInterface* network, const std::string& api_url) {
    std::string url = api_url + "&br=320";
    auto http = network->CreateHttp(0);
    std::string location;
    if (http->Open("GET", url)) {
        auto status = http->GetStatusCode();
        if (!status) {
            ESP_LOGW(TAG, "Meting url resolve status error: %s",
                     status.error().ToString().c_str());
        } else {
            ESP_LOGI(TAG, "Meting url resolve status=%d", *status);
            // 3xx 均视为重定向；部分实例直接 200 返回 JSON {"url": "..."} 作为兜底
            if (*status >= 300 && *status < 400) {
                location = http->GetResponseHeader("Location");
            } else if (*status == 200) {
                std::string body = http->ReadAll();
                cJSON* j = cJSON_Parse(body.c_str());
                if (j) {
                    cJSON* u = cJSON_GetObjectItem(j, "url");
                    if (cJSON_IsString(u) && u->valuestring) location = u->valuestring;
                    cJSON_Delete(j);
                }
            }
        }
    }
    http->Close();
    // 去掉首尾空白
    while (!location.empty() && (location.front() == ' ' || location.front() == '\r' || location.front() == '\n')) {
        location.erase(location.begin());
    }
    while (!location.empty() && (location.back() == ' ' || location.back() == '\r' || location.back() == '\n')) {
        location.pop_back();
    }
    // 只接受 http(s) 直链
    if (location.rfind("http://", 0) == 0 || location.rfind("https://", 0) == 0) {
        return location;
    }
    return "";
}

// 提取歌曲名称的关键词处理函数
static std::string extract_song_keyword(const std::string& input) {
    std::string result = input;

    // 检查是否存在《》
    if(input.find("《") != std::string::npos && input.find("》") != std::string::npos) {
        // 存在书名号，提取里面的内容
        size_t left_bookmark = input.find("《");
        size_t right_bookmark = input.find("》");
        if (left_bookmark != std::string::npos && right_bookmark != std::string::npos && right_bookmark > left_bookmark) {
            result = input.substr(left_bookmark + 1, right_bookmark - left_bookmark - 1);
        }
    }
    // 不存在书名号，保持原样不变
    // 移除首尾空格
    while (!result.empty() && result[0] == ' ') {
        result.erase(0, 1);
    }
    while (!result.empty() && result.back() == ' ') {
        result.pop_back();
    }

    // 泛词兜底：大模型常把"播放音乐"直接当成 keyword 传进来（如用户说"播放音乐你看到的我"
    // 却只传了"音乐"）。用泛词去搜 Meting 会命中热门歌单，第一条几乎总是"卡农"，
    // 导致每次都播同一首。这里识别泛词并返回空串，让调用方提示用户给出具体歌名。
    static const char* generic_words[] = {
        "音乐", "歌曲", "歌", "听歌", "听音乐", "放音乐", "播放音乐",
        "一首歌", "来首歌", "来一首歌", "放首歌", "放一首歌", "随便",
        "music", "song", "play music"
    };
    for (auto word : generic_words) {
        if (result == word) {
            ESP_LOGW(TAG, "Generic music keyword '%s' rejected, need a specific song name", result.c_str());
            return "";
        }
    }

    ESP_LOGI(TAG, "Original keyword: '%s' -> Extracted: '%s'", input.c_str(), result.c_str());
    return result;
}

// 某些 Meting 实例的 type=lrc 会返回 JSON 包装：{"lyric":"[00:00.00]...\n..."}
// 这里统一解包成纯文本 LRC，并还原 \n \" \\ \uXXXX 等转义，保证解析器只看到 \n 分隔的行。
static std::string extract_lrc_text(const std::string& raw) {
    size_t first = raw.find_first_not_of(" \t\r\n");
    if (first == std::string::npos || raw[first] != '{') {
        return raw;  // 已经是纯文本 LRC
    }
    const char* keys[] = {"\"lyric\"", "\"lrc\"", "\"lrcx\""};
    for (const char* key : keys) {
        size_t k = raw.find(key);
        if (k == std::string::npos) continue;
        size_t colon = raw.find(':', k + strlen(key));
        if (colon == std::string::npos) continue;
        size_t q = raw.find('"', colon + 1);
        if (q == std::string::npos) continue;

        std::string out;
        out.reserve(raw.size() / 2);
        for (size_t i = q + 1; i < raw.size(); ++i) {
            char c = raw[i];
            if (c == '"') break;
            if (c != '\\' || i + 1 >= raw.size()) {
                out += c;
                continue;
            }
            char n = raw[++i];
            switch (n) {
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                case '/': out += '/'; break;
                case 'u': {
                    if (i + 4 >= raw.size()) break;
                    unsigned code = (unsigned)strtoul(raw.substr(i + 1, 4).c_str(), nullptr, 16);
                    i += 4;
                    if (code < 0x80) {
                        out += (char)code;
                    } else if (code < 0x800) {
                        out += (char)(0xC0 | (code >> 6));
                        out += (char)(0x80 | (code & 0x3F));
                    } else {
                        out += (char)(0xE0 | (code >> 12));
                        out += (char)(0x80 | ((code >> 6) & 0x3F));
                        out += (char)(0x80 | (code & 0x3F));
                    }
                    break;
                }
                default: out += n; break;
            }
        }
        if (!out.empty()) {
            ESP_LOGI(TAG, "Unwrapped JSON lyric payload: %d -> %d bytes",
                     (int)raw.size(), (int)out.size());
            return out;
        }
    }
    return raw;
}
McpServer::McpServer() {
}

McpServer::~McpServer() {
    for (auto tool : tools_) {
        delete tool;
    }
    tools_.clear();
}

class MCPController {
public:
    MCPController() {
        RegisterMcpTools();
        ESP_LOGI(TAG, "注册MCP工具");
    }

	void RegisterMcpTools() {
        auto& mcp_server = McpServer::GetInstance();
		ESP_LOGI(TAG, "开始注册MCP工具...");

	/* 2026-11-15 精简：原来的 set_mode / get_mode 合成一个（不传 mode = 查询）。
       少一个工具、少一段描述，服务端 prompt 更短，AI 也不容易选错。 */
	mcp_server.AddTool(
        "self.AEC.mode",
        "查询或设置「语音打断」开关（它同时控制设备端 AEC 回声消除）。"
        "用户说「关掉语音打断」「打开语音打断」「现在打断开着吗」「为什么说话打断不了」时使用。\n"
        "参数：\n"
        "   `mode`: 可选。`kAecOnDeviceSide`=打开，`kAecOff`=关闭；不传 = 只查询状态\n",
        PropertyList({
            Property("mode", kPropertyTypeString, std::string(""))
        }),
        [](const PropertyList& properties) -> ReturnValue {
            auto& app = Application::GetInstance();
            const std::string mode = properties["mode"].value<std::string>();
            if (mode.empty()) {
                if (app.IsVoiceInterruptEnabled()) {
                    return std::string("{\"success\": true, \"message\": \"语音打断当前是开启的（AEC 开）\"}");
                }
                return std::string("{\"success\": true, \"message\": \"语音打断当前是关闭的（AEC 关）\"}");
            }
            /* 关/开 AEC 会重建音频通道，先等这一句播完再切 */
            vTaskDelay(pdMS_TO_TICKS(2000));
            /* 统一走 SetVoiceInterruptEnabled（内部同步 AEC + NVS），
               否则网页上的打断开关和实际 AEC 状态会分叉。 */
            if (mode == "kAecOff") {
                app.SetVoiceInterruptEnabled(false);
                return std::string("{\"success\": true, \"message\": \"语音打断已关闭（AEC 一并关闭），AI 说话时喊唤醒词不会打断\"}");
            }
            app.SetVoiceInterruptEnabled(true);
            return std::string("{\"success\": true, \"message\": \"语音打断已开启（AEC 一并开启），AI 说话时喊唤醒词可打断\"}");
        }
    );
	
    mcp_server.AddTool(
        "self.device.get_ip",
        "获取设备的 IP 地址和网页控制页地址。当用户问「你的 IP 是多少」「怎么打开控制页」"
        "「网址是什么」时使用此工具。\n"
        "返回：IP、网页控制页完整地址\n",
        PropertyList(),
        [](const PropertyList&) -> ReturnValue {
            auto& wifi = WifiManager::GetInstance();
            std::string ip = wifi.GetIpAddress();
            if (ip.empty()) {
                return "{\"success\": false, \"message\": \"当前没有连上 Wi-Fi，拿不到 IP\"}";
            }
            char buf[192];
            snprintf(buf, sizeof(buf),
                     "{\"success\": true, \"ip\": \"%s\", \"web\": \"http://%s\", "
                     "\"message\": \"我的 IP 是 %s，浏览器打开 http://%s 可以进网页控制页\"}",
                     ip.c_str(), ip.c_str(), ip.c_str(), ip.c_str());
            return std::string(buf);
        }
    );

    /* 2026-11-15 精简：原来的 self.led.control 已删除，能力并入板级的
       self.led.set（颜色/亮度/效果 + 新增 dance 参数），避免两个灯工具
       让 AI 选错、也省一段 prompt。 */

    // ⚠️ 2026-10-31：这个工具【默认不再注册】。
    //
    // 原因：只要注册了，服务端 AI 在任意一轮对话里都可能判定「用户想重启」
    //       并调用它，实际表现就是「一按 BOOT / 一语音唤醒就重启」，设备几乎
    //       没法正常使用。一台随时会被远端_reset 的设备，比没有远程重启能力糟得多。
    //
    // 恢复办法：把下面的 #if 0 改成 #if 1 重新编译即可 —— 但请先确认你能接受
    //       「AI 随时可能把设备重启」这个风险。真需要远程重启的话，更合适的做法
    //       是让工具只返回一个「请求已记录」，由用户在网页上点确认后才真正重启。
#if 0
    mcp_server.AddTool(
        "self.res.esp_restart",
        "重启设备。当用户意图重启设备时使用此工具。\n",
        PropertyList(),
        [](const PropertyList&) -> ReturnValue {
            ESP_LOGW(TAG, "!!! esp_restart 由 MCP 工具调用（服务端请求重启），非崩溃 !!!");
            vTaskDelay(pdMS_TO_TICKS(1000));
            // Reboot the device
            esp_restart();
            return true;
        }
    );
#endif

    /* ── 2026-11-12 距离保护（GY-530 / VL53L0X，装在 C3 从机上）──
       传感器在从机，阈值由主机下发（ESP-NOW 配置包），从机存 NVS。
       单位对外用厘米（用户说话就是「30 厘米」），内部换算成毫米。 */
    mcp_server.AddTool(
        "self.car.distance",
        "设置或查询小车的激光距离保护（GY-530）：防碰撞 + 防跌落。"
        "用户说「碰撞距离设成 30 厘米」「跌落阈值调大」「关掉防撞」「距离保护现在多少」时使用。\n"
        "用户问「刚才为什么停住」「为什么不走了」「是什么拦住你的」「撞到东西了吗」时，"
        "也必须用 action=get：返回里的 last_block 就是最后一次拦截原因（碰撞还是跌落、距离多远、多久之前）。\n"
        "参数：\n"
        "   `action`: get(查询) / set(修改) / on(开启) / off(关闭)\n"
        "   `obstacle_cm`: action=set 时填，小于等于这个距离就判定前方有障碍并拒绝前进，单位厘米（3~200）\n"
        "   `fall_cm`: action=set 时填，大于等于这个距离判定前方悬空/跌落并拒绝前进，单位厘米（8~800）\n"
        "返回值：\n"
        "   当前设置与下发结果，直接播报给用户\n",
        PropertyList({
            Property("action", kPropertyTypeString),
            /* ⚠️ 必须带默认值：value<int>() 是 std::get，属性缺失时 variant 里
               是 bool → bad_variant_access 直接 abort。 */
            Property("obstacle_cm", kPropertyTypeInteger, 0),
            Property("fall_cm", kPropertyTypeInteger, 0),
        }),
        [](const PropertyList& properties) -> ReturnValue {
            const auto action = properties["action"].value<std::string>();

            /* 主机侧记一份，用于 get；从机自己也会存 NVS，双向都不会丢 */
            Settings s("car_guard", true);
            int obs_mm = s.GetInt("obs_mm", 60);
            int fall_mm = s.GetInt("fall_mm", 450);
            bool enabled = s.GetInt("enabled", 1) != 0;

            char buf[768];
            if (action == "get") {
                BlockInfo blk;
                const bool has_blk = EspNowLink::Get().LastBlock(&blk);
                LinkStatus st;
                const bool linked = EspNowLink::Get().Status(&st);
                const char* base = enabled ? "已开启" : "已关闭";

                char link_txt[128];
                if (linked) {
                    snprintf(link_txt, sizeof(link_txt),
                             "从机在线，当前距离 %.1f 厘米，传感器%s",
                             st.mm / 10.0,
                             (st.flags & TLM_FLAG_SENSOR) ? "正常" : "未就绪");
                } else {
                    snprintf(link_txt, sizeof(link_txt), "从机离线，拿不到实时距离");
                }

                char blk_txt[192];
                if (has_blk) {
                    snprintf(blk_txt, sizeof(blk_txt),
                             "；最后一次是%s，距离 %.1f 厘米，%.0f 秒前，本次开机一共拦了 %d 次",
                             blk.type == 2 ? "前方悬空被防跌落拦住"
                                           : "前方有障碍被防碰撞拦住",
                             blk.mm / 10.0, blk.age_ms / 1000.0, blk.count);
                } else {
                    snprintf(blk_txt, sizeof(blk_txt), "；本次开机还没被拦过");
                }

                /* 网页设的值可能被从机「地面基线夹紧」改掉，
                   播报时用从机实际上报的阈值，别让用户以为设的就是跑的 */
                const int live_obs = linked ? st.obstacle_mm : obs_mm;
                const int live_fall = linked ? st.fall_mm : fall_mm;
                snprintf(buf, sizeof(buf),
                         "{\"success\": true, \"enabled\": %s, \"obstacle_cm\": %.1f, "
                         "\"fall_cm\": %.1f, \"obstacle_live_cm\": %.1f, "
                         "\"fall_live_cm\": %.1f, \"linked\": %s, \"distance_cm\": %.1f, "
                         "\"sensor_ok\": %s, \"last_block\": %s, \"block_count\": %d, "
                         "\"message\": \"距离保护%s；从机实际阈值 碰撞 %d 厘米 / 跌落 %d 厘米；%s%s\"}",
                         enabled ? "true" : "false", obs_mm / 10.0, fall_mm / 10.0,
                         live_obs / 10.0, live_fall / 10.0,
                         linked ? "true" : "false", linked ? st.mm / 10.0 : 0.0,
                         (linked && (st.flags & TLM_FLAG_SENSOR)) ? "true" : "false",
                         has_blk ? (blk.type == 2 ? "\"跌落\"" : "\"碰撞\"") : "null",
                         has_blk ? blk.count : 0,
                         base, live_obs / 10, live_fall / 10, link_txt, blk_txt);
                return std::string(buf);
            }
            if (action == "on" || action == "off") {
                enabled = (action == "on");
            } else if (action == "set") {
                int obs_cm = properties["obstacle_cm"].value<int>();
                int fall_cm = properties["fall_cm"].value<int>();
                if (obs_cm > 0) obs_mm = obs_cm * 10;
                if (fall_cm > 0) fall_mm = fall_cm * 10;
            } else {
                return "{\"success\": false, \"message\": \"不认识这个动作，只能是 get/set/on/off\"}";
            }

            if (obs_mm < 30) obs_mm = 30;
            if (obs_mm > 2000) obs_mm = 2000;
            if (fall_mm < obs_mm + 50) fall_mm = obs_mm + 50;
            if (fall_mm > 8000) fall_mm = 8000;

            if (!EspNowLink::Get().SendConfig(obs_mm, fall_mm, enabled)) {
                return "{\"success\": false, \"message\": \"指令没发出去，ESP-NOW 还没启动（要先进入小车/遥控模式，并且连上 WiFi）\"}";
            }

            s.SetInt("obs_mm", obs_mm);
            s.SetInt("fall_mm", fall_mm);
            s.SetInt("enabled", enabled ? 1 : 0);
            s.Commit();   /* ⚠️ 不 Commit 就重启会丢（踩过） */

            snprintf(buf, sizeof(buf),
                     "{\"success\": true, \"enabled\": %s, \"obstacle_cm\": %.1f, "
                     "\"fall_cm\": %.1f, \"message\": \"距离保护%s：障碍 %d 厘米，跌落 %d 厘米\"}",
                     enabled ? "true" : "false", obs_mm / 10.0, fall_mm / 10.0,
                     enabled ? "已开启" : "已关闭", obs_mm / 10, fall_mm / 10);
            return std::string(buf);
        }
    );

    /* ── 2026-11-12 探索模式：从机自主溜达 + 自动避障/防跌落 ──
       逻辑在从机（C3）上跑，主机只发启停命令。 */
    mcp_server.AddTool(
        "self.car.explore",
        "让小车自己探索、随机溜达（遇障自动转向，临边先倒退再转向）。"
        "用户说「去探索一下」「自己走走」「随便转转」「探索五分钟」「别转了/停下来」时使用。\n"
        "参数：\n"
        "   `action`: start(开始) / stop(停止) / status(查询)\n"
        "   `duration_s`: 探索多少秒，默认 120，最长 3600\n"
        "   `speed_pct`: 速度百分比 20~100，默认 100\n"
        "注意：探索期间用户给任何手动指令（网页摇杆/语音控制）都会立即退出探索。\n",
        PropertyList({
            Property("action", kPropertyTypeString),
            Property("duration_s", kPropertyTypeInteger, 0),
            Property("speed_pct", kPropertyTypeInteger, 0),
        }),
        [](const PropertyList& p) -> ReturnValue {
            const auto action = p["action"].value<std::string>();
            int dur = p["duration_s"].value<int>();
            int spd = p["speed_pct"].value<int>();
            if (dur <= 0) dur = 120;
            if (spd <= 0) spd = 100;

            auto& link = EspNowLink::Get();
            char buf[224];

            if (action == "stop") {
                if (!link.SendExplore(0, spd, dur)) {
                    return "{\"success\":false,\"message\":\"指令没发出去，ESP-NOW 没启动\"}";
                }
                return "{\"success\":true,\"message\":\"已停止探索\"}";
            }

            if (action == "status") {
                LinkStatus st;
                const bool linked = link.Status(&st);
                const bool running = linked && ((st.flags & TLM_FLAG_EXPLORE) != 0);
                snprintf(buf, sizeof(buf),
                         "{\"success\":true,\"running\":%s,\"linked\":%s,\"message\":\"%s\"}",
                         running ? "true" : "false", linked ? "true" : "false",
                         running ? "正在探索" : (linked ? "没有在探索" : "从机离线"));
                return std::string(buf);
            }

            if (action != "start") {
                return "{\"success\":false,\"message\":\"action 只能是 start/stop/status\"}";
            }
            if (!link.SendExplore(1, spd, dur)) {
                return "{\"success\":false,\"message\":\"指令没发出去，ESP-NOW 没启动（先进入小车模式并连上 WiFi）\"}";
            }
            snprintf(buf, sizeof(buf),
                     "{\"success\":true,\"message\":\"开始探索 %d 秒，速度 %d%%\"}", dur, spd);
            return std::string(buf);
        }
    );

    /* ── 2026-11-12 语音打断开关 ── */
    mcp_server.AddTool(
        "self.voice.interrupt",
        "打开或关闭「语音打断」：AI 说话时喊它能否立刻打断。"
        "用户说「打开语音打断」「别插话 / 别打断你」「等我说完」「能不能打断你」时使用。\n"
        "参数：\n"
        "   `action`: get(查询) / on(允许打断) / off(不允许打断)\n",
        PropertyList({
            Property("action", kPropertyTypeString),
        }),
        [](const PropertyList& p) -> ReturnValue {
            auto& app = Application::GetInstance();
            const auto action = p["action"].value<std::string>();
            if (action == "on") {
                app.SetVoiceInterruptEnabled(true);
            } else if (action == "off") {
                app.SetVoiceInterruptEnabled(false);
            }
            char buf[192];
            snprintf(buf, sizeof(buf),
                     "{\"success\":true,\"interrupt\":%s,\"message\":\"语音打断%s\"}",
                     app.IsVoiceInterruptEnabled() ? "true" : "false",
                     app.IsVoiceInterruptEnabled()
                         ? "已打开，AI 说话时可以直接打断"
                         : "已关闭，AI 会说完再响应你");
            return std::string(buf);
        }
    );


    // ===== my-bot: 音乐点歌工具 =====
    // 添加音乐搜索和播放工具
    AddTool("test_search_music",
        "Search and play a specific song. Use this tool when the user asks to play a song. The keyword MUST be the concrete song name (and optionally the artist), e.g. '晴天 周杰伦', '孤勇者', '卡农'. NEVER pass a generic word like '音乐' or '歌曲' as the keyword - if the user did not tell you a specific song name, ask them which song they want instead of calling this tool. Searches and plays the best matching result.",
        PropertyList({
            Property("keyword", kPropertyTypeString, "Music search keyword")
        }),
        [](const PropertyList& properties) -> ReturnValue {
            auto keyword = properties["keyword"].value<std::string>();
            ESP_LOGI(TAG, "Original search keyword: %s", keyword.c_str());

            // 提取歌曲名称关键词
            std::string extracted_keyword = extract_song_keyword(keyword);
            if (extracted_keyword.empty()) {
                // 泛词（如"音乐"）会导致每次都播同一首热门歌曲，这里明确拒绝并引导用户给出歌名
                ESP_LOGW(TAG, "Empty/generic keyword, asking user for a specific song name");
                return "{\"success\": false, \"message\": \"你没有告诉我想听哪首歌。请说出具体的歌曲名（可以说「播放《晴天》」或「播放周杰伦的稻香」），我才能帮你播放\"}";
            }
            ESP_LOGI(TAG, "Searching for music with extracted keyword: %s", extracted_keyword.c_str());

            std::string encoded_keyword = url_encode(extracted_keyword);
            auto& board = Board::GetInstance();
            auto network = board.GetNetwork();

            // ===== Meting 公共音源（网易云）=====
            // 1) 搜索：liiiu 稳定优先（实测 8/8 命中，含歌手名），失败重试一次，再退 qijieya。
            //    qijieya 的 search 有速率限制（连续几次后返回空数组），这是之前"搜不到歌"的根因。
            cJSON* root = nullptr;
            for (int attempt = 0; attempt < 2 && root == nullptr; ++attempt) {
                root = meting_search(network, METING_SEARCH_BASE, encoded_keyword);
            }
            if (root == nullptr) {
                ESP_LOGW(TAG, "Primary search exhausted, falling back to backup instance");
                root = meting_search(network, METING_SEARCH_BACKUP, encoded_keyword);
            }
            if (root == nullptr) {
                ESP_LOGE(TAG, "All Meting search instances failed for keyword: %s", extracted_keyword.c_str());
                return "{\"success\": false, \"message\": \"搜索音乐失败，请稍后再试\"}";
            }

            std::string found_music_url;
            std::string found_song;
            std::string found_singer;
            std::string found_id;         // 网易云歌曲 id，用于取完整版直链和歌词
            std::string lrc_fallback_url; // 搜索结果自带的 lrc 链接（JSON 释放前保存）
            int count = cJSON_GetArraySize(root);
            ESP_LOGI(TAG, "Meting search returned %d results", count);

            // 2) 逐个候选：提取 id -> 用 qijieya 取完整版直链（320kbps，不受 search 限流影响）。
            //    最多尝试前 6 个，避免付费/无版权歌曲（每首多一次 HTTP 请求）拖慢响应。
            int max_try = count < 6 ? count : 6;
            for (int i = 0; i < max_try; ++i) {
                cJSON* item = cJSON_GetArrayItem(root, i);
                // 兼容 qijieya(name/artist) 与 liiiu(title/author) 两种字段命名
                std::string song = json_first_string(item, "name", "title");
                std::string singer = json_first_string(item, "artist", "author");
                cJSON* api_url = cJSON_GetObjectItem(item, "url");
                std::string id = extract_meting_id(cJSON_IsString(api_url) ? api_url->valuestring : nullptr);
                cJSON* lrc = cJSON_GetObjectItem(item, "lrc");
                if (cJSON_IsString(lrc) && lrc->valuestring && lrc_fallback_url.empty()) {
                    lrc_fallback_url = lrc->valuestring;
                }

                std::string direct;
                if (!id.empty()) {
                    // 优先 qijieya 取完整版（br=320 由 resolve_meting_url 追加）
                    std::string stream_api = std::string(METING_STREAM_BASE) +
                        "?server=netease&type=url&id=" + id;
                    direct = resolve_meting_url(network, stream_api);
                }
                if (direct.empty() && cJSON_IsString(api_url) && api_url->valuestring) {
                    // 回退：用搜索结果自带 url（可能是 liiiu，部分为试听，总比没有强）
                    direct = resolve_meting_url(network, api_url->valuestring);
                }
                if (direct.empty()) {
                    ESP_LOGW(TAG, "Meting result %d (%s) has no playable url, trying next", i, song.c_str());
                    continue;
                }

                found_music_url = direct;
                found_song = song;
                found_singer = singer;
                found_id = id;
                ESP_LOGI(TAG, "Candidate %d picked: %s - %s (id=%s)", i, song.c_str(), singer.c_str(), id.c_str());
                break;  // 找到可用链接
            }
            cJSON_Delete(root);

            // 歌词链接：优先用 id 走 qijieya 的 type=lrc（纯文本 LRC），否则退回搜索结果自带链接
            std::string found_lrc_url;
            if (!found_id.empty()) {
                found_lrc_url = std::string(METING_STREAM_BASE) +
                    "?server=netease&type=lrc&id=" + found_id;
            } else {
                found_lrc_url = lrc_fallback_url;
            }

            if (!found_music_url.empty()) {
                ESP_LOGI(TAG, "Found playable music: %s - %s", found_song.c_str(), found_singer.c_str());
                ESP_LOGI(TAG, "Direct MP3 url: %.160s", found_music_url.c_str());

                auto& audio_service = Application::GetInstance().GetAudioService();
                audio_service.PlayMusicFromUrl(found_music_url);

                // 下载歌词（Meting 的 lrc 接口返回纯文本 LRC），传给 AudioService 驱动显示。
                // 放在播放启动之后，避免歌词下载延迟拖慢起播；失败不影响音乐。
                std::string lrc_text;
                if (!found_lrc_url.empty()) {
                    auto lrc_http = network->CreateHttp(0);
                    if (lrc_http->Open("GET", found_lrc_url)) {
                        auto lrc_status = lrc_http->GetStatusCode();
                        if (lrc_status && *lrc_status == 200) {
                            lrc_text = extract_lrc_text(lrc_http->ReadAll());
                            ESP_LOGI(TAG, "Lyrics downloaded: %d bytes, head: %.60s",
                                     (int)lrc_text.size(), lrc_text.c_str());
                        } else {
                            ESP_LOGW(TAG, "Lyrics download status=%d",
                                     lrc_status ? *lrc_status : -1);
                        }
                    } else {
                        ESP_LOGW(TAG, "Lyrics download open failed");
                    }
                    lrc_http->Close();
                }
                // 即使没有歌词也调用一次：屏幕至少显示歌名，避免"什么都没有"
                audio_service.SetLyrics(lrc_text, found_song, found_singer);

                // 音乐播放时用 idle 状态：保持唤醒词开启，允许喊"小智"打断
                Application::GetInstance().SetDeviceState(kDeviceStateIdle);

                std::string result = "{\"success\": true, \"song\": \"";
                result += found_song;
                result += "\", \"artist\": \"";
                result += found_singer;
                result += "\", \"message\": \"开始播放音乐\"}";
                return result;
            }

            return "{\"success\": false, \"message\": \"搜索音乐失败，没有找到可播放的链接\"}";
        });

    // 添加音乐控制工具
    AddTool("control_music",
        "Control music playback. Use this tool to stop, pause, or control music playback.",
        PropertyList({
            Property("action", kPropertyTypeString, "Control action: play, pause, stop, next, previous")
        }),
        [](const PropertyList& properties) -> ReturnValue {
            auto action = properties["action"].value<std::string>();
            auto& audio_service = Application::GetInstance().GetAudioService();

            if (action == "stop") {
                audio_service.StopMusic();
                return "{\"success\": true, \"message\": \"音乐已停止\"}";
            } else if (action == "pause") {
                audio_service.StopMusic();
                return "{\"success\": true, \"message\": \"音乐已暂停\"}";
            } else {
                return "{\"success\": false, \"message\": \"不支持的操作\"}";
            }
        });

        ESP_LOGI(TAG, "MCP工具注册完成");
    }

};

static MCPController* g_mcp_controller = nullptr;

void InitializeMCPController() {
    if (g_mcp_controller == nullptr) {
        g_mcp_controller = new MCPController();
        ESP_LOGI(TAG, "注册MCP工具");
    }
}