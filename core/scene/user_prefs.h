#pragma once
// Platform-neutral user preference state & deterministic key=value serializer.
// Zero platform objects or real filesystem paths in core/.

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <string>
#include <string_view>

#include "core/scene/playground.h"

namespace dpp {

struct UserPreferences {
    PlayMode lastPlayMode = PlayMode::Gravity;
    // Display scope preference: 0..15 = single screen index (0 = primary), -1 = span all connected screens.
    int displayMode = 0;
    bool soundMuted = false;
    bool showFilenames = true;

    float gravityMagnitude = kEarthGravityMps2;
    GravityDirection gravityDirection = GravityDirection::Down;
    float gravityRestitution = kDefaultGravityRestitution;

    float blackHoleScale = 1.0f;
    float blackHoleElevation = BlackHoleState::kDefaultElevation;
    float blackHoleTilt = 0.0f;

    BilliardsRule billiardsRule = BilliardsRule::Free;
    BilliardsMatchMode billiardsMatchMode = BilliardsMatchMode::Solo;
    BilliardsAiDifficulty billiardsAiDifficulty = BilliardsAiDifficulty::Medium;
    IconSelectionPolicy iconSelection = IconSelectionPolicy::FirstN;
    ExtraIconsPolicy extraIcons = ExtraIconsPolicy::Hide;
    bool showBallBorders = true;
    bool showBallNumbers = false;
    BilliardsTableTheme tableTheme = BilliardsTableTheme::Full;
    bool showTableChrome = true;
    bool showAimGuide = true;
    bool showAiIntent = true;

    SlingshotPreset slingshotPreset = SlingshotPreset::TwinTowers;
    int slingshotAmmoCount = 4;
    bool slingshotShowTrajectory = true;
    int slingshotHighScore = 0;
    SlingshotProgress slingshotProgress{};
    std::vector<SlingshotCustomPiece> slingshotCustomPieces;

    AirHockeyMatchMode hockeyMatchMode = AirHockeyMatchMode::SoloAI;
    AirHockeyDifficulty hockeyDifficulty = AirHockeyDifficulty::Standard;
    int hockeyTargetGoals = 7;
    bool hockeyShowRinkChrome = true;

    std::string lastLanEndpoint = "127.0.0.1:39457";

