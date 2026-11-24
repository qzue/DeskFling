#pragma once
// Procedural copyright-free billiards acoustic synthesizer & 8-voice polyphonic WinMM player.
// Pre-builds tiny float32 PCM waveforms once in memory (no external audio files);
// degrades silently to no-op if no output device is available.

// clang-format off
#include <windows.h>
#include <mmsystem.h>
// clang-format on

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <span>
#include <vector>

#include "core/modes/game_mode.h"

namespace dpp::winmirror {

class BilliardsAudio {
    static constexpr std::uint32_t kSampleRate = 44100u;
    static constexpr std::size_t kVoiceCount = 8u;
    static constexpr std::size_t kMaxRenderFrames = 32768u;  // > 0.500s / 0.78 pitch @ 44.1 kHz

    struct Voice {
        HWAVEOUT handle = nullptr;
        WAVEHDR header{};
        std::vector<std::int16_t> stereoPcm;
        bool active = false;
    };

    std::vector<float> cueStrike_;
    std::vector<float> ballSoft_;
    std::vector<float> ballMed_;
    std::vector<float> ballHard_;
    std::vector<float> railBounce_;
    std::vector<float> pocketDrop_;
    std::vector<float> slingStretch_;
    std::vector<float> slingRelease_;
    std::vector<float> woodImpact_;
    std::vector<float> stoneImpact_;
    std::vector<float> glassShatter_;
    std::vector<float> targetPop_;
    std::vector<float> malletStrike_;
    std::vector<float> hockeyGoal_;
    std::vector<float> iconTap_;
    std::vector<float> iconThud_;
    std::vector<float> iconGrab_;
    std::vector<float> blackHoleHum_;
    std::vector<float> blackHoleSwallow_;
    std::array<Voice, kVoiceCount> voices_{};
    std::size_t nextVoice_ = 0;
    std::uint64_t played_ = 0;
    bool prepared_ = false;
    bool engineFailed_ = false;

    static float nextNoise(std::uint32_t& state) {
        state = state * 1664525u + 1013904223u;
        return static_cast<float>(static_cast<std::int32_t>(state)) / 2147483648.0f;
    }

    template <typename FillFn>
    static std::vector<float> makeBuffer(double durationSec, FillFn&& fill) {
        const auto frames = static_cast<std::size_t>(
            std::max(64.0, std::round(durationSec * static_cast<double>(kSampleRate))));
        std::vector<float> data(frames, 0.0f);
        fill(data.data(), frames);
        const std::size_t fade = std::min<std::size_t>(frames / 6u, 96u);
        for (std::size_t i = 0; i < fade; ++i) {
            const float w = static_cast<float>(i) / static_cast<float>(fade);
            data[frames - 1u - i] *= w;
        }
        return data;
    }

    static std::vector<float> makeBallClack(float hardness) {
        return makeBuffer(0.080, [hardness](float* out, std::size_t n) {
            std::uint32_t rng = 0x51a7c0deu + static_cast<std::uint32_t>(hardness * 1000.0f);
            constexpr double twoPi = 6.283185307179586;
            const double hard = static_cast<double>(hardness);
            for (std::size_t i = 0; i < n; ++i) {
                const double t = static_cast<double>(i) / static_cast<double>(kSampleRate);
                const double attack = 1.0 - std::exp(-t * 4800.0);
                const double click =
                    static_cast<double>(nextNoise(rng)) * std::exp(-t * (2400.0 - 700.0 * hard));
                const double m1 = std::sin(twoPi * 1915.0 * t) * std::exp(-t * 78.0);
                const double m2 = std::sin(twoPi * 3215.0 * t + 0.4) * std::exp(-t * 115.0);
                const double m3 = std::sin(twoPi * 4690.0 * t + 1.1) * std::exp(-t * 165.0);
                const double m4 = std::sin(twoPi * 6420.0 * t + 1.9) * std::exp(-t * 230.0);
                const double sample =
                    attack * ((0.30 + 0.35 * hard) * click + 0.44 * m1 + (0.22 + 0.24 * hard) * m2 +
                              (0.08 + 0.22 * hard) * m3 + (0.02 + 0.14 * hard) * m4);
                out[i] = static_cast<float>(std::clamp(sample * 0.72, -0.98, 0.98));
            }
        });
    }

