#pragma once
// Pure C++20 wire codec for 2-player LAN Desktop Billiards (newline-delimited UTF-8 JSON).
// Compatible wire-for-wire with macOS NSJSONSerialization packets in platforms/macos/billiards_lan.h.
// Never serializes filenames, paths, icons, or display identifiers.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "core/modes/air_hockey_mode.h"
#include "core/modes/billiards_mode.h"

namespace dpp::lan_codec {

inline constexpr std::uint16_t kDefaultDiscoveryPort = 39458;

enum class LanGameKind : std::uint8_t {
    Billiards = 0,
    AirHockey = 1,
};

struct RulePacket {
    BilliardsRule rule = BilliardsRule::Free;
};

struct ActionPacket {
    BilliardsRemoteAction action{};
};

struct SnapshotPacket {
    BilliardsLanSnapshot snapshot{};
    std::vector<SoundEvent> sounds;
};

struct HockeyInputPacket {
    AirHockeyRemoteInput input{};
};

struct HockeyStatePacket {
    AirHockeyLanState state{};
    std::vector<SoundEvent> sounds;
};

struct BeaconPacket {
    std::uint32_t roomId = 0;
    std::uint16_t tcpPort = 39457;
    LanGameKind game = LanGameKind::Billiards;
    int variant = 0;
    bool reconnectWaiting = false;
    std::array<char, 24> roomTag{};
};

struct DiscoverProbePacket {
    LanGameKind preferredGame = LanGameKind::Billiards;
};

struct SessionHelloPacket {
    std::uint32_t sessionToken = 0;
    bool resume = false;
};

struct DiscoveredLanRoom {
    std::uint32_t roomId = 0;
    std::string hostIp;
    std::uint16_t tcpPort = 39457;
    LanGameKind game = LanGameKind::Billiards;
    int variant = 0;
    bool reconnectWaiting = false;
    std::string roomTag;
    double lastSeenSeconds = 0.0;

    [[nodiscard]] std::string endpoint() const {
        char buf[64]{};
        std::snprintf(buf, sizeof(buf), "%s:%u", hostIp.c_str(), static_cast<unsigned>(tcpPort));
        return buf;
    }

    [[nodiscard]] std::string displayLabel() const {
        const char* modeStr = (game == LanGameKind::AirHockey) ? "曲棍球" : "台球";
        char ruleBuf[32]{};
        if (game == LanGameKind::AirHockey) {
            std::snprintf(ruleBuf, sizeof(ruleBuf), "%d球", std::clamp(variant, 1, 21));
        } else {
            const char* r = (variant == 1) ? "黑八" : (variant == 2 ? "斯诺克" : "自由");
            std::snprintf(ruleBuf, sizeof(ruleBuf), "%s", r);
        }
        char out[128]{};
        std::snprintf(out, sizeof(out), "%s·%s [%s]%s (%s:%u)", modeStr, ruleBuf,
                      roomTag.empty() ? "房间" : roomTag.c_str(), reconnectWaiting ? " 待重连" : "",
                      hostIp.c_str(), static_cast<unsigned>(tcpPort));
        return out;
    }
};

using DecodedPacket =
    std::variant<std::monostate, RulePacket, ActionPacket, SnapshotPacket, HockeyInputPacket,
                 HockeyStatePacket, BeaconPacket, DiscoverProbePacket, SessionHelloPacket>;

namespace detail {

inline void appendFloat(std::string& out, float v) {
    if (!std::isfinite(v)) {
        out += "0";
        return;
    }
    char buf[48]{};
    const int n = std::snprintf(buf, sizeof(buf), "%.6g", static_cast<double>(v));
    if (n <= 0) {
        out += "0";
        return;
    }
    for (int i = 0; i < n && buf[i] != '\0'; ++i) {
        if (buf[i] == ',') buf[i] = '.';
    }
    out.append(buf, static_cast<std::size_t>(n));
}

inline void appendInt(std::string& out, int v) {
    char buf[32]{};
    const int n = std::snprintf(buf, sizeof(buf), "%d", v);
    if (n > 0) out.append(buf, static_cast<std::size_t>(n));
}

inline void appendVec2(std::string& out, Vec2 v) {
    out.push_back('[');
    appendFloat(out, v.x);
    out.push_back(',');
    appendFloat(out, v.y);
    out.push_back(']');
}

inline void appendEscapedString(std::string& out, std::string_view s) {
    out.push_back('"');
    for (unsigned char c : s) {
        if (c == '\0') break;
        switch (c) {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                if (c >= 0x20u) out.push_back(static_cast<char>(c));
                break;
        }
    }
    out.push_back('"');
}

struct Reader {
    const char* p = nullptr;
    const char* end = nullptr;

