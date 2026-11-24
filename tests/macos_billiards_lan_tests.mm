#import <CoreFoundation/CoreFoundation.h>
#import <Foundation/Foundation.h>
#include <gtest/gtest.h>
#include <sys/socket.h>

#include "core/scene/playground.h"
#include "platforms/macos/billiards_lan.h"

namespace {

void pumpMainRunLoopUntil(const std::function<bool()>& pred, double timeoutSec = 2.0) {
    const CFAbsoluteTime deadline = CFAbsoluteTimeGetCurrent() + timeoutSec;
    while (!pred() && CFAbsoluteTimeGetCurrent() < deadline) {
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.005, true);
    }
}

TEST(MacBilliardsLan, EndpointParsingAndBidirectionalStreamSync) {
    std::string ip;
    std::uint16_t port = 0;
    EXPECT_TRUE(dpp::mac::BilliardsLan::parseEndpoint("", ip, port));
    EXPECT_EQ(ip, "127.0.0.1");
    EXPECT_EQ(port, dpp::mac::BilliardsLan::kDefaultPort);

    EXPECT_TRUE(dpp::mac::BilliardsLan::parseEndpoint("192.168.1.88:40123", ip, port));
    EXPECT_EQ(ip, "192.168.1.88");
    EXPECT_EQ(port, 40123u);

    EXPECT_FALSE(dpp::mac::BilliardsLan::parseEndpoint("not_an_ip:39457", ip, port));
    EXPECT_FALSE(dpp::mac::BilliardsLan::parseEndpoint("127.0.0.1:99999", ip, port));

    dpp::mac::BilliardsLan hostLan;
    dpp::mac::BilliardsLan guestLan;

    bool connectedViaTcp = false;
    if (hostLan.host(/*port=*/0, /*loopbackOnly=*/true) && hostLan.boundPort() > 0) {
        const std::string ep = "127.0.0.1:" + std::to_string(hostLan.boundPort());
        if (guestLan.join(ep)) {
            pumpMainRunLoopUntil([&]() { return hostLan.connected() && guestLan.connected(); }, 1.0);
            connectedViaTcp = hostLan.connected() && guestLan.connected();
        } else {
            EXPECT_TRUE(guestLan.lastErrno() == EPERM || guestLan.lastErrno() == EACCES);
        }
    }
    if (!connectedViaTcp) {
        hostLan.stop();
        guestLan.stop();
        int pair[2] = {-1, -1};
        ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, pair), 0);
        ASSERT_TRUE(hostLan.attachConnectedSocket(pair[0], /*asHost=*/true));
        ASSERT_TRUE(guestLan.attachConnectedSocket(pair[1], /*asHost=*/false));
    }

    ASSERT_EQ(hostLan.state(), dpp::mac::BilliardsLan::State::ConnectedHost);
    ASSERT_EQ(guestLan.state(), dpp::mac::BilliardsLan::State::ConnectedGuest);

    bool ruleSynced = false;
    dpp::BilliardsRule receivedRule = dpp::BilliardsRule::Free;
    bool snapReceived = false;
    dpp::BilliardsLanSnapshot lastSnap{};
    std::vector<dpp::SoundEvent> lastSounds;
    bool actionReceived = false;
    dpp::BilliardsRemoteAction lastAction{};

    guestLan.onRuleSync = [&](dpp::BilliardsRule r) {
        ruleSynced = true;
        receivedRule = r;
    };
    guestLan.onSnapshot = [&](const dpp::BilliardsLanSnapshot& s, const std::vector<dpp::SoundEvent>& snd) {
        snapReceived = true;
        lastSnap = s;
        lastSounds = snd;
    };
    hostLan.onAction = [&](const dpp::BilliardsRemoteAction& a) {
        actionReceived = true;
        lastAction = a;
    };

    // 1. Host syncs Snooker rule + snapshot + sound event to Guest.
    dpp::BilliardsConfig cfg = dpp::BilliardsConfig::desktop();
    cfg.rule = dpp::BilliardsRule::Snooker;
    cfg.matchMode = dpp::BilliardsMatchMode::Lan;
    dpp::BilliardsMode hostMode(dpp::syntheticScene(12, {1440, 900}), cfg);
    std::vector<dpp::SoundEvent> outSounds = {{dpp::SoundKind::CueStrike, 0.75f, -0.25f, 1.0f}};
    EXPECT_TRUE(hostLan.sendRuleSync(dpp::BilliardsRule::Snooker));
    EXPECT_TRUE(hostLan.sendSnapshot(hostMode.exportLanSnapshot(), outSounds));

    pumpMainRunLoopUntil([&]() { return ruleSynced && snapReceived; });
    EXPECT_TRUE(ruleSynced);
    EXPECT_EQ(receivedRule, dpp::BilliardsRule::Snooker);
    EXPECT_TRUE(snapReceived);
    EXPECT_EQ(lastSnap.balls.size(), 21u);
    ASSERT_EQ(lastSounds.size(), 1u);
    EXPECT_EQ(lastSounds[0].kind, dpp::SoundKind::CueStrike);
    EXPECT_NEAR(lastSounds[0].gain, 0.75f, 1e-3f);

    // 2. Guest sends Shoot action back to Host.
    dpp::BilliardsRemoteAction shoot;
    shoot.kind = dpp::BilliardsRemoteAction::Kind::Shoot;
    shoot.aimDir = {0.8f, -0.6f};
    shoot.power = 0.65f;
    shoot.tipOffset = {0.2f, -0.5f};
    EXPECT_TRUE(guestLan.sendAction(shoot));

    pumpMainRunLoopUntil([&]() { return actionReceived; });
    ASSERT_TRUE(actionReceived);
    EXPECT_EQ(lastAction.kind, dpp::BilliardsRemoteAction::Kind::Shoot);
    EXPECT_NEAR(lastAction.power, 0.65f, 1e-3f);
    EXPECT_NEAR(lastAction.tipOffset.y, -0.5f, 1e-3f);

    guestLan.stop();
    pumpMainRunLoopUntil([&]() { return !hostLan.connected(); });
    EXPECT_FALSE(hostLan.connected());
    hostLan.stop();
    EXPECT_EQ(hostLan.state(), dpp::mac::BilliardsLan::State::Disconnected);
}

