#pragma once
#include <algorithm>
#include <memory>
#include <set>
#include <stdexcept>

#include "core/scene/coordinates.h"
#include "core/scene/playground.h"
namespace dpp::mac {
enum class MirrorPhase { Closed, Static, Playing, Interactive };
// Adapter values only: URLs and resource identifiers remain in the native owner.
// View-configured drawing boxes are not measured Finder image/label boundaries.
struct MirrorItem {
    IconId id{};
    TextureId texture{};
    Vec2 rawDpos;
    Vec2 sceneAnchor;
    bool coordinateValidated = false;
    bool syntheticFixture = false;  // only opt-in native test harness; production adapter never sets this
    Vec2 dppDrawSize{48, 48};
    Vec2 dppCollisionBox{43.2f, 43.2f};  // GravityMode's existing 0.90 scale, rounded box
    float finderIconSize = 0;            // 0: explicit DPP 48pt fallback; otherwise raw view configuration
};
struct MirrorSnapshot {
    std::vector<MirrorItem> items;
    Rect screen, workArea;
    double capturedAt = 0;
    bool singleScreen = false;
    bool multiScreenAllowed = false;
    std::vector<Rect> screenFrames;
    std::vector<Rect> screenWorkAreasScene;
    Rect primaryArenaScene{};
    bool fullDesktop = false;
    std::size_t finderItemCount = 0;
    bool candidatePreview = false;  // visible originals, static-only manual alignment inspection
};
class MirrorSession {
    std::unique_ptr<Playground> game_;
    MirrorPhase phase_ = MirrorPhase::Closed;
    std::vector<MirrorItem> items_;
    bool candidatePreview_ = false;
    std::size_t startupInsetCount_ = 0;