    void skipWs() {
        while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) ++p;
    }

    bool consume(char ch) {
        skipWs();
        if (p < end && *p == ch) {
            ++p;
            return true;
        }
        return false;
    }

    bool parseString(std::string& out) {
        skipWs();
        if (p >= end || *p != '"') return false;
        ++p;
        out.clear();
        while (p < end) {
            const char c = *p++;
            if (c == '"') return true;
            if (c == '\\') {
                if (p >= end) return false;
                const char esc = *p++;
                switch (esc) {
                    case '"':
                    case '\\':
                    case '/':
                        out.push_back(esc);
                        break;
                    case 'b':
                        out.push_back('\b');
                        break;
                    case 'f':
                        out.push_back('\f');
                        break;
                    case 'n':
                        out.push_back('\n');
                        break;
                    case 'r':
                        out.push_back('\r');
                        break;
                    case 't':
                        out.push_back('\t');
                        break;
                    case 'u': {
                        if (end - p < 4) return false;
                        std::uint32_t cp = 0;
                        for (int i = 0; i < 4; ++i) {
                            const char h = p[i];
                            cp <<= 4u;
                            if (h >= '0' && h <= '9')
                                cp |= static_cast<std::uint32_t>(h - '0');
                            else if (h >= 'a' && h <= 'f')
                                cp |= static_cast<std::uint32_t>(h - 'a' + 10);
                            else if (h >= 'A' && h <= 'F')
                                cp |= static_cast<std::uint32_t>(h - 'A' + 10);
                            else
                                return false;
                        }
                        p += 4;
                        if (cp <= 0x7Fu) {
                            out.push_back(static_cast<char>(cp));
                        } else if (cp <= 0x7FFu) {
                            out.push_back(static_cast<char>(0xC0u | (cp >> 6u)));
                            out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
                        } else {
                            out.push_back(static_cast<char>(0xE0u | (cp >> 12u)));
                            out.push_back(static_cast<char>(0x80u | ((cp >> 6u) & 0x3Fu)));
                            out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
                        }
                        break;
                    }
                    default:
                        out.push_back(esc);
                        break;
                }
            } else {
                out.push_back(c);
            }
        }
        return false;
    }

    bool parseDouble(double& out) {
        skipWs();
        if (p >= end) return false;
        bool neg = false;
        if (*p == '-') {
            neg = true;
            ++p;
        } else if (*p == '+') {
            ++p;
        }
        if (p >= end || (*p < '0' || *p > '9')) return false;
        double val = 0.0;
        while (p < end && *p >= '0' && *p <= '9') {
            val = val * 10.0 + static_cast<double>(*p - '0');
            ++p;
        }
        if (p < end && *p == '.') {
            ++p;
            double place = 0.1;
            while (p < end && *p >= '0' && *p <= '9') {
                val += static_cast<double>(*p - '0') * place;
                place *= 0.1;
                ++p;
            }
        }
        if (p < end && (*p == 'e' || *p == 'E')) {
            ++p;
            bool expNeg = false;
            if (p < end && (*p == '-' || *p == '+')) {
                expNeg = (*p == '-');
                ++p;
            }
            if (p >= end || *p < '0' || *p > '9') return false;
            int expVal = 0;
            while (p < end && *p >= '0' && *p <= '9') {
                if (expVal < 1000) expVal = expVal * 10 + (*p - '0');
                ++p;
            }
            val *= std::pow(10.0, expNeg ? -expVal : expVal);
        }
        out = neg ? -val : val;
        return std::isfinite(out);
    }

