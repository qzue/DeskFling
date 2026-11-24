#include "core/modes/air_hockey_mode.h"
#include "core/modes/billiards_mode.h"
#include "core/modes/gravity_mode.h"
#include "core/modes/slingshot_mode.h"
namespace dpp {
const char* modeName(ModeKind k) {
    switch (k) {
        case ModeKind::Gravity:
            return "Gravity";
        case ModeKind::Billiards:
            return "Billiards";
        case ModeKind::Slingshot:
            return "Slingshot";
        case ModeKind::AirHockey:
            return "AirHockey";
    }
    return "Gravity";
}
std::unique_ptr<GameMode> createMode(ModeKind k, const SceneSetup& s) {
    switch (k) {
        case ModeKind::Gravity:
            return std::make_unique<GravityMode>(s);
        case ModeKind::Billiards:
            return std::make_unique<BilliardsMode>(s);
        case ModeKind::Slingshot:
            return std::make_unique<SlingshotMode>(s);
        case ModeKind::AirHockey:
            return std::make_unique<AirHockeyMode>(s);
    }
    return std::make_unique<GravityMode>(s);
}
}  // namespace dpp