    void sanitize(bool forColdBoot = false) {
        if (static_cast<unsigned>(lastPlayMode) > 4u) {
            lastPlayMode = PlayMode::Gravity;
        }
        displayMode = std::clamp(displayMode, -1, 15);
        gravityMagnitude = std::isfinite(gravityMagnitude)
                               ? std::clamp(gravityMagnitude, kMinGravityMps2, kMaxGravityMps2)
                               : kEarthGravityMps2;
        if (static_cast<unsigned>(gravityDirection) > 3u) {
            gravityDirection = GravityDirection::Down;
        }
        gravityRestitution =
            std::isfinite(gravityRestitution)
                ? std::clamp(gravityRestitution, kMinGravityRestitution, kMaxGravityRestitution)
                : kDefaultGravityRestitution;
        blackHoleScale = std::isfinite(blackHoleScale)
                             ? std::clamp(blackHoleScale, BlackHoleState::kMinScale, BlackHoleState::kMaxScale)
                             : 1.0f;
        blackHoleElevation = std::isfinite(blackHoleElevation)
                                 ? std::clamp(blackHoleElevation, BlackHoleState::kMinElevation,
                                              BlackHoleState::kMaxElevation)
                                 : BlackHoleState::kDefaultElevation;
        blackHoleTilt = std::isfinite(blackHoleTilt)
                            ? std::clamp(blackHoleTilt, BlackHoleState::kMinTilt, BlackHoleState::kMaxTilt)
                            : 0.0f;

        if (static_cast<unsigned>(billiardsRule) > 2u) {
            billiardsRule = BilliardsRule::Free;
        }
        if (static_cast<unsigned>(billiardsMatchMode) > 3u) {
            billiardsMatchMode = BilliardsMatchMode::Solo;
        }
        if (static_cast<unsigned>(billiardsAiDifficulty) > 3u) {
            billiardsAiDifficulty = BilliardsAiDifficulty::Medium;
        }
        if (static_cast<unsigned>(iconSelection) > 2u) {
            iconSelection = IconSelectionPolicy::FirstN;
        }
        if (static_cast<unsigned>(extraIcons) > 1u) {
            extraIcons = ExtraIconsPolicy::Hide;
        }
        if (static_cast<unsigned>(tableTheme) > 2u) {
            tableTheme = BilliardsTableTheme::Full;
        }
        if (tableTheme == BilliardsTableTheme::Full && !showTableChrome) {
            tableTheme = BilliardsTableTheme::Borderless;
        }
        showTableChrome = (tableTheme == BilliardsTableTheme::Full);

        if (static_cast<unsigned>(slingshotPreset) > 5u) {
            slingshotPreset = SlingshotPreset::TwinTowers;
        }
        slingshotAmmoCount = std::clamp(slingshotAmmoCount, 3, 6);
        slingshotHighScore = std::clamp(std::max(slingshotHighScore, slingshotProgress.highScore), 0, 9999999);
        slingshotProgress.highScore = slingshotHighScore;
        if (slingshotCustomPieces.size() > 48u) slingshotCustomPieces.resize(48u);

        if (static_cast<unsigned>(hockeyMatchMode) > 2u) {
            hockeyMatchMode = AirHockeyMatchMode::SoloAI;
        }
        if (static_cast<unsigned>(hockeyDifficulty) > 2u) {
            hockeyDifficulty = AirHockeyDifficulty::Standard;
        }
        if (hockeyTargetGoals != 5 && hockeyTargetGoals != 7 && hockeyTargetGoals != 11) {
            hockeyTargetGoals = 7;
        }

        std::string cleanEp;
        cleanEp.reserve(std::min<std::size_t>(lastLanEndpoint.size(), 96u));
        for (char c : lastLanEndpoint) {
            if (cleanEp.size() >= 96u) break;
            const auto uc = static_cast<unsigned char>(c);
            if (uc >= 33u && uc <= 126u && c != '=' && c != ';') {
                cleanEp.push_back(c);
            }
        }
        lastLanEndpoint = cleanEp;

        if (forColdBoot) {
            if (billiardsMatchMode == BilliardsMatchMode::Lan) {
                billiardsMatchMode = BilliardsMatchMode::Solo;
            }
            if (hockeyMatchMode == AirHockeyMatchMode::Lan) {
                hockeyMatchMode = AirHockeyMatchMode::SoloAI;
            }
            if (iconSelection == IconSelectionPolicy::Custom) {
                iconSelection = IconSelectionPolicy::FirstN;
            }
        }
    }

    [[nodiscard]] BilliardsConfig toBilliardsConfig() const {
        BilliardsConfig cfg = BilliardsConfig::desktop();
        cfg.rule = billiardsRule;
        cfg.matchMode = billiardsMatchMode;
        cfg.aiDifficulty = billiardsAiDifficulty;
        cfg.iconSelection = iconSelection;
        cfg.extraIcons = extraIcons;
        cfg.showBallBorders = showBallBorders;
        cfg.showBallNumbers = showBallNumbers;
        cfg.tableTheme =
            (tableTheme != BilliardsTableTheme::Full)
                ? tableTheme
                : (showTableChrome ? BilliardsTableTheme::Full : BilliardsTableTheme::Borderless);
        cfg.showTableChrome = (cfg.tableTheme == BilliardsTableTheme::Full);
        cfg.showAimGuide = showAimGuide;
        cfg.showAiIntent = showAiIntent;
        cfg.freeModeMaxRackBalls = (billiardsRule == BilliardsRule::Free) ? 21u : 0u;
        return cfg;
    }