    bool parseFloat(float& out) {
        double d = 0.0;
        if (!parseDouble(d)) return false;
        const float f = static_cast<float>(d);
        if (!std::isfinite(f)) return false;
        out = f;
        return true;
    }

    bool parseInt(int& out) {
        double d = 0.0;
        if (!parseDouble(d)) return false;
        if (d < -2147483647.0 || d > 2147483647.0) return false;
        out = static_cast<int>(std::lround(d));
        return true;
    }

    bool parseVec2(Vec2& out, Vec2 fallback) {
        if (!consume('[')) return false;
        float x = fallback.x;
        float y = fallback.y;
        if (!parseFloat(x) || !consume(',') || !parseFloat(y)) return false;
        while (consume(',')) {
            if (!skipValue()) return false;
        }
        if (!consume(']')) return false;
        out = (std::isfinite(x) && std::isfinite(y)) ? Vec2{x, y} : fallback;
        return true;
    }

    bool parseIntPair(int& a, int& b) {
        if (!consume('[')) return false;
        if (!parseInt(a) || !consume(',') || !parseInt(b)) return false;
        while (consume(',')) {
            if (!skipValue()) return false;
        }
        return consume(']');
    }

    bool skipValue(int depth = 0) {
        if (depth > 8) return false;
        skipWs();
        if (p >= end) return false;
        if (*p == '"') {
            std::string tmp;
            return parseString(tmp);
        }
        if (*p == '[') {
            ++p;
            if (consume(']')) return true;
            for (;;) {
                if (!skipValue(depth + 1)) return false;
                if (consume(',')) continue;
                return consume(']');
            }
        }
        if (*p == '{') {
            ++p;
            if (consume('}')) return true;
            for (;;) {
                std::string k;
                if (!parseString(k) || !consume(':') || !skipValue(depth + 1)) return false;
                if (consume(',')) continue;
                return consume('}');
            }
        }
        if (end - p >= 4 && std::memcmp(p, "true", 4) == 0) {
            p += 4;
            return true;
        }
        if (end - p >= 5 && std::memcmp(p, "false", 5) == 0) {
            p += 5;
            return true;
        }
        if (end - p >= 4 && std::memcmp(p, "null", 4) == 0) {
            p += 4;
            return true;
        }
        double d = 0.0;
        return parseDouble(d);
    }
};

}  // namespace detail

[[nodiscard]] inline std::string encodeRule(BilliardsRule rule) {
    std::string out = "{\"t\":\"rule\",\"r\":";
    detail::appendInt(out, static_cast<int>(rule));
    out += "}\n";
    return out;
}

[[nodiscard]] inline std::string encodeAction(const BilliardsRemoteAction& act) {
    std::string out;
    out.reserve(160);
    out += "{\"t\":\"act\",\"k\":";
    detail::appendInt(out, static_cast<int>(act.kind));
    out += ",\"ad\":";
    detail::appendVec2(out, act.aimDir);
    out += ",\"pw\":";
    detail::appendFloat(out, act.power);
    out += ",\"to\":";
    detail::appendVec2(out, act.tipOffset);
    out += ",\"pl\":";
    detail::appendVec2(out, act.placeNormPos);
    out += ",\"pr\":";
    detail::appendFloat(out, act.pullRatio);
    out += "}\n";
    return out;
}