TEST(MacBilliardsLan, CodecDiscoveryPacketsZeroPiiAndLoopbackAutoDiscoveryAndReconnect) {
    using namespace dpp::lan_codec;
    // 1. Codec round-trips for Beacon ('bcn'), DiscoverProbe ('dsc'), and SessionHello ('hlo')
    //    with strict zero-PII tag sanitization.
    BeaconPacket bcn{};
    bcn.tcpPort = 39457;
    bcn.game = LanGameKind::Billiards;
    bcn.variant = static_cast<int>(dpp::BilliardsRule::EightBall);
    bcn.reconnectWaiting = true;
    bcn.roomId = 0x19887766u;
    const std::string cleanTag = sanitizeAnonymousTag("/Users/secret/Room#A42.txt");
    EXPECT_EQ(cleanTag, "UserssecretRoomA42tx");
    std::snprintf(bcn.roomTag.data(), bcn.roomTag.size(), "%s", cleanTag.c_str());

    const std::string bcnLine = encodeBeacon(bcn);
    EXPECT_EQ(bcnLine.find("/Users"), std::string::npos);
    const DecodedPacket decBcn = decodeLine(std::string_view(bcnLine.data(), bcnLine.size() - 1));
    const auto* gotBcn = std::get_if<BeaconPacket>(&decBcn);
    ASSERT_NE(gotBcn, nullptr);
    EXPECT_EQ(gotBcn->tcpPort, 39457u);
    EXPECT_EQ(gotBcn->game, LanGameKind::Billiards);
    EXPECT_EQ(gotBcn->variant, static_cast<int>(dpp::BilliardsRule::EightBall));
    EXPECT_TRUE(gotBcn->reconnectWaiting);
    EXPECT_EQ(gotBcn->roomId, 0x19887766u);
    EXPECT_STREQ(gotBcn->roomTag.data(), "UserssecretRoomA42tx");

    const std::string dscLine = encodeDiscoverProbe(LanGameKind::AirHockey);
    const DecodedPacket decDsc = decodeLine(std::string_view(dscLine.data(), dscLine.size() - 1));
    const auto* gotDsc = std::get_if<DiscoverProbePacket>(&decDsc);
    ASSERT_NE(gotDsc, nullptr);
    EXPECT_EQ(gotDsc->preferredGame, LanGameKind::AirHockey);

    const std::string hloLine = encodeSessionHello(0x3AFEBABEu, /*resume=*/true);
    const DecodedPacket decHlo = decodeLine(std::string_view(hloLine.data(), hloLine.size() - 1));
    const auto* gotHlo = std::get_if<SessionHelloPacket>(&decHlo);
    ASSERT_NE(gotHlo, nullptr);
    EXPECT_EQ(gotHlo->sessionToken, 0x3AFEBABEu);
    EXPECT_TRUE(gotHlo->resume);

    DiscoveredLanRoom room{};
    room.hostIp = "192.168.1.25";
    room.tcpPort = 39457;
    room.game = LanGameKind::Billiards;
    room.variant = 1;
    room.roomTag = sanitizeAnonymousTag("DPP-88A1");
    EXPECT_EQ(room.endpoint(), "192.168.1.25:39457");
    EXPECT_NE(room.displayLabel().find("黑八"), std::string::npos);

    // 2. Live loopback UDP discovery + quickScanAndJoin + TCP disconnect & automatic Guest reconnect
    //    (gracefully skipping AF_INET datagram/connect when Seatbelt sandbox blocks outbound IP).
    int probeFd = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    bool loopbackOutboundAllowed = false;
    if (probeFd >= 0) {
        struct sockaddr_in lo{};
        lo.sin_family = AF_INET;
        lo.sin_port = htons(9);
        lo.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        const char byte = 'x';
        loopbackOutboundAllowed =
            (::sendto(probeFd, &byte, 1, 0, reinterpret_cast<const struct sockaddr*>(&lo), sizeof(lo)) >= 0);
        ::close(probeFd);
    }

    dpp::mac::BilliardsLan hostLan;
    dpp::mac::BilliardsLan guestLan;
    guestLan.setReconnectDelayMs(50);
    hostLan.setAdvertisedGame(LanGameKind::Billiards, static_cast<int>(dpp::BilliardsRule::Snooker));

    if (loopbackOutboundAllowed && hostLan.host(/*port=*/0, /*loopbackOnly=*/true, /*discoveryPort=*/0) &&
        hostLan.boundPort() > 0 && hostLan.boundDiscoveryPort() > 0) {
        const std::uint16_t discPort = hostLan.boundDiscoveryPort();
        ASSERT_TRUE(guestLan.quickScanAndJoin(LanGameKind::Billiards, discPort, /*loopbackOnly=*/true));
        pumpMainRunLoopUntil(
            [&]() { return hostLan.connected() && guestLan.connected() && guestLan.sessionToken() != 0u; },
            1.5);
        ASSERT_TRUE(hostLan.connected() && guestLan.connected());
        EXPECT_FALSE(guestLan.discoveredRooms().empty());

        const std::uint32_t tokenBefore = guestLan.sessionToken();
        EXPECT_NE(tokenBefore, 0u);
        EXPECT_EQ(hostLan.sessionToken(), tokenBefore);

        bool resyncedRule = false;
        hostLan.onStatus = [&](dpp::mac::BilliardsLan::State st, const std::string&) {
            if (st == dpp::mac::BilliardsLan::State::ConnectedHost) {
                hostLan.sendRuleSync(dpp::BilliardsRule::Snooker);
            }
        };
        guestLan.onRuleSync = [&](dpp::BilliardsRule r) {
            if (r == dpp::BilliardsRule::Snooker) resyncedRule = true;
        };

        const std::uint16_t samePort = hostLan.boundPort();
        hostLan.stop();
        pumpMainRunLoopUntil([&]() { return guestLan.reconnecting(); }, 0.6);
        EXPECT_TRUE(guestLan.reconnecting());

        ASSERT_TRUE(hostLan.host(samePort, /*loopbackOnly=*/true, /*discoveryPort=*/0));
        pumpMainRunLoopUntil([&]() { return hostLan.connected() && guestLan.connected() && resyncedRule; },
                             2.0);
        EXPECT_TRUE(hostLan.connected());
        EXPECT_TRUE(guestLan.connected());
        EXPECT_FALSE(guestLan.reconnecting());
        EXPECT_TRUE(resyncedRule);
    } else {
        // Seatbelt AF_INET restriction: verify live HockeyState + SessionHello over AF_UNIX socketpair!
        int pair[2] = {-1, -1};
        ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, pair), 0);
        ASSERT_TRUE(hostLan.attachConnectedSocket(pair[0], /*asHost=*/true));
        ASSERT_TRUE(guestLan.attachConnectedSocket(pair[1], /*asHost=*/false));
        bool gotHockey = false;
        guestLan.onHockeyState = [&](const dpp::AirHockeyLanState& st, const std::vector<dpp::SoundEvent>&) {
            if (st.scores[0] == 5 && st.targetGoals == 7) gotHockey = true;
        };
        dpp::AirHockeyLanState hst{};
        hst.scores = {5, 3};
        hst.targetGoals = 7;
        EXPECT_TRUE(hostLan.sendHockeyState(hst, {}));
        pumpMainRunLoopUntil([&]() { return gotHockey; }, 1.0);
        EXPECT_TRUE(gotHockey);
    }
    guestLan.stop();
    hostLan.stop();
}

}  // namespace
