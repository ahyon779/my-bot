/*
 * music_player.cc —— MP3 软解播放核心
 *
 * HTTP 拉流 → esp_mp3_dec 解码 → 重采样 → 回调 PCM 给 AudioService 播放队列。
 * 全程不碰 I2S/ADF，与语音共用官方 codec。
 */
#include "music_player.h"

#include "board.h"
#include <esp_log.h>
#include <esp_heap_caps.h>
#include <cstring>

#include "esp_audio_dec.h"
#include "decoder/impl/esp_mp3_dec.h"

#define TAG "MusicPlayer"

// MP3 一帧 1152 采样；这里按立体声 2 声道留缓冲
static constexpr int kRawChunkSize = 4096;
static constexpr int kPcmFrameSamples = 1152 * 2;
static constexpr int kTaskStackSize = 8192;
static constexpr int kTaskPriority = 5;

MusicPlayer& MusicPlayer::GetInstance() {
    static MusicPlayer instance;
    return instance;
}

bool MusicPlayer::PlayUrl(const std::string& url) {
    if (url.empty()) {
        ESP_LOGE(TAG, "Empty music url");
        return false;
    }
    // 换歌前先停掉上一首，等解码任务真正退出（dec_handle 非线程安全）
    Stop();

    url_ = url;
    samples_played_ = 0;
    resample_phase_ = 0.0;
    playing_ = true;
    task_exited_ = false;

    xTaskCreate(DecodeTaskEntry, "music_decode", kTaskStackSize, this, kTaskPriority,
                &task_handle_);
    ESP_LOGI(TAG, "Start playing: %.150s", url.c_str());
    return true;
}