[[nodiscard]] inline std::string encodeSnapshot(const BilliardsLanSnapshot& snap,
                                                std::span<const SoundEvent> sounds) {
    std::string out;
    out.reserve(512 + snap.balls.size() * 44);
    out += "{\"t\":\"snap\",\"st\":";
    detail::appendInt(out, static_cast<int>(snap.state));
    out += ",\"ru\":";
    detail::appendInt(out, static_cast<int>(snap.rule));
    out += ",\"cp\":";
    detail::appendInt(out, snap.currentPlayer);
    out += ",\"wn\":";
    detail::appendInt(out, snap.winner);
    out += ",\"sc\":[";
    detail::appendInt(out, snap.scores[0]);
    out.push_back(',');
    detail::appendInt(out, snap.scores[1]);
    out += "],\"pg\":[";
    detail::appendInt(out, static_cast<int>(snap.playerGroups[0]));
    out.push_back(',');
    detail::appendInt(out, static_cast<int>(snap.playerGroups[1]));
    out += "],\"sn\":";
    detail::appendInt(out, snap.snookerTarget);
    out += ",\"sh\":";
    detail::appendInt(out, snap.shots);
    out += ",\"pk\":";
    detail::appendInt(out, snap.pocketedCount);
    out += ",\"wp\":";
    detail::appendInt(out, snap.whitePocketed ? 1 : 0);
    out += ",\"w\":";
    detail::appendVec2(out, snap.whiteNormPos);
    out += ",\"pl\":";
    detail::appendVec2(out, snap.placeNormPos);
    out += ",\"ad\":";
    detail::appendVec2(out, snap.aimDir);
    out += ",\"to\":";
    detail::appendVec2(out, snap.tipOffset);
    out += ",\"pr\":";
    detail::appendFloat(out, snap.pullRatio);
    out += ",\"nt\":";
    const std::size_t noteLen = static_cast<std::size_t>(
        std::find(snap.statusNote.begin(), snap.statusNote.end(), '\0') - snap.statusNote.begin());
    detail::appendEscapedString(out, std::string_view(snap.statusNote.data(), noteLen));
    out += ",\"b\":[";
    for (std::size_t i = 0; i < snap.balls.size(); ++i) {
        if (i > 0) out.push_back(',');
        const BilliardsLanBall& b = snap.balls[i];
        out.push_back('[');
        detail::appendFloat(out, b.normPos.x);
        out.push_back(',');
        detail::appendFloat(out, b.normPos.y);
        out.push_back(',');
        detail::appendInt(out, b.number);
        out.push_back(',');
        detail::appendInt(out, static_cast<int>(b.group));
        out.push_back(',');
        detail::appendInt(out, b.pocketed ? 1 : 0);
        out.push_back(',');
        detail::appendInt(out, b.tube);
        out.push_back(',');
        detail::appendInt(out, b.tubeSlot);
        out.push_back(',');
        detail::appendFloat(out, b.dropProgress);
        out.push_back(']');
    }
    out += "],\"sd\":[";
    for (std::size_t i = 0; i < sounds.size(); ++i) {
        if (i > 0) out.push_back(',');
        const SoundEvent& se = sounds[i];
        out.push_back('[');
        detail::appendInt(out, static_cast<int>(se.kind));
        out.push_back(',');
        detail::appendFloat(out, se.gain);
        out.push_back(',');
        detail::appendFloat(out, se.pan);
        out.push_back(',');
        detail::appendFloat(out, se.pitch);
        out.push_back(']');
    }
    out += "]}\n";
    return out;
}

[[nodiscard]] inline std::string encodeHockeyInput(const AirHockeyRemoteInput& in) {
    std::string out;
    out.reserve(96);
    out += "{\"t\":\"hk_in\",\"pl\":";
    detail::appendVec2(out, in.normTarget);
    out += ",\"rm\":";
    detail::appendInt(out, in.requestRematch ? 1 : 0);
    out += "}\n";
    return out;
}

