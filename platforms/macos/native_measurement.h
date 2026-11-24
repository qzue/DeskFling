#pragma once
#include "core/scene/playground.h"
#include "platforms/macos/diagnostics.h"
namespace dpp::mac {
inline NSDictionary* statistics(const Samples& s) {
    if (!s.count())
        return
            @{@"samples" : @0,
              @"median_ms" : NSNull.null,
              @"p95_ms" : NSNull.null,
              @"p99_ms" : NSNull.null};
    return @{
        @"samples" : @(s.count()),
        @"median_ms" : @(s.percentile(.5)),
        @"p95_ms" : @(s.percentile(.95)),
        @"p99_ms" : @(s.percentile(.99))
    };
}
// Opt-in native measurement only. The ordinary sandbox retains its existing pause-on-focus-loss policy.
class NativeMeasurement {
   public:
    NativeMeasurement(double seconds, NSString* path)
        : duration_(seconds),
          path_(path),
          start_(SteadyClock::now()),
          phaseStart_(start_),
          cpuStart_(processCPUSeconds()) {
        phases_ = [NSMutableArray array];
        trend_ = [NSMutableArray array];
    }
    void exercise(Playground& game) {
        if (userInteractions_ == 0 && phase_ == 0 && elapsedMs(start_) / 1000 >= nextReset_) {
            game.reset();
            nextReset_ += 2;
        }
    }
    void userInteraction() { ++userInteractions_; }
    void focusLost() { focusLost_ = true; }
    bool sample(Playground& game, NSUInteger images, NSUInteger layers, BOOL active, BOOL visible,
                CGFloat scale) {
        const double seconds = elapsedMs(start_) / 1000;
        ++observations_;
        if (active) ++activeObservations_;
        if (visible) ++visibleObservations_;
        if (game.mode().kind() != ModeKind::Gravity || game.mode().stats().bodies != 50 ||
            game.paused() != (phase_ == 3) || !active || !visible)
            workloadChanged_ = true;
        [trend_ addObject:@{
            @"elapsed_seconds" : @(seconds),
            @"phase" : phaseName(),
            @"mode" : game.mode().kind() == ModeKind::Gravity ? @"gravity" : @"billiards",
            @"resident_bytes" : residentMemory(),
            @"process_cpu_seconds" : finite(processCPUSeconds()),
            @"images" : @(images),
            @"layer_pool" : @(layers),
            @"bodies" : @(game.mode().stats().bodies),
            @"awake" : @(game.mode().stats().awakeBodies),
            @"paused" : @(game.paused()),
            @"key_and_active" : @(active),
            @"visible_not_fully_occluded" : @(visible),
            @"backing_scale" : @(scale)
        }];
        const double fraction = seconds / duration_;
        const int desired = fraction >= 1      ? 4
                            : fraction >= .925 ? 3
                            : fraction >= .85  ? 2
                            : fraction >= .8   ? 1
                                               : 0;
        if (desired > phase_) {
            endPhase(game);
            phase_ = desired;
            if (phase_ == 4) return true;
            game.perf = PerformanceMonitor{};
            phaseStart_ = SteadyClock::now();
            cpuStart_ = processCPUSeconds();
            observations_ = activeObservations_ = visibleObservations_ = 0;
            if (userInteractions_ == 0 && phase_ == 3 && !game.paused())
                game.input({InputType::TogglePause, {}, 0});
        }
        return false;
    }
    bool finish(Playground& game, bool completed) {
        if (finished_) return true;
        finished_ = true;
        if (phase_ < 4) endPhase(game);
        NSDictionary* report = @{
            @"schema_version" : @1,
            @"kind" : @"native_sandbox_measurement",
            @"environment" : environment(),
            @"build_config" : @DPP_BUILD_CONFIG,
            @"screens" : screens(),
            @"scene_points" : @[ @1100, @700 ],
            @"synthetic_objects" : @50,
            @"requested_seconds" : @(duration_),
            @"elapsed_seconds" : @(elapsedMs(start_) / 1000),
            @"completed" : @(completed),
            @"workload_valid" : @(completed && !workloadChanged_ && !focusLost_ && userInteractions_ == 0),
            @"user_interactions" : @(userInteractions_),
            @"focus_lost" : @(focusLost_),
            @"requested_workload" :
                @"80% gravity reset every 2s; 5% settling; 7.5% natural rest; 7.5% pause; focus "
                @"loss pauses simulation",
            @"cpu_scope" :
                @"getrusage RUSAGE_SELF user+system; 100% = one logical CPU; excludes WindowServer/GPU",
            @"gpu_completion_ms" : NSNull.null,
            @"presented_fps" : NSNull.null,
            @"interval_scope" : @"timer callbacks with active simulation at both endpoints; excludes "
                                @"initial/reset/resume/idle wake intervals; not presentation timestamps",
            @"phases" : phases_,
            @"one_second_samples" : trend_
        };
        return writeJSON(report, path_);
    }

   private:
    static id finite(double v) { return std::isfinite(v) ? @(v) : NSNull.null; }
    NSString* phaseName() const {
        return @[ @"active", @"settling", @"rest", @"paused" ][std::min(phase_, 3)];
    }
    void endPhase(const Playground& game) {
        const auto& p = game.perf;
        const double wall = elapsedMs(phaseStart_) / 1000;
        const double cpu = processCPUSeconds() - cpuStart_;
        [phases_ addObject:@{
            @"phase" : phaseName(),
            @"wall_seconds" : @(wall),
            @"cpu_seconds" : finite(cpu),
            @"average_process_cpu_percent" : finite(wall > 0 ? cpu / wall * 100 : NAN),
            @"observations" : @(observations_),
            @"key_and_active_observations" : @(activeObservations_),
            @"visible_observations" : @(visibleObservations_),
            @"input" : statistics(p.input),
            @"physics" : statistics(p.physics),
            @"rules" : statistics(p.rules),
            @"draw_data" : statistics(p.draw),
            @"cpu_render_submission" : statistics(p.submit),
            @"active_callback_interval" : statistics(p.interval),
            @"active_callback_stalls_over_33_34_ms" : @(p.stalls)
        }];
    }
    double duration_;
    NSString* path_;
    SteadyClock::time_point start_, phaseStart_;
    double cpuStart_;
    double nextReset_ = 2;
    int phase_ = 0;
    bool finished_ = false;
    bool workloadChanged_ = false, focusLost_ = false;
    NSUInteger userInteractions_ = 0;
    NSUInteger observations_ = 0, activeObservations_ = 0, visibleObservations_ = 0;
    NSMutableArray* phases_;
    NSMutableArray* trend_;
};
}  // namespace dpp::mac
