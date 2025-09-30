#include "level_calc.h"
#include <cmath>
#include <algorithm>
#include <deque>

// Short LUFS用ウィンドウ長（100ms×30=3秒）
static constexpr size_t kShortWindowBlocks = 30;

LevelCalc::LevelCalc()
    : rms_(0.0f),
      peak_(0.0f),
      lufs_m_(-120.0f),
      lufs_short_(-120.0f),
      smoothed_lufs_short_(-120.0f),
      smoothed_lufs_m_(-120.0f),
      sampleRate_(48000) {
    // 初期化
    for (auto &v : rms_ch_) v.store(0.0f, std::memory_order_relaxed);
    for (auto &v : peak_ch_) v.store(0.0f, std::memory_order_relaxed);
    for (auto &v : lufs_m_ch_) v.store(-120.0f, std::memory_order_relaxed);
    for (auto &v : lufs_short_ch_) v.store(-120.0f, std::memory_order_relaxed);
    smoothed_lufs_short_ch_.fill(-120.0f);
    smoothed_lufs_m_ch_.fill(-120.0f);
}

LevelCalc::~LevelCalc() {}

void LevelCalc::setSampleRate(uint32_t sampleRate) {
    sampleRate_.store(sampleRate, std::memory_order_relaxed);
    hopSamples_ = static_cast<uint32_t>(
        std::max<uint32_t>(1, static_cast<uint32_t>(std::lround(sampleRate * 0.100))) );
    blockWindowSamples_ = hopSamples_ * 4; // 400ms 相当
    updateFilterCoeffs();
    resetBlockAccumulators();
}

void LevelCalc::setChannels(size_t channels) {
    channels_ = std::min(channels, kMaxChannels);
    meterChans_ = {0, 1};
    initFiltersIfNeeded(channels_);
    updateFilterCoeffs();
    sumSquaresHop_.assign(channels_, 0.0);
    recentSubblocks_.assign(channels_, {});
    rollingSubSum_.assign(channels_, 0.0);
    recentSubblocksShort_.assign(channels_, {});
    rollingSubSumShort_.assign(channels_, 0.0);
    for (size_t ch = 0; ch < kMaxChannels; ++ch) {
        rms_ch_[ch].store(0.0f, std::memory_order_relaxed);
        peak_ch_[ch].store(0.0f, std::memory_order_relaxed);
        lufs_m_ch_[ch].store(-120.0f, std::memory_order_relaxed);
        lufs_short_ch_[ch].store(-120.0f, std::memory_order_relaxed);
        smoothed_lufs_short_ch_[ch] = -120.0f;
        smoothed_lufs_m_ch_[ch] = -120.0f;
    }
    lufs_short_.store(-120.0f, std::memory_order_relaxed);
    smoothed_lufs_short_ = -120.0f;
    smoothed_lufs_m_ = -120.0f;
    resetBlockAccumulators();
}

void LevelCalc::initFiltersIfNeeded(size_t channels) {
    shelfFilters_.resize(channels);
    hp1Filters_.resize(channels);
    hp2Filters_.resize(channels);
}

void LevelCalc::resetBlockAccumulators() {
    hopSampleCount_ = 0;
    sumSquaresHop_.assign(channels_, 0.0);
    recentSubblocks_.assign(channels_, {});
    rollingSubSum_.assign(channels_, 0.0);
    recentSubblocksShort_.assign(channels_, {});
    rollingSubSumShort_.assign(channels_, 0.0);
    lufs_m_.store(-120.0f, std::memory_order_relaxed);
    lufs_short_.store(-120.0f, std::memory_order_relaxed);
    smoothed_lufs_short_ = -120.0f;
    smoothed_lufs_m_ = -120.0f;
    smoothed_lufs_short_ch_.fill(-120.0f);
    smoothed_lufs_m_ch_.fill(-120.0f);
    for (size_t ch = 0; ch < channels_; ++ch) {
        lufs_m_ch_[ch].store(-120.0f, std::memory_order_relaxed);
        lufs_short_ch_[ch].store(-120.0f, std::memory_order_relaxed);
    }
}

