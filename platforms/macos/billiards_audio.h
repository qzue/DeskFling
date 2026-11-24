#pragma once
// Procedural copyright-free billiards acoustic synthesizer & polyphonic player.
// Pre-builds tiny float32 PCM buffers once in memory (no external audio files);
// degrades silently to no-op if no output device is available.

#import <AVFoundation/AVFoundation.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <span>

#include "core/modes/game_mode.h"

namespace dpp::mac {

class BilliardsAudio {
    static constexpr double kSampleRate = 44100.0;
    static constexpr std::size_t kVoiceCount = 8;
    static constexpr AVAudioFrameCount kMaxRenderFrames = 32768;

    AVAudioEngine* engine_ = nil;
    AVAudioFormat* format_ = nil;
    NSMutableArray<AVAudioPlayerNode*>* voices_ = nil;
    NSMutableArray<AVAudioPCMBuffer*>* voiceBufs_ = nil;
    AVAudioPCMBuffer* cueStrike_ = nil;
    AVAudioPCMBuffer* ballSoft_ = nil;
    AVAudioPCMBuffer* ballMed_ = nil;
    AVAudioPCMBuffer* ballHard_ = nil;
    AVAudioPCMBuffer* railBounce_ = nil;
    AVAudioPCMBuffer* pocketDrop_ = nil;
    AVAudioPCMBuffer* slingStretch_ = nil;
    AVAudioPCMBuffer* slingRelease_ = nil;
    AVAudioPCMBuffer* woodImpact_ = nil;
    AVAudioPCMBuffer* stoneImpact_ = nil;
    AVAudioPCMBuffer* glassShatter_ = nil;
    AVAudioPCMBuffer* targetPop_ = nil;
    AVAudioPCMBuffer* malletStrike_ = nil;
    AVAudioPCMBuffer* hockeyGoal_ = nil;
    AVAudioPCMBuffer* iconTap_ = nil;
    AVAudioPCMBuffer* iconThud_ = nil;
    AVAudioPCMBuffer* iconGrab_ = nil;
    AVAudioPCMBuffer* blackHoleHum_ = nil;
    AVAudioPCMBuffer* blackHoleSwallow_ = nil;
    std::size_t nextVoice_ = 0;
    std::uint64_t played_ = 0;
    bool prepared_ = false;
    bool engineFailed_ = false;

    static float nextNoise(std::uint32_t& state) {
        state = state * 1664525u + 1013904223u;
        return static_cast<float>(static_cast<std::int32_t>(state)) / 2147483648.0f;
    }

    AVAudioPCMBuffer* makeBuffer(double durationSec, void (^fill)(float* samples, AVAudioFrameCount count)) {
        const auto frames =
            static_cast<AVAudioFrameCount>(std::max(64.0, std::round(durationSec * kSampleRate)));
        AVAudioPCMBuffer* buf = [[AVAudioPCMBuffer alloc] initWithPCMFormat:format_ frameCapacity:frames];
        if (!buf || !buf.floatChannelData) return nil;
        buf.frameLength = frames;
        float* data = buf.floatChannelData[0];
        fill(data, frames);
        // Short fade-out tail to guarantee zero click on buffer end.
        const AVAudioFrameCount fade = std::min<AVAudioFrameCount>(frames / 6, 96);
        for (AVAudioFrameCount i = 0; i < fade; ++i) {
            const float w = static_cast<float>(i) / static_cast<float>(fade);
            data[frames - 1 - i] *= w;
        }
        return buf;
    }