void MusicPlayer::Stop() {
    if (!playing_ && task_exited_) {
        return;
    }
    playing_ = false;
    // 等待解码任务自行退出（它每轮循环都会检查 playing_；HTTP Read 有超时）
    for (int i = 0; i < 100 && !task_exited_; ++i) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    if (!task_exited_) {
        ESP_LOGW(TAG, "Decode task did not exit in time, still waiting");
        for (int i = 0; i < 100 && !task_exited_; ++i) {
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    }
    task_handle_ = nullptr;
}

void MusicPlayer::DecodeTaskEntry(void* arg) {
    static_cast<MusicPlayer*>(arg)->DecodeTask();
}

void MusicPlayer::DecodeTask() {
    esp_audio_err_t ret;

    // 1. 打开/复用解码器
    if (dec_handle_ == nullptr) {
        ret = esp_mp3_dec_open(nullptr, 0, &dec_handle_);
        if (ret != ESP_AUDIO_ERR_OK || dec_handle_ == nullptr) {
            ESP_LOGE(TAG, "Failed to open MP3 decoder: %d", ret);
            playing_ = false;
            task_exited_ = true;
            vTaskDelete(nullptr);
            return;
        }
    } else {
        esp_mp3_dec_reset(dec_handle_);
    }

    // 2. HTTP 拉流（官方网络接口，复用 mcp_tools/assets 的写法）
    auto network = Board::GetInstance().GetNetwork();
    if (network == nullptr) {
        ESP_LOGE(TAG, "Network unavailable");
        playing_ = false;
        task_exited_ = true;
        vTaskDelete(nullptr);
        return;
    }
    auto http = network->CreateHttp(0);  // 0 = 小缓冲，边下边播
    http->SetTimeout(15000);

    auto opened = http->Open("GET", url_);
    if (!opened) {
        ESP_LOGE(TAG, "HTTP open failed: %s", opened.error().ToString().c_str());
        http->Close();
        playing_ = false;
        task_exited_ = true;
        vTaskDelete(nullptr);
        return;
    }
    auto status = http->GetStatusCode();
    if (!status || *status != 200) {
        ESP_LOGE(TAG, "HTTP status bad: %d", status ? *status : -1);
        http->Close();
        playing_ = false;
        task_exited_ = true;
        vTaskDelete(nullptr);
        return;
    }

    ESP_LOGI(TAG, "Streaming started, content-length=%u", (unsigned)http->GetBodyLength());

    // 3. 边读边解
    std::vector<uint8_t> raw_buf;
    raw_buf.reserve(kRawChunkSize + 4096);
    std::vector<int16_t> pcm(kPcmFrameSamples);

    while (playing_) {
        uint8_t chunk[kRawChunkSize];
        auto read_ret = http->Read(chunk, sizeof(chunk));
        if (!read_ret) {
            ESP_LOGE(TAG, "HTTP read failed: %s", read_ret.error().ToString().c_str());
            break;
        }
        int n = *read_ret;
        if (n == 0) break;  // 流结束

        // 把新数据接到上一帧未消费的尾部
        raw_buf.insert(raw_buf.end(), chunk, chunk + n);

        size_t offset = 0;
        while (playing_ && offset < raw_buf.size()) {
            esp_audio_dec_in_raw_t in = {
                .buffer = raw_buf.data() + offset,
                .len = (uint32_t)(raw_buf.size() - offset),
                .consumed = 0,
                .frame_recover = ESP_AUDIO_DEC_RECOVERY_NONE,
            };
            esp_audio_dec_out_frame_t out = {
                .buffer = (uint8_t*)pcm.data(),
                .len = (uint32_t)(pcm.size() * sizeof(int16_t)),
                .needed_size = 0,
                .decoded_size = 0,
            };
            esp_audio_dec_info_t info = {};
            ret = esp_mp3_dec_decode(dec_handle_, &in, &out, &info);

            if (in.consumed == 0 && ret != ESP_AUDIO_ERR_OK) {
                // 无法前进，防止死循环：丢掉一个字节重新同步
                offset += 1;
                continue;
            }
            offset += in.consumed;

            if (ret == ESP_AUDIO_ERR_OK && out.decoded_size > 0) {
                int samples = out.decoded_size / sizeof(int16_t);
                int src_rate = info.sample_rate > 0 ? info.sample_rate : 44100;

                std::vector<int16_t> decoded(pcm.begin(), pcm.begin() + samples);
                // MP3 解码通常是立体声；官方 codec 输出单声道，做声道_downmix
                // info.channel 为 2 时，取左右声道平均
                if (info.channel == 2) {
                    std::vector<int16_t> mono(samples / 2);
                    for (int i = 0; i < (int)mono.size(); ++i) {
                        int32_t sum = (int32_t)decoded[i * 2] + (int32_t)decoded[i * 2 + 1];
                        mono[i] = (int16_t)(sum / 2);
                    }
                    decoded = std::move(mono);
                    samples = decoded.size();
                }

                if (src_rate != output_sample_rate_) {
                    decoded = Resample(decoded, src_rate, output_sample_rate_);
                }

                samples_played_ += decoded.size();
                if (pcm_cb_) pcm_cb_(decoded);
                if (progress_cb_) {
                    progress_cb_((int)(samples_played_ * 1000 / output_sample_rate_));
                }
            }
        }
        // 保留未消费的尾部，供下一块数据继续解析
        if (offset > 0) {
            raw_buf.erase(raw_buf.begin(), raw_buf.begin() + offset);
        }
        if (raw_buf.size() > kRawChunkSize * 2) {
            // 异常膨胀（比如一直解不出帧），丢弃防止 OOM
            raw_buf.clear();
        }
    }

    http->Close();
    ESP_LOGI(TAG, "Decode task finished, played %.1fs",
             samples_played_ * 1.0 / output_sample_rate_);
    playing_ = false;
    task_exited_ = true;
    if (finished_cb_) finished_cb_();
    vTaskDelete(nullptr);
}

std::vector<int16_t> MusicPlayer::Resample(const std::vector<int16_t>& src,
                                            int src_rate, int dst_rate) {
    if (src.empty() || src_rate == dst_rate) return src;

    // 线性插值 + 跨块相位连续：
    // phase 是"当前输出采样对应源域的位置"（相对本块起点），每输出一个样本前进
    // src_rate/dst_rate。超出本块的部分作为下一块的起始相位保留，避免咔哒声。
    double step = (double)src_rate / dst_rate;
    double phase = resample_phase_;   // 上一块遗留的源域小数位置
    std::vector<int16_t> out;
    out.reserve((size_t)(src.size() / step) + 2);

    while (phase < (double)src.size()) {
        size_t idx = (size_t)phase;
        double frac = phase - idx;
        int32_t a = idx < src.size() ? src[idx] : 0;
        int32_t b = (idx + 1) < src.size() ? src[idx + 1] : a;
        out.push_back((int16_t)(a + (b - a) * frac));
        phase += step;
    }
    resample_phase_ = phase - src.size();  // 遗留到下一块
    return out;
}