    void prepareBuffers() {
        if (prepared_) return;
        prepared_ = true;

        cueStrike_ = makeBuffer(0.095, [](float* out, std::size_t n) {
            std::uint32_t rng = 0x9e3779b9u;
            constexpr double twoPi = 6.283185307179586;
            double phaseWood = 0.0;
            float lp = 0.0f;
            for (std::size_t i = 0; i < n; ++i) {
                const double t = static_cast<double>(i) / static_cast<double>(kSampleRate);
                const double attack = 1.0 - std::exp(-t * 2600.0);
                const double woodFreq = 115.0 + 110.0 * std::exp(-t * 140.0);
                phaseWood += twoPi * woodFreq / static_cast<double>(kSampleRate);
                const float rawNoise = nextNoise(rng);
                lp = lp + 0.32f * (rawNoise - lp);
                const double tip = static_cast<double>(lp) * std::exp(-t * 620.0);
                const double shaft = std::sin(phaseWood) * std::exp(-t * 62.0);
                const double ballPing = std::sin(twoPi * 1880.0 * t) * std::exp(-t * 130.0);
                const double sample = attack * (0.58 * tip + 0.42 * shaft + 0.22 * ballPing);
                out[i] = static_cast<float>(std::clamp(sample * 0.80, -0.98, 0.98));
            }
        });

        ballSoft_ = makeBallClack(0.15f);
        ballMed_ = makeBallClack(0.55f);
        ballHard_ = makeBallClack(1.00f);

        railBounce_ = makeBuffer(0.115, [](float* out, std::size_t n) {
            std::uint32_t rng = 0x243f6a88u;
            constexpr double twoPi = 6.283185307179586;
            double phase = 0.0;
            float lp = 0.0f;
            for (std::size_t i = 0; i < n; ++i) {
                const double t = static_cast<double>(i) / static_cast<double>(kSampleRate);
                const double freq = 66.0 + 95.0 * std::exp(-t * 85.0);
                phase += twoPi * freq / static_cast<double>(kSampleRate);
                const double rubber = std::sin(phase) * std::exp(-t * 38.0);
                const double wood = std::sin(twoPi * 282.0 * t) * std::exp(-t * 68.0);
                const double scuff = static_cast<double>(nextNoise(rng)) * std::exp(-t * 520.0);
                const float raw = static_cast<float>((1.0 - std::exp(-t * 1400.0)) *
                                                     (0.62 * rubber + 0.24 * wood + 0.18 * scuff));
                lp = lp + 0.24f * (raw - lp);
                out[i] = std::clamp(lp * 0.85f, -0.98f, 0.98f);
            }
        });

        pocketDrop_ = makeBuffer(0.255, [](float* out, std::size_t n) {
            std::uint32_t rng = 0xb7e15162u;
            constexpr double twoPi = 6.283185307179586;
            double phase = 0.0;
            float lp = 0.0f;
            for (std::size_t i = 0; i < n; ++i) {
                const double t = static_cast<double>(i) / static_cast<double>(kSampleRate);
                const double freq = 70.0 + 68.0 * std::exp(-t * 55.0);
                phase += twoPi * freq / static_cast<double>(kSampleRate);
                double sample = (0.52 * std::sin(phase) * std::exp(-t * 28.0) +
                                 0.26 * static_cast<double>(nextNoise(rng)) * std::exp(-t * 260.0)) *
                                (1.0 - std::exp(-t * 1200.0));
                if (t >= 0.064) {
                    const double t1 = t - 0.064;
                    sample += 0.26 * (std::sin(twoPi * 1680.0 * t1) + 0.5 * std::sin(twoPi * 2840.0 * t1)) *
                              std::exp(-t1 * 135.0);
                }
                if (t >= 0.128) {
                    const double t2 = t - 0.128;
                    sample += 0.18 * (std::sin(twoPi * 1920.0 * t2) + 0.4 * std::sin(twoPi * 3190.0 * t2)) *
                              std::exp(-t2 * 155.0);
                }
                const float raw = static_cast<float>(sample);
                lp = lp + 0.48f * (raw - lp);
                out[i] = std::clamp(lp * 0.82f, -0.98f, 0.98f);
            }
        });

        slingStretch_ = makeBuffer(0.075, [](float* out, std::size_t n) {
            constexpr double twoPi = 6.283185307179586;
            double phase = 0.0;
            for (std::size_t i = 0; i < n; ++i) {
                const double t = static_cast<double>(i) / static_cast<double>(kSampleRate);
                const double f = 145.0 + 75.0 * t / 0.075 + 18.0 * std::sin(twoPi * 68.0 * t);
                phase += twoPi * f / static_cast<double>(kSampleRate);
                const double env = std::sin(3.14159265 * t / 0.075) * std::exp(-t * 14.0);
                out[i] = static_cast<float>(
                    std::clamp(0.38 * env * std::tanh(1.8 * std::sin(phase)), -0.95, 0.95));
            }
        });

        slingRelease_ = makeBuffer(0.160, [](float* out, std::size_t n) {
            std::uint32_t rng = 0x71e5a912u;
            constexpr double twoPi = 6.283185307179586;
            double phase = 0.0;
            float lp = 0.0f;
            for (std::size_t i = 0; i < n; ++i) {
                const double t = static_cast<double>(i) / static_cast<double>(kSampleRate);
                const double f = 115.0 + 240.0 * std::exp(-t * 32.0);
                phase += twoPi * f / static_cast<double>(kSampleRate);
                const float nz = nextNoise(rng);
                lp = lp + 0.18f * (nz - lp);
                const double twang = (std::sin(phase) + 0.35 * std::sin(2.0 * phase)) * std::exp(-t * 24.0);
                const double whoosh =
                    static_cast<double>(lp) * std::sin(3.14159265 * std::min(1.0, t / 0.14)) * 0.55;
                out[i] = static_cast<float>(std::clamp((0.55 * twang + whoosh) * 0.82, -0.98, 0.98));
            }
        });

        woodImpact_ = makeBuffer(0.110, [](float* out, std::size_t n) {
            std::uint32_t rng = 0x38d01a44u;
            constexpr double twoPi = 6.283185307179586;
            for (std::size_t i = 0; i < n; ++i) {
                const double t = static_cast<double>(i) / static_cast<double>(kSampleRate);
                const double m1 = std::sin(twoPi * 295.0 * t) * std::exp(-t * 52.0);
                const double m2 = std::sin(twoPi * 540.0 * t + 0.5) * std::exp(-t * 78.0);
                const double crack = static_cast<double>(nextNoise(rng)) * std::exp(-t * 280.0);
                out[i] = static_cast<float>(
                    std::clamp((0.52 * m1 + 0.30 * m2 + 0.38 * crack) * 0.85, -0.98, 0.98));
            }
        });

        stoneImpact_ = makeBuffer(0.135, [](float* out, std::size_t n) {
            std::uint32_t rng = 0x99a2bc11u;
            constexpr double twoPi = 6.283185307179586;
            double phase = 0.0;
            float lp = 0.0f;
            for (std::size_t i = 0; i < n; ++i) {
                const double t = static_cast<double>(i) / static_cast<double>(kSampleRate);
                phase += twoPi * (68.0 + 75.0 * std::exp(-t * 65.0)) / static_cast<double>(kSampleRate);
                const float nz = nextNoise(rng);
                lp = lp + 0.14f * (nz - lp);
                const double s = 0.68 * std::sin(phase) * std::exp(-t * 28.0) +
                                 0.35 * static_cast<double>(lp) * std::exp(-t * 95.0);
                out[i] = static_cast<float>(std::clamp(s * 0.88, -0.98, 0.98));
            }
        });

        glassShatter_ = makeBuffer(0.180, [](float* out, std::size_t n) {
            std::uint32_t rng = 0xcafef00du;
            constexpr double twoPi = 6.283185307179586;
            for (std::size_t i = 0; i < n; ++i) {
                const double t = static_cast<double>(i) / static_cast<double>(kSampleRate);
                const double c1 = std::sin(twoPi * 2680.0 * t) * std::exp(-t * 36.0);
                const double c2 = std::sin(twoPi * 4120.0 * t + 0.8) * std::exp(-t * 48.0);
                const double c3 = std::sin(twoPi * 5890.0 * t + 1.7) * std::exp(-t * 65.0);
                const double burst = static_cast<double>(nextNoise(rng)) * std::exp(-t * 190.0);
                out[i] = static_cast<float>(
                    std::clamp((0.32 * c1 + 0.28 * c2 + 0.22 * c3 + 0.30 * burst) * 0.76, -0.98, 0.98));
            }
        });

        targetPop_ = makeBuffer(0.145, [](float* out, std::size_t n) {
            constexpr double twoPi = 6.283185307179586;
            double phase = 0.0;
            for (std::size_t i = 0; i < n; ++i) {
                const double t = static_cast<double>(i) / static_cast<double>(kSampleRate);
                const double f = 360.0 + 520.0 * std::min(1.0, t / 0.09);
                phase += twoPi * f / static_cast<double>(kSampleRate);
                const double env = (1.0 - std::exp(-t * 1800.0)) * std::exp(-t * 22.0);
                out[i] = static_cast<float>(std::clamp(0.68 * env * std::sin(phase), -0.98, 0.98));
            }
        });

        malletStrike_ = makeBuffer(0.072, [](float* out, std::size_t n) {
            std::uint32_t rng = 0x4411cc22u;
            constexpr double twoPi = 6.283185307179586;
            for (std::size_t i = 0; i < n; ++i) {
                const double t = static_cast<double>(i) / static_cast<double>(kSampleRate);
                const double p1 = std::sin(twoPi * 1420.0 * t) * std::exp(-t * 85.0);
                const double p2 = std::sin(twoPi * 2760.0 * t + 0.3) * std::exp(-t * 125.0);
                const double snap = static_cast<double>(nextNoise(rng)) * std::exp(-t * 1600.0);
                out[i] =
                    static_cast<float>(std::clamp((0.46 * p1 + 0.34 * p2 + 0.42 * snap) * 0.84, -0.98, 0.98));
            }
        });

        hockeyGoal_ = makeBuffer(0.360, [](float* out, std::size_t n) {
            constexpr double twoPi = 6.283185307179586;
            for (std::size_t i = 0; i < n; ++i) {
                const double t = static_cast<double>(i) / static_cast<double>(kSampleRate);
                double s = 0.34 * std::sin(twoPi * 523.25 * t) * std::exp(-t * 5.5);
                if (t >= 0.055)
                    s += 0.34 * std::sin(twoPi * 659.25 * (t - 0.055)) * std::exp(-(t - 0.055) * 5.5);
                if (t >= 0.110)
                    s += 0.40 * std::sin(twoPi * 783.99 * (t - 0.110)) * std::exp(-(t - 0.110) * 4.5);
                out[i] = static_cast<float>(std::clamp(s * 0.85, -0.98, 0.98));
            }
        });

        iconTap_ = makeBuffer(0.068, [](float* out, std::size_t n) {
            std::uint32_t rng = 0x5f3759dfu;
            constexpr double twoPi = 6.283185307179586;
            float lp = 0.0f;
            for (std::size_t i = 0; i < n; ++i) {
                const double t = static_cast<double>(i) / static_cast<double>(kSampleRate);
                const double attack = 1.0 - std::exp(-t * 3200.0);
                lp = lp + 0.38f * (nextNoise(rng) - lp);
                const double click = static_cast<double>(lp) * std::exp(-t * 980.0);
                const double m1 = std::sin(twoPi * 690.0 * t) * std::exp(-t * 72.0);
                const double m2 = std::sin(twoPi * 1310.0 * t + 0.35) * std::exp(-t * 108.0);
                const double m3 = std::sin(twoPi * 2180.0 * t + 1.05) * std::exp(-t * 165.0);
                const double s = attack * (0.44 * m1 + 0.30 * m2 + 0.16 * m3 + 0.32 * click);
                out[i] = static_cast<float>(std::clamp(s * 0.78, -0.98, 0.98));
            }
        });

        iconThud_ = makeBuffer(0.105, [](float* out, std::size_t n) {
            std::uint32_t rng = 0x2a6d3f91u;
            constexpr double twoPi = 6.283185307179586;
            double phase = 0.0;
            float lp = 0.0f;
            for (std::size_t i = 0; i < n; ++i) {
                const double t = static_cast<double>(i) / static_cast<double>(kSampleRate);
                phase += twoPi * (64.0 + 58.0 * std::exp(-t * 82.0)) / static_cast<double>(kSampleRate);
                lp = lp + 0.22f * (nextNoise(rng) - lp);
                const double body = std::sin(phase) * std::exp(-t * 38.0);
                const double tile = std::sin(twoPi * 365.0 * t + 0.2) * std::exp(-t * 88.0);
                const double mat = static_cast<double>(lp) * std::exp(-t * 360.0);
                const double s = (1.0 - std::exp(-t * 1800.0)) * (0.58 * body + 0.24 * tile + 0.24 * mat);
                out[i] = static_cast<float>(std::clamp(s * 0.82, -0.98, 0.98));
            }
        });

        iconGrab_ = makeBuffer(0.084, [](float* out, std::size_t n) {
            std::uint32_t rng = 0x811c9dc5u;
            constexpr double twoPi = 6.283185307179586;
            double phase = 0.0;
            float lp = 0.0f;
            for (std::size_t i = 0; i < n; ++i) {
                const double t = static_cast<double>(i) / static_cast<double>(kSampleRate);
                const double f = 285.0 + 185.0 * std::min(1.0, t / 0.024);
                phase += twoPi * f / static_cast<double>(kSampleRate);
                lp = lp + 0.16f * (nextNoise(rng) - lp);
                const double pop = std::sin(phase) * std::exp(-t * 68.0);
                const double air =
                    static_cast<double>(lp) * std::sin(3.14159265 * std::min(1.0, t / 0.082)) * 0.36;
                out[i] = static_cast<float>(std::clamp((0.56 * pop + air) * 0.72, -0.96, 0.96));
            }
        });

        blackHoleHum_ = makeBuffer(0.500, [](float* out, std::size_t n) {
            std::uint32_t rng = 0x1b873593u;
            constexpr double twoPi = 6.283185307179586;
            float lp = 0.0f;
            const double dur =
                static_cast<double>(std::max<std::size_t>(1u, n - 1u)) / static_cast<double>(kSampleRate);
            for (std::size_t i = 0; i < n; ++i) {
                const double t = static_cast<double>(i) / static_cast<double>(kSampleRate);
                const double hann = 0.5 * (1.0 - std::cos(twoPi * t / dur));
                lp = lp + 0.035f * (nextNoise(rng) - lp);
                const double sub =
                    0.50 * std::sin(twoPi * 55.0 * t) + 0.24 * std::sin(twoPi * 56.0 * t + 0.6);
                const double oct = 0.22 * std::sin(twoPi * 110.0 * t);
                const double fifth =
                    0.13 * std::sin(twoPi * 164.81 * t) * (0.72 + 0.28 * std::sin(twoPi * 3.0 * t));
                const double shimmer =
                    0.06 * std::sin(twoPi * 329.63 * t + 0.4 * std::sin(twoPi * 5.0 * t));
                const double s = hann * (sub + oct + fifth + shimmer + 0.18 * static_cast<double>(lp));
                out[i] = static_cast<float>(std::clamp(s * 0.76, -0.95, 0.95));
            }
        });

        blackHoleSwallow_ = makeBuffer(0.320, [](float* out, std::size_t n) {
            std::uint32_t rng = 0xcc9e2d51u;
            constexpr double twoPi = 6.283185307179586;
            double phDown = 0.0, phUp = 0.0;
            float lp = 0.0f;
            for (std::size_t i = 0; i < n; ++i) {
                const double t = static_cast<double>(i) / static_cast<double>(kSampleRate);
                const double u = std::min(1.0, t / 0.30);
                phDown += twoPi * (270.0 * std::exp(-t * 5.4) + 52.0) / static_cast<double>(kSampleRate);
                phUp += twoPi * (380.0 + 820.0 * u * u) / static_cast<double>(kSampleRate);
                lp = lp + 0.26f * (nextNoise(rng) - lp);
                const double env = (1.0 - std::exp(-t * 950.0)) * std::exp(-t * 9.2);
                const double plunge = 0.56 * std::sin(phDown);
                const double ring = 0.28 * std::sin(phUp) * std::exp(-t * 6.5);
                const double dust = static_cast<double>(lp) * 0.24 * std::exp(-t * 14.0);
                out[i] = static_cast<float>(std::clamp(env * (plunge + ring + dust) * 0.82, -0.98, 0.98));
            }
        });
    }