    AVAudioPCMBuffer* makeBallClack(float hardness) {
        return makeBuffer(0.080, ^(float* out, AVAudioFrameCount n) {
          std::uint32_t rng = 0x51a7c0deu + static_cast<std::uint32_t>(hardness * 1000.0f);
          constexpr double twoPi = 6.283185307179586;
          for (AVAudioFrameCount i = 0; i < n; ++i) {
              const double t = static_cast<double>(i) / kSampleRate;
              const double attack = 1.0 - std::exp(-t * 4800.0);
              // Hertzian contact impulse (< 0.9 ms).
              const double click = nextNoise(rng) * std::exp(-t * (2400.0 - 700.0 * hardness));
              // Cast-phenolic sphere modal resonances (~1.91, 3.21, 4.69, 6.42 kHz).
              const double m1 = std::sin(twoPi * 1915.0 * t) * std::exp(-t * 78.0);
              const double m2 = std::sin(twoPi * 3215.0 * t + 0.4) * std::exp(-t * 115.0);
              const double m3 = std::sin(twoPi * 4690.0 * t + 1.1) * std::exp(-t * 165.0);
              const double m4 = std::sin(twoPi * 6420.0 * t + 1.9) * std::exp(-t * 230.0);
              const double sample =
                  attack * ((0.30 + 0.35 * hardness) * click + 0.44 * m1 + (0.22 + 0.24 * hardness) * m2 +
                            (0.08 + 0.22 * hardness) * m3 + (0.02 + 0.14 * hardness) * m4);
              out[i] = static_cast<float>(std::clamp(sample * 0.72, -0.98, 0.98));
          }
        });
    }

