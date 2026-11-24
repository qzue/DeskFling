#pragma once
// UI-thread transaction shared by native adapters. No platform data enters core.
#include <optional>
#include <string_view>

#include "core/scene/playground.h"
namespace dpp {
inline const char* migrationStageName(MigrationPreparationStage stage) {
    switch (stage) {
        case MigrationPreparationStage::Validation:
            return "snapshot_validation";
        case MigrationPreparationStage::CurrentLayout:
            return "current_layout";
        case MigrationPreparationStage::ResetLayout:
            return "reset_layout";
        case MigrationPreparationStage::World:
            return "physics_world";
        case MigrationPreparationStage::Draw:
            return "draw_list";
    }
    return "unknown";
}
// Never log arbitrary exception text: only fixed, path-free core failure categories.
inline const char* migrationFailureCategory(const std::exception& error) {
    const std::string_view message(error.what());
    for (const char* known :
         {"unsupported migration geometry", "invalid frozen position", "invalid migration icon",
          "rotated icon cannot fit", "full icon outside migration boundary",
          "migration overlap cannot be repaired", "migration pair has no room",
          "migration correction exhausted", "invalid frozen scene",
          "invalid scene coordinate dimensions or scale", "work area must be finite and at least 100 points",
          "icon size outside work area", "migration identity count mismatch", "billiards icons cannot fit"})
        if (message == known) return known;
    return "unclassified_cpp_exception";
}

class DisplayMigration {
    std::optional<FrozenScene> frozen_;
    double began_ = 0, changed_ = 0;
    uint64_t revision_ = 0, generation_ = 0;
    std::unique_ptr<Playground> candidate_;
    MigrationPreparationStage stage_ = MigrationPreparationStage::Validation;

   public:
    static constexpr double maximumWait = 3.0, settleTime = .15;
    bool active() const { return frozen_.has_value(); }
    uint64_t revision() const { return revision_; }
    uint64_t generation() const { return generation_; }
    void begin(Playground& game, double now) {
        if (active()) return;
        frozen_ = game.freezeForMigration();
        began_ = changed_ = now;
        ++revision_;
    }
    void changed(double now) {
        if (!active()) return;
        changed_ = now;
        candidate_.reset();
        ++revision_;
    }
    bool expired(double now) const {
        return active() && (!std::isfinite(now) || now < began_ || now - began_ >= maximumWait);
    }
    bool settled(double now) const { return active() && !expired(now) && now - changed_ >= settleTime; }
    void prepare(Vec2 size) { candidate_ = Playground::prepareMigration(frozen_.value(), size, &stage_); }
    MigrationPreparationStage preparationStage() const { return stage_; }
    const FrozenScene* frozenScene() const { return frozen_ ? &*frozen_ : nullptr; }
    Playground* candidate() const { return candidate_.get(); }
    std::unique_ptr<Playground> commit(uint64_t token, double now, bool nativeAndProtectionValid) {
        if (!active() || token != revision_) return {};
        if (expired(now) || !nativeAndProtectionValid) {
            invalidate();
            return {};
        }
        if (!candidate_) return {};
        auto result = std::move(candidate_);
        result->commitMigration();
        frozen_.reset();
        ++generation_;
        ++revision_;
        return result;
    }
    void invalidate() {
        candidate_.reset();
        frozen_.reset();
        ++revision_;
    }
};
}  // namespace dpp