    bool ensureVoiceOpen(Voice& v) {
        if (engineFailed_) return false;
#ifdef DPP_DIRECT_INPUT_TESTS
        (void)v;
        return false;
#else
        if (v.handle) return true;
        if (waveOutGetNumDevs() == 0u) {
            engineFailed_ = true;
            return false;
        }
        WAVEFORMATEX fmt{};
        fmt.wFormatTag = WAVE_FORMAT_PCM;
        fmt.nChannels = 2;
        fmt.nSamplesPerSec = kSampleRate;
        fmt.wBitsPerSample = 16;
        fmt.nBlockAlign = static_cast<WORD>(fmt.nChannels * (fmt.wBitsPerSample / 8u));
        fmt.nAvgBytesPerSec = fmt.nSamplesPerSec * fmt.nBlockAlign;
        if (waveOutOpen(&v.handle, WAVE_MAPPER, &fmt, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR || !v.handle) {
            v.handle = nullptr;
            engineFailed_ = true;
            return false;
        }
        v.stereoPcm.resize(kMaxRenderFrames * 2u, 0);
        return true;
#endif
    }

    static void retireFinishedHeader(Voice& v) noexcept {
        if (!v.handle || !v.active) return;
        if ((v.header.dwFlags & WHDR_DONE) != 0) {
            if ((v.header.dwFlags & WHDR_PREPARED) != 0) {
                waveOutUnprepareHeader(v.handle, &v.header, sizeof(WAVEHDR));
            }
            v.header = {};
            v.active = false;
        }
    }

    static void forceResetVoice(Voice& v) noexcept {
        if (!v.handle) return;
        if (v.active) {
            waveOutReset(v.handle);
            if ((v.header.dwFlags & WHDR_PREPARED) != 0) {
                waveOutUnprepareHeader(v.handle, &v.header, sizeof(WAVEHDR));
            }
            v.header = {};
            v.active = false;
        }
    }

   public:
    BilliardsAudio() = default;
    ~BilliardsAudio() { stop(); }
    BilliardsAudio(const BilliardsAudio&) = delete;
    BilliardsAudio& operator=(const BilliardsAudio&) = delete;

    [[nodiscard]] std::uint64_t playedCount() const { return played_; }

    void play(std::span<const SoundEvent> events, bool muted) noexcept {
        if (muted || events.empty()) return;
        try {
            prepareBuffers();
            for (const SoundEvent& ev : events) {
                ++played_;
                const std::vector<float>* src = nullptr;
                switch (ev.kind) {
                    case SoundKind::CueStrike:
                        src = &cueStrike_;
                        break;
                    case SoundKind::BallCollide:
                        src = ev.gain < 0.34f ? &ballSoft_ : (ev.gain < 0.72f ? &ballMed_ : &ballHard_);
                        break;
                    case SoundKind::RailBounce:
                        src = &railBounce_;
                        break;
                    case SoundKind::PocketDrop:
                        src = &pocketDrop_;
                        break;
                    case SoundKind::SlingStretch:
                        src = &slingStretch_;
                        break;
                    case SoundKind::SlingRelease:
                        src = &slingRelease_;
                        break;
                    case SoundKind::WoodImpact:
                        src = &woodImpact_;
                        break;
                    case SoundKind::StoneImpact:
                        src = &stoneImpact_;
                        break;
                    case SoundKind::GlassShatter:
                        src = &glassShatter_;
                        break;
                    case SoundKind::TargetPop:
                        src = &targetPop_;
                        break;
                    case SoundKind::MalletStrike:
                        src = &malletStrike_;
                        break;
                    case SoundKind::HockeyGoal:
                        src = &hockeyGoal_;
                        break;
                    case SoundKind::IconTap:
                        src = &iconTap_;
                        break;
                    case SoundKind::IconThud:
                        src = &iconThud_;
                        break;
                    case SoundKind::IconGrab:
                        src = &iconGrab_;
                        break;
                    case SoundKind::BlackHoleHum:
                        src = &blackHoleHum_;
                        break;
                    case SoundKind::BlackHoleSwallow:
                        src = &blackHoleSwallow_;
                        break;
                }
                if (!src || src->empty()) continue;

                Voice* chosen = nullptr;
                for (std::size_t k = 0; k < kVoiceCount; ++k) {
                    const std::size_t idx = (nextVoice_ + k) % kVoiceCount;
                    retireFinishedHeader(voices_[idx]);
                    if (!voices_[idx].active) {
                        chosen = &voices_[idx];
                        nextVoice_ = (idx + 1u) % kVoiceCount;
                        break;
                    }
                }
                if (!chosen) {
                    chosen = &voices_[nextVoice_ % kVoiceCount];
                    nextVoice_ = (nextVoice_ + 1u) % kVoiceCount;
                    forceResetVoice(*chosen);
                }
                if (!ensureVoiceOpen(*chosen)) continue;

                const float gain = std::clamp(ev.gain, 0.04f, 1.0f);
                const float pan = std::clamp(ev.pan, -1.0f, 1.0f);
                const float pitch = std::clamp(ev.pitch, 0.88f, 1.14f);
                const float angle = (pan + 1.0f) * 0.78539816f;  // [0, pi/2] constant-power pan
                const float leftGain = gain * std::cos(angle);
                const float rightGain = gain * std::sin(angle);

                const std::size_t srcLen = src->size();
                const auto outFrames = std::min<std::size_t>(
                    kMaxRenderFrames,
                    static_cast<std::size_t>(std::max(16.0f, static_cast<float>(srcLen) / pitch)));
                if (chosen->stereoPcm.size() < outFrames * 2u) {
                    chosen->stereoPcm.resize(outFrames * 2u, 0);
                }
                const float* mono = src->data();
                for (std::size_t f = 0; f < outFrames; ++f) {
                    const float srcPos = static_cast<float>(f) * pitch;
                    const auto idx0 = static_cast<std::size_t>(srcPos);
                    if (idx0 >= srcLen) {
                        chosen->stereoPcm[f * 2u] = 0;
                        chosen->stereoPcm[f * 2u + 1u] = 0;
                        continue;
                    }
                    const std::size_t idx1 = std::min(idx0 + 1u, srcLen - 1u);
                    const float frac = srcPos - static_cast<float>(idx0);
                    const float s = mono[idx0] + (mono[idx1] - mono[idx0]) * frac;
                    const int lInt =
                        static_cast<int>(std::lround(std::clamp(s * leftGain, -1.0f, 1.0f) * 32760.0f));
                    const int rInt =
                        static_cast<int>(std::lround(std::clamp(s * rightGain, -1.0f, 1.0f) * 32760.0f));
                    chosen->stereoPcm[f * 2u] = static_cast<std::int16_t>(lInt);
                    chosen->stereoPcm[f * 2u + 1u] = static_cast<std::int16_t>(rInt);
                }

                chosen->header = {};
                chosen->header.lpData = reinterpret_cast<LPSTR>(chosen->stereoPcm.data());
                chosen->header.dwBufferLength = static_cast<DWORD>(outFrames * 2u * sizeof(std::int16_t));
                if (waveOutPrepareHeader(chosen->handle, &chosen->header, sizeof(WAVEHDR)) ==
                    MMSYSERR_NOERROR) {
                    if (waveOutWrite(chosen->handle, &chosen->header, sizeof(WAVEHDR)) == MMSYSERR_NOERROR) {
                        chosen->active = true;
                    } else {
                        waveOutUnprepareHeader(chosen->handle, &chosen->header, sizeof(WAVEHDR));
                        chosen->header = {};
                    }
                }
            }
        } catch (...) {
        }
    }

    void stop() noexcept {
        for (Voice& v : voices_) {
            if (!v.handle) continue;
            forceResetVoice(v);
            waveOutClose(v.handle);
            v.handle = nullptr;
        }
    }
};

}  // namespace dpp::winmirror