    void prepareBuffers() {
        if (prepared_) return;
        prepared_ = true;
        format_ = [[AVAudioFormat alloc] initStandardFormatWithSampleRate:kSampleRate channels:1];
        if (!format_) return;

        // 1. Leather cue tip + maple shaft + phenolic cue-ball excitation.
        cueStrike_ = makeBuffer(0.095, ^(float* out, AVAudioFrameCount n) {
          std::uint32_t rng = 0x9e3779b9u;
          constexpr double twoPi = 6.283185307179586;
          double phaseWood = 0.0;
          float lp = 0.0f;
          for (AVAudioFrameCount i = 0; i < n; ++i) {
              const double t = static_cast<double>(i) / kSampleRate;
              const double attack = 1.0 - std::exp(-t * 2600.0);
              const double woodFreq = 115.0 + 110.0 * std::exp(-t * 140.0);
              phaseWood += twoPi * woodFreq / kSampleRate;
              const float rawNoise = nextNoise(rng);
              lp = lp + 0.32f * (rawNoise - lp);  // chalked leather tip thwack
              const double tip = static_cast<double>(lp) * std::exp(-t * 620.0);
              const double shaft = std::sin(phaseWood) * std::exp(-t * 62.0);
              const double ballPing = std::sin(twoPi * 1880.0 * t) * std::exp(-t * 130.0);
              const double sample = attack * (0.58 * tip + 0.42 * shaft + 0.22 * ballPing);
              out[i] = static_cast<float>(std::clamp(sample * 0.80, -0.98, 0.98));
          }
        });

        // 2. Three velocity layers for phenolic ball-to-ball clacks.
        ballSoft_ = makeBallClack(0.15f);
        ballMed_ = makeBallClack(0.55f);
        ballHard_ = makeBallClack(1.00f);

        // 3. Vulcanized K-66 rubber cushion thump + wooden rail body.
        railBounce_ = makeBuffer(0.115, ^(float* out, AVAudioFrameCount n) {
          std::uint32_t rng = 0x243f6a88u;
          constexpr double twoPi = 6.283185307179586;
          double phase = 0.0;
          float lp = 0.0f;
          for (AVAudioFrameCount i = 0; i < n; ++i) {
              const double t = static_cast<double>(i) / kSampleRate;
              const double freq = 66.0 + 95.0 * std::exp(-t * 85.0);
              phase += twoPi * freq / kSampleRate;
              const double rubber = std::sin(phase) * std::exp(-t * 38.0);
              const double wood = std::sin(twoPi * 282.0 * t) * std::exp(-t * 68.0);
              const double scuff = static_cast<double>(nextNoise(rng)) * std::exp(-t * 520.0);
              const float raw = static_cast<float>((1.0 - std::exp(-t * 1400.0)) *
                                                   (0.62 * rubber + 0.24 * wood + 0.18 * scuff));
              lp = lp + 0.24f * (raw - lp);
              out[i] = std::clamp(lp * 0.85f, -0.98f, 0.98f);
          }
        });

        // 4. Leather pocket drop + two cylinder return-tube clicks.
        pocketDrop_ = makeBuffer(0.255, ^(float* out, AVAudioFrameCount n) {
          std::uint32_t rng = 0xb7e15162u;
          constexpr double twoPi = 6.283185307179586;
          double phase = 0.0;
          float lp = 0.0f;
          for (AVAudioFrameCount i = 0; i < n; ++i) {
              const double t = static_cast<double>(i) / kSampleRate;
              const double freq = 70.0 + 68.0 * std::exp(-t * 55.0);
              phase += twoPi * freq / kSampleRate;
              double sample = (0.52 * std::sin(phase) * std::exp(-t * 28.0) +
                               0.26 * static_cast<double>(nextNoise(rng)) * std::exp(-t * 260.0)) *
                              (1.0 - std::exp(-t * 1200.0));
              // Secondary rattle into the side cylinder tube at ~64 ms and ~128 ms.
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

        // 5. Elastic slingshot stretch creak.
        slingStretch_ = makeBuffer(0.075, ^(float* out, AVAudioFrameCount n) {
          constexpr double twoPi = 6.283185307179586;
          double phase = 0.0;
          for (AVAudioFrameCount i = 0; i < n; ++i) {
              const double t = static_cast<double>(i) / kSampleRate;
              const double f = 145.0 + 75.0 * t / 0.075 + 18.0 * std::sin(twoPi * 68.0 * t);
              phase += twoPi * f / kSampleRate;
              const double env = std::sin(3.14159265 * t / 0.075) * std::exp(-t * 14.0);
              out[i] =
                  static_cast<float>(std::clamp(0.38 * env * std::tanh(1.8 * std::sin(phase)), -0.95, 0.95));
          }
        });

        // 6. Slingshot rubber release twang + air whoosh.
        slingRelease_ = makeBuffer(0.160, ^(float* out, AVAudioFrameCount n) {
          std::uint32_t rng = 0x71e5a912u;
          constexpr double twoPi = 6.283185307179586;
          double phase = 0.0;
          float lp = 0.0f;
          for (AVAudioFrameCount i = 0; i < n; ++i) {
              const double t = static_cast<double>(i) / kSampleRate;
              const double f = 115.0 + 240.0 * std::exp(-t * 32.0);
              phase += twoPi * f / kSampleRate;
              const float nz = nextNoise(rng);
              lp = lp + 0.18f * (nz - lp);
              const double twang = (std::sin(phase) + 0.35 * std::sin(2.0 * phase)) * std::exp(-t * 24.0);
              const double whoosh =
                  static_cast<double>(lp) * std::sin(3.14159265 * std::min(1.0, t / 0.14)) * 0.55;
              out[i] = static_cast<float>(std::clamp((0.55 * twang + whoosh) * 0.82, -0.98, 0.98));
          }
        });

        // 7. Hollow timber knock / fracture.
        woodImpact_ = makeBuffer(0.110, ^(float* out, AVAudioFrameCount n) {
          std::uint32_t rng = 0x38d01a44u;
          constexpr double twoPi = 6.283185307179586;
          for (AVAudioFrameCount i = 0; i < n; ++i) {
              const double t = static_cast<double>(i) / kSampleRate;
              const double m1 = std::sin(twoPi * 295.0 * t) * std::exp(-t * 52.0);
              const double m2 = std::sin(twoPi * 540.0 * t + 0.5) * std::exp(-t * 78.0);
              const double crack = static_cast<double>(nextNoise(rng)) * std::exp(-t * 280.0);
              out[i] =
                  static_cast<float>(std::clamp((0.52 * m1 + 0.30 * m2 + 0.38 * crack) * 0.85, -0.98, 0.98));
          }
        });

        // 8. Heavy granite thud.
        stoneImpact_ = makeBuffer(0.135, ^(float* out, AVAudioFrameCount n) {
          std::uint32_t rng = 0x99a2bc11u;
          constexpr double twoPi = 6.283185307179586;
          double phase = 0.0;
          float lp = 0.0f;
          for (AVAudioFrameCount i = 0; i < n; ++i) {
              const double t = static_cast<double>(i) / kSampleRate;
              phase += twoPi * (68.0 + 75.0 * std::exp(-t * 65.0)) / kSampleRate;
              const float nz = nextNoise(rng);
              lp = lp + 0.14f * (nz - lp);
              const double s = 0.68 * std::sin(phase) * std::exp(-t * 28.0) +
                               0.35 * static_cast<double>(lp) * std::exp(-t * 95.0);
              out[i] = static_cast<float>(std::clamp(s * 0.88, -0.98, 0.98));
          }
        });

        // 9. Crystalline glass / ice shatter chime.
        glassShatter_ = makeBuffer(0.180, ^(float* out, AVAudioFrameCount n) {
          std::uint32_t rng = 0xcafef00du;
          constexpr double twoPi = 6.283185307179586;
          for (AVAudioFrameCount i = 0; i < n; ++i) {
              const double t = static_cast<double>(i) / kSampleRate;
              const double c1 = std::sin(twoPi * 2680.0 * t) * std::exp(-t * 36.0);
              const double c2 = std::sin(twoPi * 4120.0 * t + 0.8) * std::exp(-t * 48.0);
              const double c3 = std::sin(twoPi * 5890.0 * t + 1.7) * std::exp(-t * 65.0);
              const double burst = static_cast<double>(nextNoise(rng)) * std::exp(-t * 190.0);
              out[i] = static_cast<float>(
                  std::clamp((0.32 * c1 + 0.28 * c2 + 0.22 * c3 + 0.30 * burst) * 0.76, -0.98, 0.98));
          }
        });

        // 10. Cheerful upward pop when knocking out a defender icon.
        targetPop_ = makeBuffer(0.145, ^(float* out, AVAudioFrameCount n) {
          constexpr double twoPi = 6.283185307179586;
          double phase = 0.0;
          for (AVAudioFrameCount i = 0; i < n; ++i) {
              const double t = static_cast<double>(i) / kSampleRate;
              const double f = 360.0 + 520.0 * std::min(1.0, t / 0.09);
              phase += twoPi * f / kSampleRate;
              const double env = (1.0 - std::exp(-t * 1800.0)) * std::exp(-t * 22.0);
              out[i] = static_cast<float>(std::clamp(0.68 * env * std::sin(phase), -0.98, 0.98));
          }
        });

        // 11. Acrylic air-hockey mallet-puck clack.
        malletStrike_ = makeBuffer(0.072, ^(float* out, AVAudioFrameCount n) {
          std::uint32_t rng = 0x4411cc22u;
          constexpr double twoPi = 6.283185307179586;
          for (AVAudioFrameCount i = 0; i < n; ++i) {
              const double t = static_cast<double>(i) / kSampleRate;
              const double p1 = std::sin(twoPi * 1420.0 * t) * std::exp(-t * 85.0);
              const double p2 = std::sin(twoPi * 2760.0 * t + 0.3) * std::exp(-t * 125.0);
              const double snap = static_cast<double>(nextNoise(rng)) * std::exp(-t * 1600.0);
              out[i] =
                  static_cast<float>(std::clamp((0.46 * p1 + 0.34 * p2 + 0.42 * snap) * 0.84, -0.98, 0.98));
          }
        });

        // 12. Bright major-triad victory / goal fanfare chime.
        hockeyGoal_ = makeBuffer(0.360, ^(float* out, AVAudioFrameCount n) {
          constexpr double twoPi = 6.283185307179586;
          for (AVAudioFrameCount i = 0; i < n; ++i) {
              const double t = static_cast<double>(i) / kSampleRate;
              double s = 0.34 * std::sin(twoPi * 523.25 * t) * std::exp(-t * 5.5);
              if (t >= 0.055)
                  s += 0.34 * std::sin(twoPi * 659.25 * (t - 0.055)) * std::exp(-(t - 0.055) * 5.5);
              if (t >= 0.110)
                  s += 0.40 * std::sin(twoPi * 783.99 * (t - 0.110)) * std::exp(-(t - 0.110) * 4.5);
              out[i] = static_cast<float>(std::clamp(s * 0.85, -0.98, 0.98));
          }
        });

        // 13. Gravity icon-to-icon frosted polycarbonate / keycap tile tap.
        iconTap_ = makeBuffer(0.068, ^(float* out, AVAudioFrameCount n) {
          std::uint32_t rng = 0x5f3759dfu;
          constexpr double twoPi = 6.283185307179586;
          float lp = 0.0f;
          for (AVAudioFrameCount i = 0; i < n; ++i) {
              const double t = static_cast<double>(i) / kSampleRate;
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

        // 14. Gravity desktop floor / wall cushioned thud + tile settle.
        iconThud_ = makeBuffer(0.105, ^(float* out, AVAudioFrameCount n) {
          std::uint32_t rng = 0x2a6d3f91u;
          constexpr double twoPi = 6.283185307179586;
          double phase = 0.0;
          float lp = 0.0f;
          for (AVAudioFrameCount i = 0; i < n; ++i) {
              const double t = static_cast<double>(i) / kSampleRate;
              phase += twoPi * (64.0 + 58.0 * std::exp(-t * 82.0)) / kSampleRate;
              lp = lp + 0.22f * (nextNoise(rng) - lp);
              const double body = std::sin(phase) * std::exp(-t * 38.0);
              const double tile = std::sin(twoPi * 365.0 * t + 0.2) * std::exp(-t * 88.0);
              const double mat = static_cast<double>(lp) * std::exp(-t * 360.0);
              const double s = (1.0 - std::exp(-t * 1800.0)) * (0.58 * body + 0.24 * tile + 0.24 * mat);
              out[i] = static_cast<float>(std::clamp(s * 0.82, -0.98, 0.98));
          }
        });

        // 15. Tactile icon pickup pluck + throw air-whoosh.
        iconGrab_ = makeBuffer(0.084, ^(float* out, AVAudioFrameCount n) {
          std::uint32_t rng = 0x811c9dc5u;
          constexpr double twoPi = 6.283185307179586;
          double phase = 0.0;
          float lp = 0.0f;
          for (AVAudioFrameCount i = 0; i < n; ++i) {
              const double t = static_cast<double>(i) / kSampleRate;
              const double f = 285.0 + 185.0 * std::min(1.0, t / 0.024);
              phase += twoPi * f / kSampleRate;
              lp = lp + 0.16f * (nextNoise(rng) - lp);
              const double pop = std::sin(phase) * std::exp(-t * 68.0);
              const double air =
                  static_cast<double>(lp) * std::sin(3.14159265 * std::min(1.0, t / 0.082)) * 0.36;
              out[i] = static_cast<float>(std::clamp((0.56 * pop + air) * 0.72, -0.96, 0.96));
          }
        });

        // 16. Black hole 0.50s Hann-windowed relativistic gravitational sub-bass + shimmer grain.
        blackHoleHum_ = makeBuffer(0.500, ^(float* out, AVAudioFrameCount n) {
          std::uint32_t rng = 0x1b873593u;
          constexpr double twoPi = 6.283185307179586;
          float lp = 0.0f;
          const double dur = static_cast<double>(std::max<AVAudioFrameCount>(1, n - 1)) / kSampleRate;
          for (AVAudioFrameCount i = 0; i < n; ++i) {
              const double t = static_cast<double>(i) / kSampleRate;
              const double hann = 0.5 * (1.0 - std::cos(twoPi * t / dur));
              lp = lp + 0.035f * (nextNoise(rng) - lp);
              const double sub =
                  0.50 * std::sin(twoPi * 55.0 * t) + 0.24 * std::sin(twoPi * 56.0 * t + 0.6);
              const double oct = 0.22 * std::sin(twoPi * 110.0 * t);
              const double fifth =
                  0.13 * std::sin(twoPi * 164.81 * t) * (0.72 + 0.28 * std::sin(twoPi * 3.0 * t));
              const double shimmer = 0.06 * std::sin(twoPi * 329.63 * t + 0.4 * std::sin(twoPi * 5.0 * t));
              const double s = hann * (sub + oct + fifth + shimmer + 0.18 * static_cast<double>(lp));
              out[i] = static_cast<float>(std::clamp(s * 0.76, -0.95, 0.95));
          }
        });

        // 17. Black hole event-horizon spaghettification chirp + photon-ring sparkle.
        blackHoleSwallow_ = makeBuffer(0.320, ^(float* out, AVAudioFrameCount n) {
          std::uint32_t rng = 0xcc9e2d51u;
          constexpr double twoPi = 6.283185307179586;
          double phDown = 0.0, phUp = 0.0;
          float lp = 0.0f;
          for (AVAudioFrameCount i = 0; i < n; ++i) {
              const double t = static_cast<double>(i) / kSampleRate;
              const double u = std::min(1.0, t / 0.30);
              phDown += twoPi * (270.0 * std::exp(-t * 5.4) + 52.0) / kSampleRate;
              phUp += twoPi * (380.0 + 820.0 * u * u) / kSampleRate;
              lp = lp + 0.26f * (nextNoise(rng) - lp);
              const double env = (1.0 - std::exp(-t * 950.0)) * std::exp(-t * 9.2);
              const double plunge = 0.56 * std::sin(phDown);
              const double ring = 0.28 * std::sin(phUp) * std::exp(-t * 6.5);
              const double dust = static_cast<double>(lp) * 0.24 * std::exp(-t * 14.0);
              out[i] = static_cast<float>(std::clamp(env * (plunge + ring + dust) * 0.82, -0.98, 0.98));
          }
        });
    }

    bool ensureEngineRunning() {
        if (engineFailed_) return false;
#ifdef DPP_MIRROR_FAULT_TESTING
        // Synthetic headless fixtures verify buffer synthesis and event dispatch without audible output.
        return false;
#else
        @try {
            if (!engine_) {
                engine_ = [[AVAudioEngine alloc] init];
                voices_ = [NSMutableArray arrayWithCapacity:kVoiceCount];
                voiceBufs_ = [NSMutableArray arrayWithCapacity:kVoiceCount];
                for (std::size_t i = 0; i < kVoiceCount; ++i) {
                    AVAudioPlayerNode* node = [[AVAudioPlayerNode alloc] init];
                    [engine_ attachNode:node];
                    [engine_ connect:node to:engine_.mainMixerNode format:format_];
                    [voices_ addObject:node];
                    AVAudioPCMBuffer* scratch =
                        [[AVAudioPCMBuffer alloc] initWithPCMFormat:format_ frameCapacity:kMaxRenderFrames];
                    if (scratch) [voiceBufs_ addObject:scratch];
                }
            }
            if (!engine_.isRunning) {
                NSError* err = nil;
                if (![engine_ startAndReturnError:&err]) {
                    engineFailed_ = true;
                    return false;
                }
            }
            return true;
        } @catch (NSException*) {
            engineFailed_ = true;
            return false;
        }
#endif
    }

   public:
    BilliardsAudio() = default;
    ~BilliardsAudio() { stop(); }

    std::uint64_t playedCount() const { return played_; }

    void play(std::span<const SoundEvent> events, bool muted) {
        if (muted || events.empty()) return;
        @try {
            prepareBuffers();
            if (!format_) return;
            const bool live = ensureEngineRunning();
            for (const SoundEvent& ev : events) {
                ++played_;
                if (!live || voices_.count == 0) continue;
                AVAudioPCMBuffer* buf = nil;
                switch (ev.kind) {
                    case SoundKind::CueStrike:
                        buf = cueStrike_;
                        break;
                    case SoundKind::BallCollide:
                        buf = ev.gain < 0.34f ? ballSoft_ : (ev.gain < 0.72f ? ballMed_ : ballHard_);
                        break;
                    case SoundKind::RailBounce:
                        buf = railBounce_;
                        break;
                    case SoundKind::PocketDrop:
                        buf = pocketDrop_;
                        break;
                    case SoundKind::SlingStretch:
                        buf = slingStretch_;
                        break;
                    case SoundKind::SlingRelease:
                        buf = slingRelease_;
                        break;
                    case SoundKind::WoodImpact:
                        buf = woodImpact_;
                        break;
                    case SoundKind::StoneImpact:
                        buf = stoneImpact_;
                        break;
                    case SoundKind::GlassShatter:
                        buf = glassShatter_;
                        break;
                    case SoundKind::TargetPop:
                        buf = targetPop_;
                        break;
                    case SoundKind::MalletStrike:
                        buf = malletStrike_;
                        break;
                    case SoundKind::HockeyGoal:
                        buf = hockeyGoal_;
                        break;
                    case SoundKind::IconTap:
                        buf = iconTap_;
                        break;
                    case SoundKind::IconThud:
                        buf = iconThud_;
                        break;
                    case SoundKind::IconGrab:
                        buf = iconGrab_;
                        break;
                    case SoundKind::BlackHoleHum:
                        buf = blackHoleHum_;
                        break;
                    case SoundKind::BlackHoleSwallow:
                        buf = blackHoleSwallow_;
                        break;
                }
                if (!buf) continue;
                AVAudioPlayerNode* chosen = nil;
                std::size_t chosenIdx = 0;
                for (std::size_t k = 0; k < voices_.count; ++k) {
                    const std::size_t idx = (nextVoice_ + k) % voices_.count;
                    if (!voices_[idx].isPlaying) {
                        chosen = voices_[idx];
                        chosenIdx = idx;
                        nextVoice_ = (idx + 1) % voices_.count;
                        break;
                    }
                }
                if (!chosen) {
                    chosenIdx = nextVoice_ % voices_.count;
                    chosen = voices_[chosenIdx];
                    nextVoice_ = (nextVoice_ + 1) % voices_.count;
                    [chosen stop];
                }
                AVAudioPCMBuffer* toPlay = buf;
                const float pitch = std::clamp(ev.pitch, 0.88f, 1.14f);
                if (std::abs(pitch - 1.0f) > 0.01f && chosenIdx < voiceBufs_.count && buf.floatChannelData) {
                    AVAudioPCMBuffer* dst = voiceBufs_[chosenIdx];
                    const AVAudioFrameCount srcLen = buf.frameLength;
                    const AVAudioFrameCount outFrames = std::min<AVAudioFrameCount>(
                        kMaxRenderFrames,
                        static_cast<AVAudioFrameCount>(std::max(16.0f, static_cast<float>(srcLen) / pitch)));
                    if (dst && dst.floatChannelData && outFrames <= dst.frameCapacity) {
                        const float* src = buf.floatChannelData[0];
                        float* out = dst.floatChannelData[0];
                        for (AVAudioFrameCount f = 0; f < outFrames; ++f) {
                            const float sp = static_cast<float>(f) * pitch;
                            const auto i0 = static_cast<AVAudioFrameCount>(sp);
                            if (i0 >= srcLen) {
                                out[f] = 0.0f;
                                continue;
                            }
                            const AVAudioFrameCount i1 = std::min(i0 + 1, srcLen - 1);
                            const float frac = sp - static_cast<float>(i0);
                            out[f] = src[i0] + (src[i1] - src[i0]) * frac;
                        }
                        dst.frameLength = outFrames;
                        toPlay = dst;
                    }
                }
                chosen.volume = std::clamp(ev.gain, 0.04f, 1.0f);
                chosen.pan = std::clamp(ev.pan, -1.0f, 1.0f);
                [chosen scheduleBuffer:toPlay
                                atTime:nil
                               options:AVAudioPlayerNodeBufferInterrupts
                     completionHandler:nil];
                [chosen play];
            }
        } @catch (NSException*) {
        }
    }

    void stop() noexcept {
        @try {
            for (AVAudioPlayerNode* node in voices_) {
                if (node.isPlaying) [node stop];
            }
            if (engine_ && engine_.isRunning) [engine_ stop];
        } @catch (NSException*) {
        }
    }
};

}  // namespace dpp::mac