    void fromBilliardsConfig(const BilliardsConfig& cfg) {
        billiardsRule = cfg.rule;
        billiardsMatchMode = cfg.matchMode;
        billiardsAiDifficulty = cfg.aiDifficulty;
        iconSelection = cfg.iconSelection;
        extraIcons = cfg.extraIcons;
        showBallBorders = cfg.showBallBorders;
        showBallNumbers = cfg.showBallNumbers;
        tableTheme = cfg.effectiveTableTheme();
        showTableChrome = (tableTheme == BilliardsTableTheme::Full);
        showAimGuide = cfg.showAimGuide;
        showAiIntent = cfg.showAiIntent;
    }

    [[nodiscard]] SlingshotConfig toSlingshotConfig() const {
        SlingshotConfig cfg;
        cfg.preset = slingshotPreset;
        cfg.ammoCount = slingshotAmmoCount;
        cfg.showTrajectory = slingshotShowTrajectory;
        cfg.iconSelection = iconSelection;
        cfg.customPieces = slingshotCustomPieces;
        return cfg;
    }

    void fromSlingshotConfig(const SlingshotConfig& cfg) {
        slingshotPreset = cfg.preset;
        slingshotAmmoCount = cfg.ammoCount;
        slingshotShowTrajectory = cfg.showTrajectory;
        iconSelection = cfg.iconSelection;
        if (!cfg.customPieces.empty()) {
            slingshotCustomPieces = cfg.customPieces;
        }
    }

    [[nodiscard]] AirHockeyConfig toAirHockeyConfig() const {
        AirHockeyConfig cfg;
        cfg.matchMode = hockeyMatchMode;
        cfg.difficulty = hockeyDifficulty;
        cfg.targetGoals = hockeyTargetGoals;
        cfg.showRinkChrome = hockeyShowRinkChrome;
        cfg.showSpectators = (extraIcons == ExtraIconsPolicy::BottomGravity);
        return cfg;
    }

    void fromAirHockeyConfig(const AirHockeyConfig& cfg) {
        hockeyMatchMode = cfg.matchMode;
        hockeyDifficulty = cfg.difficulty;
        hockeyTargetGoals = cfg.targetGoals;
        hockeyShowRinkChrome = cfg.showRinkChrome;
        extraIcons = cfg.showSpectators ? ExtraIconsPolicy::BottomGravity : ExtraIconsPolicy::Hide;
    }

    [[nodiscard]] std::string serializeIni() const {
        UserPreferences copy = *this;
        copy.sanitize(false);
        const std::string progStr = encodeSlingshotProgress(copy.slingshotProgress);
        const std::string custStr = encodeSlingshotCustomPieces(copy.slingshotCustomPieces);
        char buf[1280]{};
        const int n = std::snprintf(
            buf, sizeof(buf),
            "version=2\n"
            "last_play_mode=%d\n"
            "sound_muted=%d\n"
            "show_filenames=%d\n"
            "gravity_mps2=%.4f\n"
            "gravity_dir=%d\n"
            "gravity_restitution=%.4f\n"
            "bh_scale=%.4f\n"
            "bh_elevation=%.5f\n"
            "bh_tilt=%.5f\n"
            "billiards_rule=%d\n"
            "billiards_match=%d\n"
            "billiards_ai=%d\n"
            "icon_selection=%d\n"
            "extra_icons=%d\n"
            "ball_borders=%d\n"
            "ball_numbers=%d\n"
            "table_theme=%d\n"
            "table_chrome=%d\n"
            "aim_guide=%d\n"
            "ai_intent=%d\n"
            "sling_preset=%d\n"
            "sling_ammo=%d\n"
            "sling_trajectory=%d\n"
            "sling_high_score=%d\n"
            "hockey_match=%d\n"
            "hockey_diff=%d\n"
            "hockey_goals=%d\n"
            "hockey_chrome=%d\n"
            "lan_endpoint=%s\n",
            static_cast<int>(copy.lastPlayMode), copy.soundMuted ? 1 : 0, copy.showFilenames ? 1 : 0,
            static_cast<double>(copy.gravityMagnitude), static_cast<int>(copy.gravityDirection),
            static_cast<double>(copy.gravityRestitution),
            static_cast<double>(copy.blackHoleScale), static_cast<double>(copy.blackHoleElevation),
            static_cast<double>(copy.blackHoleTilt), static_cast<int>(copy.billiardsRule),
            static_cast<int>(copy.billiardsMatchMode), static_cast<int>(copy.billiardsAiDifficulty),
            static_cast<int>(copy.iconSelection), static_cast<int>(copy.extraIcons),
            copy.showBallBorders ? 1 : 0, copy.showBallNumbers ? 1 : 0, static_cast<int>(copy.tableTheme),
            copy.showTableChrome ? 1 : 0, copy.showAimGuide ? 1 : 0, copy.showAiIntent ? 1 : 0,
            static_cast<int>(copy.slingshotPreset),
            copy.slingshotAmmoCount, copy.slingshotShowTrajectory ? 1 : 0, copy.slingshotHighScore,
            static_cast<int>(copy.hockeyMatchMode), static_cast<int>(copy.hockeyDifficulty),
            copy.hockeyTargetGoals, copy.hockeyShowRinkChrome ? 1 : 0, copy.lastLanEndpoint.c_str());
        if (n <= 0) return {};
        std::string out(buf, static_cast<std::size_t>(std::min<int>(n, static_cast<int>(sizeof(buf) - 1))));
        out += "sling_progress=" + progStr + "\n";
        out += "display_mode=" + std::to_string(copy.displayMode) + "\n";
        if (!copy.slingshotCustomPieces.empty()) {
            out += "sling_custom=" + custStr + "\n";
        }
        return out;
    }