[[nodiscard]] inline std::string encodeHockeyState(const AirHockeyLanState& st,
                                                   std::span<const SoundEvent> sounds) {
    std::string out;
    out.reserve(256 + sounds.size() * 28);
    out += "{\"t\":\"hk_st\",\"w\":";
    detail::appendVec2(out, st.puckNorm);
    out += ",\"pv\":";
    detail::appendVec2(out, st.puckVelNorm);
    out += ",\"lm\":";
    detail::appendVec2(out, st.leftMalletNorm);
    out += ",\"rmp\":";
    detail::appendVec2(out, st.rightMalletNorm);
    out += ",\"sc\":[";
    detail::appendInt(out, st.scores[0]);
    out.push_back(',');
    detail::appendInt(out, st.scores[1]);
    out += "],\"tg\":";
    detail::appendInt(out, st.targetGoals);
    out += ",\"wn\":";
    detail::appendInt(out, st.winner);
    out += ",\"gf\":";
    detail::appendFloat(out, st.goalFlash);
    out += ",\"sd\":[";
    for (std::size_t i = 0; i < sounds.size(); ++i) {
        if (i > 0) out.push_back(',');
        const SoundEvent& se = sounds[i];
        out.push_back('[');
        detail::appendInt(out, static_cast<int>(se.kind));
        out.push_back(',');
        detail::appendFloat(out, se.gain);
        out.push_back(',');
        detail::appendFloat(out, se.pan);
        out.push_back(',');
        detail::appendFloat(out, se.pitch);
        out.push_back(']');
    }
    out += "]}\n";
    return out;
}

[[nodiscard]] inline std::string sanitizeAnonymousTag(std::string_view raw) {
    std::string clean;
    clean.reserve(20);
    for (unsigned char ch : raw) {
        if (clean.size() >= 20u) break;
        if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '-') {
            clean.push_back(static_cast<char>(ch));
        }
    }
    if (clean.empty()) clean = "DPP-Room";
    return clean;
}

[[nodiscard]] inline std::string encodeBeacon(const BeaconPacket& b) {
    std::string out;
    out.reserve(128);
    out += "{\"t\":\"bcn\",\"rid\":";
    detail::appendInt(out, static_cast<int>(b.roomId & 0x7FFFFFFFu));
    out += ",\"pt\":";
    detail::appendInt(out, static_cast<int>(b.tcpPort));
    out += ",\"gm\":";
    detail::appendInt(out, static_cast<int>(b.game));
    out += ",\"vr\":";
    detail::appendInt(out, b.variant);
    out += ",\"rw\":";
    detail::appendInt(out, b.reconnectWaiting ? 1 : 0);
    out += ",\"tg\":";
    const std::size_t tlen = static_cast<std::size_t>(
        std::find(b.roomTag.begin(), b.roomTag.end(), '\0') - b.roomTag.begin());
    detail::appendEscapedString(out, sanitizeAnonymousTag(std::string_view(b.roomTag.data(), tlen)));
    out += "}\n";
    return out;
}

[[nodiscard]] inline std::string encodeDiscoverProbe(LanGameKind preferredGame = LanGameKind::Billiards) {
    std::string out = "{\"t\":\"dsc\",\"gm\":";
    detail::appendInt(out, static_cast<int>(preferredGame));
    out += "}\n";
    return out;
}

[[nodiscard]] inline std::string encodeSessionHello(std::uint32_t sessionToken, bool resume) {
    std::string out = "{\"t\":\"hlo\",\"sid\":";
    detail::appendInt(out, static_cast<int>(sessionToken & 0x7FFFFFFFu));
    out += ",\"rs\":";
    detail::appendInt(out, resume ? 1 : 0);
    out += "}\n";
    return out;
}