void LevelCalc::updateFilterCoeffs() {
    uint32_t fs = sampleRate_.load(std::memory_order_relaxed);
    if (fs == 0 || channels_ == 0) return;

    // 1st-order HP 2段 @60 Hz
    const float fc_hp = 60.f;
    float K = static_cast<float>(fs) / (static_cast<float>(M_PI) * fc_hp);
    float b0_hp = K / (K + 1.0f);
    float b1_hp = -b0_hp;
    float a1_hp = (1.0f - K) / (1.0f + K);
    for (size_t ch = 0; ch < channels_; ++ch) {
        hp1Filters_[ch].setCoeffs(b0_hp, b1_hp, a1_hp);
        hp2Filters_[ch].setCoeffs(b0_hp, b1_hp, a1_hp);
        hp1Filters_[ch].reset();
        hp2Filters_[ch].reset();
    }

    // High-shelf +4 dB @ ~1.7kHz
    const float gainDb = 4.0f;
    const float f0 = 1681.974f;
    float A = std::sqrt(std::pow(10.0f, gainDb / 20.0f));
    float w0 = 2.0f * static_cast<float>(M_PI) * f0 / static_cast<float>(fs);
    float cw = std::cos(w0);
    float sw = std::sin(w0);
    float alpha = sw * std::sqrt(2.0f) * 0.5f;
    float a0 = (A + 1.0f) - (A - 1.0f) * cw + 2.0f * std::sqrt(A) * alpha;
    float b0 = A * ((A + 1.0f) + (A - 1.0f) * cw + 2.0f * std::sqrt(A) * alpha) / a0;
    float b1 = -2.0f * A * ((A - 1.0f) + (A + 1.0f) * cw) / a0;
    float b2 = A * ((A + 1.0f) + (A - 1.0f) * cw - 2.0f * std::sqrt(A) * alpha) / a0;
    float a1 =  2.0f * ((A - 1.0f) - (A + 1.0f) * cw) / a0;
    float a2 = ((A + 1.0f) - (A - 1.0f) * cw - 2.0f * std::sqrt(A) * alpha) / a0;

    for (size_t ch = 0; ch < channels_; ++ch) {
        shelfFilters_[ch].b0 = b0;
        shelfFilters_[ch].b1 = b1;
        shelfFilters_[ch].b2 = b2;
        shelfFilters_[ch].a1 = a1;
        shelfFilters_[ch].a2 = a2;
        shelfFilters_[ch].reset();
    }
}

