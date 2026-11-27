/*
 * music_player.h —— my-bot 的 MP3 软解播放器（去 ESP-ADF 化）
 *
 * 设计要点：
 *   1. 用 esp_audio_codec 的 esp_mp3_dec_* 把网络 MP3 解成 PCM；
 *   2. 不碰 I2S —— 解码后的 PCM 通过回调送进官方 AudioService 播放队列，
 *      与 TTS/提示音共用 ES8311 这一条链路；
 *   3. 不依赖 ESP-ADF，跟随官方 IDF 6.x。
 */
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

class MusicPlayer {
public:
    using PcmCallback = std::function<void(std::vector<int16_t>&)>;
    using ProgressCallback = std::function<void(int position_ms)>;
    using FinishedCallback = std::function<void()>;

    static MusicPlayer& GetInstance();

    void SetPcmCallback(PcmCallback cb) { pcm_cb_ = std::move(cb); }
    void SetProgressCallback(ProgressCallback cb) { progress_cb_ = std::move(cb); }
    void SetFinishedCallback(FinishedCallback cb) { finished_cb_ = std::move(cb); }

    // 设置输出采样率（与 AudioCodec::output_sample_rate() 一致，默认 24000）
    void SetOutputSampleRate(int rate) { output_sample_rate_ = rate; }

    // 开始播放网络 MP3（异步，内部任务拉流+解码）。同一时刻只支持一首。
    bool PlayUrl(const std::string& url);

    // 停止播放并等待解码任务退出（换歌/唤醒打断时调用）
    void Stop();

    bool IsPlaying() const { return playing_; }
    const std::string& CurrentUrl() const { return url_; }

private:
    MusicPlayer() = default;
    ~MusicPlayer() = default;
    MusicPlayer(const MusicPlayer&) = delete;
    MusicPlayer& operator=(const MusicPlayer&) = delete;

    static void DecodeTaskEntry(void* arg);
    void DecodeTask();

    // 线性插值重采样（单声道，含跨块相位连续）
    std::vector<int16_t> Resample(const std::vector<int16_t>& src, int src_rate, int dst_rate);

    void* dec_handle_ = nullptr;       // esp_mp3_dec handle
    volatile bool playing_ = false;
    volatile bool task_exited_ = true;
    int output_sample_rate_ = 24000;
    int64_t samples_played_ = 0;       // 已送出的采样数 → 歌词进度
    double resample_phase_ = 0.0;      // 重采样跨块相位（源域小数位置）
    std::string url_;
    TaskHandle_t task_handle_ = nullptr;

    PcmCallback pcm_cb_;
    ProgressCallback progress_cb_;
    FinishedCallback finished_cb_;
};