[[nodiscard]] inline DecodedPacket decodeLine(std::string_view line) {
    if (line.empty() || line.size() > 32768u) return std::monostate{};
    detail::Reader rd{line.data(), line.data() + line.size()};
    if (!rd.consume('{')) return std::monostate{};

    std::string type;
    int ruleInt = 0;
    int actKind = -1;
    BilliardsRemoteAction act{};
    BilliardsLanSnapshot snap{};
    AirHockeyRemoteInput hkIn{};
    AirHockeyLanState hkSt{};
    BeaconPacket bcn{};
    DiscoverProbePacket dsc{};
    SessionHelloPacket hlo{};
    std::vector<SoundEvent> sounds;
    float pw = 0.0f;

    if (rd.consume('}')) return std::monostate{};
    for (;;) {
        std::string key;
        if (!rd.parseString(key) || !rd.consume(':')) return std::monostate{};

        if (key == "t") {
            if (!rd.parseString(type)) return std::monostate{};
        } else if (key == "r") {
            if (!rd.parseInt(ruleInt)) return std::monostate{};
        } else if (key == "k") {
            if (!rd.parseInt(actKind)) return std::monostate{};
        } else if (key == "pw") {
            if (!rd.parseFloat(pw)) return std::monostate{};
            act.power = std::clamp(pw, 0.0f, 1.0f);
        } else if (key == "st") {
            int st = 0;
            if (!rd.parseInt(st)) return std::monostate{};
            if (st >= 0 && st <= 4) snap.state = static_cast<BilliardsState>(st);
        } else if (key == "ru") {
            int ru = 0;
            if (!rd.parseInt(ru)) return std::monostate{};
            if (ru >= 0 && ru <= 2) snap.rule = static_cast<BilliardsRule>(ru);
        } else if (key == "cp") {
            int cp = 0;
            if (!rd.parseInt(cp)) return std::monostate{};
            snap.currentPlayer = (cp == 1) ? 1 : 0;
        } else if (key == "wn") {
            int wn = -1;
            if (!rd.parseInt(wn)) return std::monostate{};
            snap.winner = wn;
            hkSt.winner = wn;
        } else if (key == "sc") {
            int s0 = 0, s1 = 0;
            if (!rd.parseIntPair(s0, s1)) return std::monostate{};
            snap.scores = {s0, s1};
            hkSt.scores = {s0, s1};
        } else if (key == "rm") {
            int rm = 0;
            if (!rd.parseInt(rm)) return std::monostate{};
            hkIn.requestRematch = (rm != 0);
        } else if (key == "pv") {
            if (!rd.parseVec2(hkSt.puckVelNorm, {0.0f, 0.0f})) return std::monostate{};
        } else if (key == "lm") {
            if (!rd.parseVec2(hkSt.leftMalletNorm, {0.15f, 0.5f})) return std::monostate{};
        } else if (key == "rmp") {
            if (!rd.parseVec2(hkSt.rightMalletNorm, {0.85f, 0.5f})) return std::monostate{};
        } else if (key == "gf") {
            if (!rd.parseFloat(hkSt.goalFlash)) return std::monostate{};
            hkSt.goalFlash = std::clamp(hkSt.goalFlash, 0.0f, 3.0f);
        } else if (key == "rid") {
            int rid = 0;
            if (!rd.parseInt(rid)) return std::monostate{};
            bcn.roomId = static_cast<std::uint32_t>(std::max(0, rid));
        } else if (key == "pt") {
            int pt = 0;
            if (!rd.parseInt(pt) || pt <= 0 || pt > 65535) return std::monostate{};
            bcn.tcpPort = static_cast<std::uint16_t>(pt);
        } else if (key == "gm") {
            int gm = 0;
            if (!rd.parseInt(gm)) return std::monostate{};
            const LanGameKind gk = (gm == 1) ? LanGameKind::AirHockey : LanGameKind::Billiards;
            bcn.game = gk;
            dsc.preferredGame = gk;
        } else if (key == "vr") {
            if (!rd.parseInt(bcn.variant)) return std::monostate{};
        } else if (key == "rw") {
            int rw = 0;
            if (!rd.parseInt(rw)) return std::monostate{};
            bcn.reconnectWaiting = (rw != 0);
        } else if (key == "sid") {
            int sid = 0;
            if (!rd.parseInt(sid)) return std::monostate{};
            hlo.sessionToken = static_cast<std::uint32_t>(std::max(0, sid));
        } else if (key == "rs") {
            int rs = 0;
            if (!rd.parseInt(rs)) return std::monostate{};
            hlo.resume = (rs != 0);
        } else if (key == "tg") {
            if (type == "hk_st") {
                if (!rd.parseInt(hkSt.targetGoals)) return std::monostate{};
            } else {
                // Note: "tg" is integer targetGoals in hk_st, string roomTag in bcn!
                rd.skipWs();
                if (rd.p < rd.end && *rd.p == '"') {
                    std::string tagStr;
                    if (!rd.parseString(tagStr)) return std::monostate{};
                    const std::string clean = sanitizeAnonymousTag(tagStr);
                    std::snprintf(bcn.roomTag.data(), bcn.roomTag.size(), "%s", clean.c_str());
                } else {
                    if (!rd.parseInt(hkSt.targetGoals)) return std::monostate{};
                }
            }
        } else if (key == "pg") {
            int g0 = 0, g1 = 0;
            if (!rd.parseIntPair(g0, g1)) return std::monostate{};
            if (g0 >= 0 && g0 <= 5) snap.playerGroups[0] = static_cast<BallGroup>(g0);
            if (g1 >= 0 && g1 <= 5) snap.playerGroups[1] = static_cast<BallGroup>(g1);
        } else if (key == "sn") {
            if (!rd.parseInt(snap.snookerTarget)) return std::monostate{};
        } else if (key == "sh") {
            if (!rd.parseInt(snap.shots)) return std::monostate{};
        } else if (key == "pk") {
            if (!rd.parseInt(snap.pocketedCount)) return std::monostate{};
        } else if (key == "wp") {
            int wp = 0;
            if (!rd.parseInt(wp)) return std::monostate{};
            snap.whitePocketed = (wp != 0);
        } else if (key == "w") {
            Vec2 wv{0.5f, 0.5f};
            if (!rd.parseVec2(wv, {0.25f, 0.5f})) return std::monostate{};
            snap.whiteNormPos = wv;
            hkSt.puckNorm = wv;
        } else if (key == "pl") {
            Vec2 pl{0.25f, 0.5f};
            if (!rd.parseVec2(pl, {0.25f, 0.5f})) return std::monostate{};
            act.placeNormPos = pl;
            snap.placeNormPos = pl;
            hkIn.normTarget = pl;
        } else if (key == "ad") {
            Vec2 ad{1.0f, 0.0f};
            if (!rd.parseVec2(ad, {1.0f, 0.0f})) return std::monostate{};
            act.aimDir = ad;
            snap.aimDir = ad;
        } else if (key == "to") {
            Vec2 to{0.0f, 0.0f};
            if (!rd.parseVec2(to, {0.0f, 0.0f})) return std::monostate{};
            act.tipOffset = to;
            snap.tipOffset = to;
        } else if (key == "pr") {
            float pr = 0.0f;
            if (!rd.parseFloat(pr)) return std::monostate{};
            pr = std::clamp(pr, 0.0f, 1.0f);
            act.pullRatio = pr;
            snap.pullRatio = pr;
        } else if (key == "nt") {
            std::string nt;
            if (!rd.parseString(nt)) return std::monostate{};
            std::snprintf(snap.statusNote.data(), snap.statusNote.size(), "%s", nt.c_str());
        } else if (key == "b") {
            if (!rd.consume('[')) return std::monostate{};
            if (!rd.consume(']')) {
                for (;;) {
                    if (!rd.consume('[')) return std::monostate{};
                    float nx = 0.5f, ny = 0.5f, dp = 1.0f;
                    int num = 1, grp = 0, pock = 0, tube = 0, slot = 0;
                    if (!rd.parseFloat(nx) || !rd.consume(',') || !rd.parseFloat(ny) || !rd.consume(',') ||
                        !rd.parseInt(num) || !rd.consume(',') || !rd.parseInt(grp) || !rd.consume(',') ||
                        !rd.parseInt(pock) || !rd.consume(',') || !rd.parseInt(tube) || !rd.consume(',') ||
                        !rd.parseInt(slot) || !rd.consume(',') || !rd.parseFloat(dp)) {
                        return std::monostate{};
                    }
                    while (rd.consume(',')) {
                        if (!rd.skipValue()) return std::monostate{};
                    }
                    if (!rd.consume(']')) return std::monostate{};
                    if (snap.balls.size() < 91u) {
                        BilliardsLanBall lb;
                        lb.normPos =
                            (std::isfinite(nx) && std::isfinite(ny)) ? Vec2{nx, ny} : Vec2{0.5f, 0.5f};
                        lb.number = num;
                        if (grp >= 0 && grp <= 5) lb.group = static_cast<BallGroup>(grp);
                        lb.pocketed = (pock != 0);
                        lb.tube = (tube == 1) ? 1 : 0;
                        lb.tubeSlot = slot;
                        lb.dropProgress = std::clamp(dp, 0.0f, 1.0f);
                        snap.balls.push_back(lb);
                    }
                    if (rd.consume(',')) continue;
                    if (!rd.consume(']')) return std::monostate{};
                    break;
                }
            }
        } else if (key == "sd") {
            if (!rd.consume('[')) return std::monostate{};
            if (!rd.consume(']')) {
                for (;;) {
                    if (!rd.consume('[')) return std::monostate{};
                    int sk = 0;
                    float g = 0.5f, pan = 0.0f, pitch = 1.0f;
                    if (!rd.parseInt(sk) || !rd.consume(',') || !rd.parseFloat(g) || !rd.consume(',') ||
                        !rd.parseFloat(pan) || !rd.consume(',') || !rd.parseFloat(pitch)) {
                        return std::monostate{};
                    }
                    while (rd.consume(',')) {
                        if (!rd.skipValue()) return std::monostate{};
                    }
                    if (!rd.consume(']')) return std::monostate{};
                    if (sk >= 0 && sk <= static_cast<int>(SoundKind::BlackHoleSwallow) &&
                        sounds.size() < 16u) {
                        SoundEvent ev;
                        ev.kind = static_cast<SoundKind>(sk);
                        ev.gain = std::clamp(g, 0.05f, 1.0f);
                        ev.pan = std::clamp(pan, -0.95f, 0.95f);
                        ev.pitch = std::clamp(pitch, 0.78f, 1.32f);
                        sounds.push_back(ev);
                    }
                    if (rd.consume(',')) continue;
                    if (!rd.consume(']')) return std::monostate{};
                    break;
                }
            }
        } else {
            if (!rd.skipValue()) return std::monostate{};
        }

        if (rd.consume(',')) continue;
        if (!rd.consume('}')) return std::monostate{};
        break;
    }

    if (type == "rule") {
        if (ruleInt < 0 || ruleInt > 2) return std::monostate{};
        return RulePacket{static_cast<BilliardsRule>(ruleInt)};
    }
    if (type == "act") {
        if (actKind < 0 || actKind > 2) return std::monostate{};
        act.kind = static_cast<BilliardsRemoteAction::Kind>(actKind);
        return ActionPacket{act};
    }
    if (type == "snap") {
        return SnapshotPacket{std::move(snap), std::move(sounds)};
    }
    if (type == "hk_in") {
        return HockeyInputPacket{hkIn};
    }
    if (type == "hk_st") {
        return HockeyStatePacket{hkSt, std::move(sounds)};
    }
    if (type == "bcn") {
        if (bcn.tcpPort == 0) return std::monostate{};
        if (bcn.roomTag[0] == '\0') std::snprintf(bcn.roomTag.data(), bcn.roomTag.size(), "DPP-Room");
        return bcn;
    }
    if (type == "dsc") {
        return dsc;
    }
    if (type == "hlo") {
        return hlo;
    }
    return std::monostate{};
}

}  // namespace dpp::lan_codec