void LevelCalc::process(float **data, uint32_t frames, size_t channels) {
    if (!data || frames == 0 || channels == 0) return;

    if (channels_ != channels)
        setChannels(channels);

    float sum_sqr_raw = 0.0f;
    float peak = 0.0f;

    std::vector<double> sumSqrPerCh(channels_, 0.0);
    std::vector<float> peakPerCh(channels_, 0.0f);
    std::vector<double> ms100(channels_, 0.0);

    uint32_t fs = sampleRate_.load(std::memory_order_relaxed);
    if (fs == 0) fs = 48000;
    if (hopSamples_ == 0) {
        hopSamples_ = static_cast<uint32_t>(
            std::max<uint32_t>(1, static_cast<uint32_t>(std::lround(fs * 0.100))) );
        blockWindowSamples_ = hopSamples_ * 4;
    }

    for (uint32_t i = 0; i < frames; ++i) {
        for (size_t ch = 0; ch < channels_; ++ch) {
            float s = data[ch][i];
            float a = std::fabs(s);
            if (a > peak) peak = a;
            if (a > peakPerCh[ch]) peakPerCh[ch] = a;
            sum_sqr_raw += s * s;
            sumSqrPerCh[ch] += static_cast<double>(s) * static_cast<double>(s);

            // K-weighting
            float y = hp1Filters_[ch].process(s);
            y = hp2Filters_[ch].process(y);
            float y2 = shelfFilters_[ch].process(y);

            if (ch < sumSquaresHop_.size())
                sumSquaresHop_[ch] += static_cast<double>(y2) * static_cast<double>(y2);
        }

        ++hopSampleCount_;
        if (hopSampleCount_ >= hopSamples_) {
            for (size_t ch = 0; ch < channels_; ++ch) {
                double ms = (hopSamples_ > 0)
                                ? (sumSquaresHop_[ch] / static_cast<double>(hopSamples_))
                                : 0.0;
                ms100[ch] = ms;
                sumSquaresHop_[ch] = 0.0;
            }

            // 100ms 小ブロックを Momentary 用に追加
            for (size_t ch = 0; ch < channels_; ++ch) {
                double ms = ms100[ch];
                if (recentSubblocks_[ch].size() == 4) {
                    rollingSubSum_[ch] -= recentSubblocks_[ch].front();
                    recentSubblocks_[ch].pop_front();
                }
                recentSubblocks_[ch].push_back(ms);
                rollingSubSum_[ch] += ms;
            }

            // 400ms (4 subblocks) 揃ったら Momentary 計算
            bool haveWindow = true;
            for (size_t ch = 0; ch < channels_; ++ch) {
                if (recentSubblocks_[ch].size() < 4) { haveWindow = false; break; }
            }
            if (haveWindow) {
                double energySum = 0.0;
                for (size_t idx : meterChans_) {
                    if (idx >= channels_) continue;
                    energySum += (rollingSubSum_[idx] / 4.0);
                }

                const double offset = -0.691;
                double lufs_m = offset + 10.0 * std::log10(std::max(energySum, 1e-12));
                lufs_m_.store(static_cast<float>(lufs_m), std::memory_order_relaxed);

                for (size_t ch = 0; ch < channels_; ++ch) {
                    double eCh = rollingSubSum_[ch] / 4.0;
                    double lufs_m_ch = offset + 10.0 * std::log10(std::max(eCh, 1e-12));
                    lufs_m_ch_[ch].store(static_cast<float>(lufs_m_ch), std::memory_order_relaxed);
                }
                // Integrated の蓄積・ゲート処理は削除
            }

            // --- Short LUFS用 ---
            for (size_t ch = 0; ch < channels_; ++ch) {
                double ms = ms100[ch];
                if (recentSubblocksShort_[ch].size() == kShortWindowBlocks) {
                    rollingSubSumShort_[ch] -= recentSubblocksShort_[ch].front();
                    recentSubblocksShort_[ch].pop_front();
                }
                recentSubblocksShort_[ch].push_back(ms);
                rollingSubSumShort_[ch] += ms;
            }
            // --- Short LUFS計算 ---
            bool haveShortWindow = true;
            for (size_t ch = 0; ch < channels_; ++ch) {
                if (recentSubblocksShort_[ch].size() < kShortWindowBlocks) { haveShortWindow = false; break; }
            }
            if (haveShortWindow) {
                double energySumShort = 0.0;
                for (size_t idx : meterChans_) {
                    if (idx >= channels_) continue;
                    energySumShort += (rollingSubSumShort_[idx] / static_cast<double>(kShortWindowBlocks));
                }
                const double offset = -0.691;
                double lufs_short = offset + 10.0 * std::log10(std::max(energySumShort, 1e-12));
                lufs_short_.store(static_cast<float>(lufs_short), std::memory_order_relaxed);
                for (size_t ch = 0; ch < channels_; ++ch) {
                    double eCh = rollingSubSumShort_[ch] / static_cast<double>(kShortWindowBlocks);
                    double lufs_short_ch = offset + 10.0 * std::log10(std::max(eCh, 1e-12));
                    lufs_short_ch_[ch].store(static_cast<float>(lufs_short_ch), std::memory_order_relaxed);
                }
            }
            hopSampleCount_ = 0;
        }
    }
    // --- LUFS Short値のスムージング ---
    float lufs_short_now = lufs_short_.load(std::memory_order_relaxed);
    float alpha_short = 0.15f;
    if (lufs_short_now < -100.0f) {
        // まだ十分なデータが溜まっていない場合は、momentary値で初期化
        lufs_short_now = lufs_m_.load(std::memory_order_relaxed);
    }
    if (smoothed_lufs_short_ < -100.0f) smoothed_lufs_short_ = lufs_short_now;
    else smoothed_lufs_short_ = alpha_short * lufs_short_now + (1.0f - alpha_short) * smoothed_lufs_short_;
    // --- チャンネルごとのLUFS Shortスムージング ---
    for (size_t ch = 0; ch < channels_; ++ch) {
        float lufs_short_ch_now = lufs_short_ch_[ch].load(std::memory_order_relaxed);
        if (lufs_short_ch_now < -100.0f) {
            lufs_short_ch_now = lufs_m_ch_[ch].load(std::memory_order_relaxed);
        }
        if (smoothed_lufs_short_ch_[ch] < -100.0f) smoothed_lufs_short_ch_[ch] = lufs_short_ch_now;
        else smoothed_lufs_short_ch_[ch] = alpha_short * lufs_short_ch_now + (1.0f - alpha_short) * smoothed_lufs_short_ch_[ch];
    }

    // --- LUFS Momentary値のスムージング ---
    float lufs_m_now = lufs_m_.load(std::memory_order_relaxed);
    float alpha_m = 0.15f; // RMS/Peakと同等の滑らかさに調整
    if (lufs_m_now < -100.0f) {
        // 十分なウィンドウがない場合は直近のshort値で初期化
        lufs_m_now = lufs_short_.load(std::memory_order_relaxed);
    }
    if (smoothed_lufs_m_ < -100.0f) smoothed_lufs_m_ = lufs_m_now;
    else smoothed_lufs_m_ = alpha_m * lufs_m_now + (1.0f - alpha_m) * smoothed_lufs_m_;
    for (size_t ch = 0; ch < channels_; ++ch) {
        float lufs_m_ch_now = lufs_m_ch_[ch].load(std::memory_order_relaxed);
        if (lufs_m_ch_now < -100.0f) {
            lufs_m_ch_now = lufs_short_ch_[ch].load(std::memory_order_relaxed);
        }
        if (smoothed_lufs_m_ch_[ch] < -100.0f) smoothed_lufs_m_ch_[ch] = lufs_m_ch_now;
        else smoothed_lufs_m_ch_[ch] = alpha_m * lufs_m_ch_now + (1.0f - alpha_m) * smoothed_lufs_m_ch_[ch];
    }

    float denom = static_cast<float>(frames) * static_cast<float>(channels_);
    if (denom <= 0.0f) denom = 1.0f;
    float rms = std::sqrt(sum_sqr_raw / denom);
    rms_.store(rms, std::memory_order_relaxed);
    peak_.store(peak, std::memory_order_relaxed);

    for (size_t ch = 0; ch < channels_; ++ch) {
        float denomCh = static_cast<float>(frames);
        if (denomCh <= 0.0f) denomCh = 1.0f;
        float rmsCh = static_cast<float>(
            std::sqrt(sumSqrPerCh[ch] / static_cast<double>(denomCh)) );
        rms_ch_[ch].store(rmsCh, std::memory_order_relaxed);
        peak_ch_[ch].store(peakPerCh[ch], std::memory_order_relaxed);
    }
}