   public:
    static SceneSetup scene(const MirrorSnapshot& snapshot, double now) {
        const bool screenCountValid = snapshot.singleScreen || snapshot.multiScreenAllowed;
        const bool itemCountValid =
            snapshot.fullDesktop
                ? ((snapshot.multiScreenAllowed ? true : !snapshot.items.empty()) &&
                   snapshot.items.size() <= 128 && snapshot.finderItemCount == snapshot.items.size())
                : (snapshot.items.size() >= 2 && snapshot.items.size() <= 3);
        if (!screenCountValid || !std::isfinite(now) || !std::isfinite(snapshot.capturedAt) ||
            now < snapshot.capturedAt || now - snapshot.capturedAt >= 300 || !itemCountValid ||
            (snapshot.candidatePreview && !snapshot.fullDesktop))
            throw std::invalid_argument("stale or unsupported mirror snapshot");
        SceneSetup setup;
        setup.workAreaSize = {snapshot.workArea.width, snapshot.workArea.height};
        setup.screenWorkAreas = snapshot.screenWorkAreasScene;
        setup.primaryArenaRect = snapshot.primaryArenaScene;
        if (!setup.workAreaSize.isFinite() || setup.workAreaSize.x < 100 || setup.workAreaSize.y < 100)
            throw std::invalid_argument("invalid mirror work area");
        std::set<IconId> ids;
        std::set<TextureId> textures;
        for (const auto& item : snapshot.items) {
            const float size = item.finderIconSize == 0 ? 48 : item.finderIconSize;
            const float half = size * .5f;
            const auto global = macTopLeftPointsToGlobal(item.rawDpos, snapshot.screen);
            const auto anchor = macGlobalPointsToScene(global, snapshot.workArea);
            const bool rawValid =
                snapshot.multiScreenAllowed
                    ? (!snapshot.screenFrames.empty()
                           ? macMultiDisplayPointValid(global, snapshot.screenFrames, 24.0f)
                           : global.isFinite())
                    : macPointerMatchesTopLeft(item.rawDpos, global, snapshot.screen, 0);
            if ((!item.coordinateValidated && !snapshot.candidatePreview) || item.id == IconId{0} ||
                item.texture == kNoTexture || !ids.insert(item.id).second ||
                !textures.insert(item.texture).second || !rawValid || !anchor.isFinite() ||
                !(item.sceneAnchor == anchor) || !std::isfinite(size) || size < 16 || size > 128 ||
                std::floor(size) != size || !(item.dppDrawSize == Vec2{size, size}) ||
                !item.dppCollisionBox.isFinite() || std::abs(item.dppCollisionBox.x - size * .90f) > .001f ||
                std::abs(item.dppCollisionBox.y - size * .90f) > .001f || size > setup.workAreaSize.x ||
                size > setup.workAreaSize.y ||
                (!snapshot.multiScreenAllowed &&
                 (anchor.x < 0 || anchor.y < 0 || anchor.x > setup.workAreaSize.x ||
                  anchor.y > setup.workAreaSize.y)) ||
                (!snapshot.fullDesktop &&
                 (anchor.x < half || anchor.y < half || anchor.x > setup.workAreaSize.x - half ||
                  anchor.y > setup.workAreaSize.y - half)))
                throw std::invalid_argument("invalid mirror identity, coordinate or DPP geometry");
            // Finder may place a center inside visibleFrame while its configured image box
            // crosses the edge. Inset only our initial replica; never change raw Finder data,
            // the fixed work area, or the original file. Candidate, active and reset share this seed.
            Vec2 center = anchor;
            if (snapshot.fullDesktop) {
                Rect clampRect{0.0f, 0.0f, setup.workAreaSize.x, setup.workAreaSize.y};
                if (!snapshot.screenWorkAreasScene.empty()) {
                    float bestDist2 = 1e30f;
                    for (const auto& sw : snapshot.screenWorkAreasScene) {
                        if (sw.width < size || sw.height < size) continue;
                        const float dx =
                            anchor.x < sw.x ? (sw.x - anchor.x)
                                            : (anchor.x > sw.x + sw.width ? (anchor.x - (sw.x + sw.width)) : 0.0f);
                        const float dy = anchor.y < sw.y
                                             ? (sw.y - anchor.y)
                                             : (anchor.y > sw.y + sw.height ? (anchor.y - (sw.y + sw.height)) : 0.0f);
                        const float d2 = dx * dx + dy * dy;
                        if (d2 < bestDist2) {
                            bestDist2 = d2;
                            clampRect = sw;
                        }
                    }
                }
                center = {std::clamp(anchor.x, clampRect.x + half, clampRect.x + clampRect.width - half),
                          std::clamp(anchor.y, clampRect.y + half, clampRect.y + clampRect.height - half)};
            }
            setup.icons.push_back({item.id, item.texture, center, item.dppDrawSize});
        }
        return setup;
    }
    void start(const MirrorSnapshot& snapshot, double now) {
        close();  // repeated start replaces the complete owned scene
        try {
            auto setup = scene(snapshot, now);
            for (std::size_t n = 0; n < setup.icons.size(); ++n)
                if (!(setup.icons[n].center == snapshot.items[n].sceneAnchor)) ++startupInsetCount_;
            game_ = std::make_unique<Playground>(std::move(setup));
            game_->input({InputType::TogglePause, {}, 0});
            game_->tick(0);
            items_ = snapshot.items;
            candidatePreview_ = snapshot.candidatePreview;
            phase_ = MirrorPhase::Static;
        } catch (...) {
            close();
            throw;
        }
    }
    void close() noexcept {
        game_.reset();
        items_.clear();
        phase_ = MirrorPhase::Closed;
        candidatePreview_ = false;
        startupInsetCount_ = 0;
    }
    void commitMigration(std::unique_ptr<Playground> candidate) {
        if (!candidate || !game_) throw std::logic_error("invalid migration commit");
        game_ = std::move(candidate);
        if (phase_ == MirrorPhase::Interactive) phase_ = MirrorPhase::Playing;
    }
    std::size_t startupInsetCount() const { return startupInsetCount_; }
    MirrorPhase phase() const { return phase_; }
    Playground* game() const { return game_.get(); }
    const std::vector<MirrorItem>& items() const { return items_; }
    void releaseInput() {
        if (game_) game_->input({InputType::PointerCancel, {}, 0});
        if (phase_ == MirrorPhase::Interactive) phase_ = MirrorPhase::Playing;
    }
    void playPause() {
        if (!game_ || candidatePreview_) return;
        releaseInput();
        phase_ = MirrorPhase::Playing;
        game_->input({InputType::TogglePause, {}, 0});
    }
    void reset() {
        if (!game_) return;
        releaseInput();
        game_->reset();
        if (!game_->paused()) game_->input({InputType::TogglePause, {}, 0});
        game_->tick(0);
        phase_ = MirrorPhase::Static;
    }
    void resetBilliards() {
        if (!game_) return;
        releaseInput();
        game_->reset();
        if (game_->paused()) game_->input({InputType::TogglePause, {}, 0});
        game_->tick(0);
        phase_ = MirrorPhase::Playing;
    }
    void ensurePlaying() {
        if (!game_ || candidatePreview_) return;
        releaseInput();
        if (game_->paused()) game_->input({InputType::TogglePause, {}, 0});
        phase_ = MirrorPhase::Playing;
    }
    void interact() {
        if (!game_ || candidatePreview_) return;
        releaseInput();
        if (game_->paused()) game_->input({InputType::TogglePause, {}, 0});
        phase_ = MirrorPhase::Interactive;
    }
    void input(const InputEvent& e) {
        if (phase_ == MirrorPhase::Interactive && game_) game_->input(e);
    }
};
}  // namespace dpp::mac