    [[nodiscard]] static UserPreferences parseIni(std::string_view text, bool forColdBoot = false) {
        UserPreferences p;
        auto trim = [](std::string_view s) {
            while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r'))
                s.remove_prefix(1);
            while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r'))
                s.remove_suffix(1);
            return s;
        };
        auto parseIntVal = [](std::string_view s, int& out) {
            int v = 0;
            const auto res = std::from_chars(s.data(), s.data() + s.size(), v);
            if (res.ec == std::errc{} && res.ptr == s.data() + s.size()) {
                out = v;
                return true;
            }
            return false;
        };
        auto parseFloatVal = [](std::string_view s, float& out) {
            if (s.empty() || s.size() > 31u) return false;
            char tmp[32]{};
            for (std::size_t i = 0; i < s.size(); ++i) tmp[i] = s[i];
            char* endPtr = nullptr;
            const float v = std::strtof(tmp, &endPtr);
            if (endPtr == tmp + s.size() && std::isfinite(v)) {
                out = v;
                return true;
            }
            return false;
        };

        bool seenTableTheme = false;
        std::size_t pos = 0;
        while (pos < text.size()) {
            const std::size_t nl = text.find('\n', pos);
            const std::string_view line =
                trim(nl == std::string_view::npos ? text.substr(pos) : text.substr(pos, nl - pos));
            pos = (nl == std::string_view::npos) ? text.size() : (nl + 1u);
            if (line.empty() || line.front() == '#' || line.front() == ';' || line.front() == '[') continue;
            const std::size_t eq = line.find('=');
            if (eq == std::string_view::npos) continue;
            const std::string_view key = trim(line.substr(0, eq));
            const std::string_view val = trim(line.substr(eq + 1u));
            int iv = 0;
            float fv = 0.0f;
            if (key == "last_play_mode" && parseIntVal(val, iv)) {
                p.lastPlayMode = static_cast<PlayMode>(std::clamp(iv, 0, 4));
            } else if (key == "sound_muted" && parseIntVal(val, iv)) {
                p.soundMuted = (iv != 0);
            } else if (key == "show_filenames" && parseIntVal(val, iv)) {
                p.showFilenames = (iv != 0);
            } else if (key == "gravity_mps2" && parseFloatVal(val, fv)) {
                p.gravityMagnitude = fv;
            } else if (key == "gravity_dir" && parseIntVal(val, iv)) {
                p.gravityDirection = static_cast<GravityDirection>(std::clamp(iv, 0, 3));
            } else if (key == "gravity_restitution" && parseFloatVal(val, fv)) {
                p.gravityRestitution = fv;
            } else if (key == "bh_scale" && parseFloatVal(val, fv)) {
                p.blackHoleScale = fv;
            } else if (key == "bh_elevation" && parseFloatVal(val, fv)) {
                p.blackHoleElevation = fv;
            } else if (key == "bh_tilt" && parseFloatVal(val, fv)) {
                p.blackHoleTilt = fv;
            } else if (key == "billiards_rule" && parseIntVal(val, iv)) {
                p.billiardsRule = static_cast<BilliardsRule>(std::clamp(iv, 0, 2));
            } else if (key == "billiards_match" && parseIntVal(val, iv)) {
                p.billiardsMatchMode = static_cast<BilliardsMatchMode>(std::clamp(iv, 0, 3));
            } else if (key == "billiards_ai" && parseIntVal(val, iv)) {
                p.billiardsAiDifficulty = static_cast<BilliardsAiDifficulty>(std::clamp(iv, 0, 3));
            } else if (key == "icon_selection" && parseIntVal(val, iv)) {
                p.iconSelection = static_cast<IconSelectionPolicy>(std::clamp(iv, 0, 2));
            } else if (key == "extra_icons" && parseIntVal(val, iv)) {
                p.extraIcons = static_cast<ExtraIconsPolicy>(std::clamp(iv, 0, 1));
            } else if (key == "ball_borders" && parseIntVal(val, iv)) {
                p.showBallBorders = (iv != 0);
            } else if (key == "ball_numbers" && parseIntVal(val, iv)) {
                p.showBallNumbers = (iv != 0);
            } else if (key == "table_theme" && parseIntVal(val, iv)) {
                p.tableTheme = static_cast<BilliardsTableTheme>(std::clamp(iv, 0, 2));
                p.showTableChrome = (p.tableTheme == BilliardsTableTheme::Full);
                seenTableTheme = true;
            } else if (key == "table_chrome" && parseIntVal(val, iv)) {
                if (!seenTableTheme) {
                    p.showTableChrome = (iv != 0);
                    p.tableTheme = (iv != 0) ? BilliardsTableTheme::Full : BilliardsTableTheme::Borderless;
                }
            } else if (key == "aim_guide" && parseIntVal(val, iv)) {
                p.showAimGuide = (iv != 0);
            } else if (key == "ai_intent" && parseIntVal(val, iv)) {
                p.showAiIntent = (iv != 0);
            } else if (key == "sling_preset" && parseIntVal(val, iv)) {
                p.slingshotPreset = static_cast<SlingshotPreset>(std::clamp(iv, 0, 5));
            } else if (key == "sling_ammo" && parseIntVal(val, iv)) {
                p.slingshotAmmoCount = iv;
            } else if (key == "sling_trajectory" && parseIntVal(val, iv)) {
                p.slingshotShowTrajectory = (iv != 0);
            } else if (key == "sling_high_score" && parseIntVal(val, iv)) {
                p.slingshotHighScore = iv;
            } else if (key == "sling_progress") {
                p.slingshotProgress = decodeSlingshotProgress(val);
            } else if (key == "sling_custom") {
                p.slingshotCustomPieces = decodeSlingshotCustomPieces(val);
            } else if (key == "hockey_match" && parseIntVal(val, iv)) {
                p.hockeyMatchMode = static_cast<AirHockeyMatchMode>(std::clamp(iv, 0, 2));
            } else if (key == "hockey_diff" && parseIntVal(val, iv)) {
                p.hockeyDifficulty = static_cast<AirHockeyDifficulty>(std::clamp(iv, 0, 2));
            } else if (key == "hockey_goals" && parseIntVal(val, iv)) {
                p.hockeyTargetGoals = iv;
            } else if (key == "hockey_chrome" && parseIntVal(val, iv)) {
                p.hockeyShowRinkChrome = (iv != 0);
            } else if (key == "lan_endpoint") {
                p.lastLanEndpoint = std::string(val);
            } else if (key == "display_mode" && parseIntVal(val, iv)) {
                p.displayMode = std::clamp(iv, -1, 15);
            }
        }
        p.sanitize(forColdBoot);
        return p;
    }
};

}  // namespace dpp