float LevelCalc::getRMS() const { return rms_.load(std::memory_order_relaxed); }
float LevelCalc::getPeak() const { return peak_.load(std::memory_order_relaxed); }

float LevelCalc::getLUFS() const {
    // 互換：Integrated を廃止したため momentary を返す
    return lufs_m_.load(std::memory_order_relaxed);
}
float LevelCalc::getLUFSMomentary() const {
    return lufs_m_.load(std::memory_order_relaxed);
}
float LevelCalc::getLUFSMomentaryCh(size_t ch) const {
    if (ch >= channels_) return -120.0f;
    return lufs_m_ch_[ch].load(std::memory_order_relaxed);
}

float LevelCalc::getRMSCh(size_t ch) const {
    if (ch >= channels_) return 0.0f;
    return rms_ch_[ch].load(std::memory_order_relaxed);
}
float LevelCalc::getPeakCh(size_t ch) const {
    if (ch >= channels_) return 0.0f;
    return peak_ch_[ch].load(std::memory_order_relaxed);
}
float LevelCalc::getLUFSShort() const { return lufs_short_.load(std::memory_order_relaxed); }
float LevelCalc::getSmoothedLUFSShort() const { return smoothed_lufs_short_; }
float LevelCalc::getLUFSShortCh(size_t ch) const {
    if (ch >= channels_) return -120.0f;
    return lufs_short_ch_[ch].load(std::memory_order_relaxed);
}
float LevelCalc::getSmoothedLUFSShortCh(size_t ch) const {
    if (ch >= channels_) return -120.0f;
    return smoothed_lufs_short_ch_[ch];
}

float LevelCalc::getSmoothedLUFSMomentary() const { return smoothed_lufs_m_; }

float LevelCalc::getSmoothedLUFSMomentaryCh(size_t ch) const {
    if (ch >= channels_) return -120.0f;
    return smoothed_lufs_m_ch_[ch];
}
