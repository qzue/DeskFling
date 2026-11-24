#include <iostream>
#include <string>

#include "core/scene/coordinates.h"
#include "core/scene/playground.h"
int main(int argc, char** argv) {
    try {
        const auto count = argc > 1 ? std::stoul(argv[1]) : 50;
        const auto frames = argc > 2 ? std::stoul(argv[2]) : 36000;
        const std::string scenario = argc > 3 ? argv[3] : "fall";
        if (count > 5000 || frames < 1 || frames > 10000000) return 2;
        if (scenario != "fall" && scenario != "stack" && scenario != "throw" && scenario != "sleep" &&
            scenario != "pause" && scenario != "switch")
            return 2;
        auto setup = dpp::syntheticScene(count, {1920, 1080});
        if (scenario == "stack")
            for (auto& icon : setup.icons) icon.center.x = 960 + (icon.center.x - 960) * .4f;
        dpp::Playground app(setup);
        if (scenario == "sleep") {
            for (int i = 0; i < 36000 && !app.idle(); ++i) app.tick(1.0 / 60);
            if (!app.idle()) {
                std::cerr << "sleep warmup did not settle\n";
                return 3;
            }
            app.perf = dpp::PerformanceMonitor{};
        }
        const dpp::SceneToPhysics coords(setup.workAreaSize);
        dpp::Vec2 grabOrigin;
        const auto start = dpp::SteadyClock::now();
        for (std::size_t i = 0; i < frames; ++i) {
            const double time = static_cast<double>(i) / 60;
            if ((scenario == "fall" || scenario == "stack") && i % 600 == 0) app.reset();
            if (scenario == "switch" && i % 120 == 0)
                app.switchMode(app.mode().kind() == dpp::ModeKind::Gravity ? dpp::ModeKind::Billiards
                                                                           : dpp::ModeKind::Gravity);
            if (scenario == "pause" && i % 120 == 0) app.input({dpp::InputType::TogglePause, {}, time});
            if (scenario == "throw" && count) {
                if (i % 120 == 0) {
                    grabOrigin = coords.toScene(app.mode().world().state(0).position);
                    app.input({dpp::InputType::PointerDown, grabOrigin, time});
                }
                if (i % 120 < 45)
                    app.input({dpp::InputType::PointerMove,
                               grabOrigin + dpp::Vec2{static_cast<float>(i % 120) * 5,
                                                      -static_cast<float>(i % 120) * 8},
                               time});
                if (i % 120 == 45)
                    app.input({dpp::InputType::PointerUp, grabOrigin + dpp::Vec2{225, -360}, time});
            }
            app.tick(1.0 / 60);
        }
        std::cout << "synthetic=true rendered=false scenario=" << scenario << " objects=" << count
                  << " frames=" << frames << " elapsed_ms=" << dpp::elapsedMs(start)
                  << " substeps=4 fixed_dt=1/60 awake=" << app.mode().stats().awakeBodies << '\n';
        auto report = [](const char* name, const dpp::Samples& s) {
            std::cout << name << " samples=" << s.count() << " median_ms=" << s.percentile(.5)
                      << " p95_ms=" << s.percentile(.95) << " p99_ms=" << s.percentile(.99) << '\n';
        };
        report("input", app.perf.input);
        report("physics", app.perf.physics);
        report("rules", app.perf.rules);
        report("draw", app.perf.draw);
        std::cout << "gpu=unavailable display_fps=unmeasured memory=unmeasured\n";
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 2;
    }
}
