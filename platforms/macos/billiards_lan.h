#pragma once
// Event-driven UI-thread LAN TCP transport for 2-player Desktop Billiards.
// Transmits ONLY normalized [0,1] table coordinates, ball numbers/groups, turn/score state,
// and procedural sound triggers — never transmits filenames, paths, icons, or display IDs.

#import <Foundation/Foundation.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

#include "core/modes/air_hockey_mode.h"
#include "core/modes/billiards_lan_codec.h"
#include "core/modes/billiards_mode.h"

namespace dpp::mac {

class BilliardsLan {
   public:
    enum class State : std::uint8_t {
        Disconnected = 0,
        HostingWaiting = 1,
        Connecting = 2,
        ConnectedHost = 3,
        ConnectedGuest = 4,
    };

    static constexpr std::uint16_t kDefaultPort = 39457;
    static constexpr std::uint16_t kDefaultDiscoveryPort = lan_codec::kDefaultDiscoveryPort;

    std::function<void(State, const std::string&)> onStatus;
    std::function<void(const BilliardsLanSnapshot&, const std::vector<SoundEvent>&)> onSnapshot;
    std::function<void(const BilliardsRemoteAction&)> onAction;
    std::function<void(BilliardsRule)> onRuleSync;
    std::function<void(const AirHockeyRemoteInput&)> onHockeyInput;
    std::function<void(const AirHockeyLanState&, const std::vector<SoundEvent>&)> onHockeyState;
    std::function<void(const std::vector<lan_codec::DiscoveredLanRoom>&)> onRoomsUpdated;

    BilliardsLan() {
        roomId_ = static_cast<std::uint32_t>(arc4random_uniform(0x7FFFFF00u) + 100u);
        std::snprintf(roomTag_.data(), roomTag_.size(), "Mac-%04X", static_cast<unsigned>(roomId_ & 0xFFFFu));
    }
    ~BilliardsLan() {
        stopDiscovery();
        stop();
    }
    BilliardsLan(const BilliardsLan&) = delete;
    BilliardsLan& operator=(const BilliardsLan&) = delete;

    [[nodiscard]] State state() const { return state_; }
    [[nodiscard]] bool connected() const {
        return state_ == State::ConnectedHost || state_ == State::ConnectedGuest;
    }
    [[nodiscard]] bool isHost() const {
        return state_ == State::HostingWaiting || state_ == State::ConnectedHost;
    }
    [[nodiscard]] bool isGuest() const {
        return state_ == State::Connecting || state_ == State::ConnectedGuest;
    }
    [[nodiscard]] std::uint16_t boundPort() const { return boundPort_; }
    [[nodiscard]] std::uint16_t boundDiscoveryPort() const { return boundDiscoveryPort_; }
    [[nodiscard]] int lastErrno() const { return lastErrno_; }
    [[nodiscard]] const std::string& statusText() const { return statusText_; }
    [[nodiscard]] bool reconnecting() const { return reconnectActive_; }
    [[nodiscard]] int reconnectAttempt() const { return reconnectAttempts_; }
    [[nodiscard]] std::uint32_t sessionToken() const { return sessionToken_; }
    void setAutoReconnect(bool enabled) { autoReconnect_ = enabled; }
    void setReconnectDelayMs(unsigned delayMs) { reconnectDelayMs_ = std::clamp(delayMs, 40u, 5000u); }
    void setAdvertisedGame(lan_codec::LanGameKind game, int variant) {
        advertisedGame_ = game;
        advertisedVariant_ = variant;
        if (state_ == State::HostingWaiting) broadcastHostBeacon();
    }
    [[nodiscard]] const std::vector<lan_codec::DiscoveredLanRoom>& discoveredRooms() const {
        return discoveredRooms_;
    }

    // Attaches an already-connected stream descriptor (e.g. AF_UNIX socketpair in sandboxed unit tests)
    // to exercise the exact non-blocking GCD framing, JSON codec, and disconnect state machine.
    bool attachConnectedSocket(int fd, bool asHost) {
        stop();
        if (fd < 0) return false;
        configureSocketOptions(fd);
        peerFd_ = fd;
        recvBuf_.clear();
        sendBuf_.clear();
        attachPeerReadSource(fd, asHost);
        updateState(asHost ? State::ConnectedHost : State::ConnectedGuest,
                    asHost ? "对手已连接 (测试通道)" : "已连房主 (测试通道)");
        return true;
    }

    [[nodiscard]] static std::string primaryLanIPv4() {
        struct ifaddrs* ifaddr = nullptr;
        if (getifaddrs(&ifaddr) != 0 || !ifaddr) return "127.0.0.1";
        std::string best;
        std::string fallback;
        for (struct ifaddrs* ifa = ifaddr; ifa != nullptr; ifa = ifa->ifa_next) {
            if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET) continue;
            if ((ifa->ifa_flags & IFF_UP) == 0 || (ifa->ifa_flags & IFF_LOOPBACK) != 0) continue;
            char host[INET_ADDRSTRLEN]{};
            const auto* sa = reinterpret_cast<const struct sockaddr_in*>(ifa->ifa_addr);
            if (!inet_ntop(AF_INET, &sa->sin_addr, host, sizeof(host))) continue;
            if (std::strncmp(host, "169.254.", 8) == 0 || std::strncmp(host, "198.18.", 7) == 0 ||
                std::strncmp(host, "198.19.", 7) == 0)
                continue;
            if (ifa->ifa_name && std::strncmp(ifa->ifa_name, "utun", 4) == 0) continue;
            if (ifa->ifa_name &&
                (std::strcmp(ifa->ifa_name, "en0") == 0 || std::strcmp(ifa->ifa_name, "en1") == 0)) {
                best = host;
                break;
            }
            if (fallback.empty()) fallback = host;
        }
        freeifaddrs(ifaddr);
        if (!best.empty()) return best;
        if (!fallback.empty()) return fallback;
        return "127.0.0.1";
    }

    static bool parseEndpoint(const std::string& raw, std::string& outIp, std::uint16_t& outPort,
                              std::uint16_t defaultPort = kDefaultPort) {
        std::string s = raw;
        while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.erase(s.begin());
        while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.pop_back();
        if (s.empty()) {
            outIp = "127.0.0.1";
            outPort = defaultPort;
            return true;
        }
        const auto colon = s.rfind(':');
        if (colon != std::string::npos) {
            outIp = s.substr(0, colon);
            const std::string portStr = s.substr(colon + 1);
            if (outIp.empty()) outIp = "127.0.0.1";
            char* end = nullptr;
            const long p = std::strtol(portStr.c_str(), &end, 10);
            if (!end || *end != '\0' || p <= 0 || p > 65535) return false;
            outPort = static_cast<std::uint16_t>(p);
        } else {
            outIp = s;
            outPort = defaultPort;
        }
        struct in_addr addr{};
        return inet_pton(AF_INET, outIp.c_str(), &addr) == 1;
    }

    bool host(std::uint16_t port = kDefaultPort, bool loopbackOnly = false,
              std::uint16_t discoveryPort = kDefaultDiscoveryPort) {
        stop();
        hostLoopbackOnly_ = loopbackOnly;
        hasHadPeer_ = false;
        lastPeerIp_.clear();
        sessionToken_ = static_cast<std::uint32_t>(arc4random_uniform(0x7FFFFF00u) + 1u);
        const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) {
            updateState(State::Disconnected, "创建监听套接字失败");
            return false;
        }
        configureSocketOptions(fd);

        struct sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(loopbackOnly ? INADDR_LOOPBACK : INADDR_ANY);
        addr.sin_port = htons(port);
        if (::bind(fd, reinterpret_cast<const struct sockaddr*>(&addr), sizeof(addr)) != 0 ||
            ::listen(fd, 1) != 0) {
            ::close(fd);
            updateState(State::Disconnected, "端口已被占用或无法监听");
            return false;
        }

        struct sockaddr_in bound{};
        socklen_t blen = sizeof(bound);
        if (::getsockname(fd, reinterpret_cast<struct sockaddr*>(&bound), &blen) == 0) {
            boundPort_ = ntohs(bound.sin_port);
        } else {
            boundPort_ = port;
        }

        listenFd_ = fd;
        listenSource_ = dispatch_source_create(DISPATCH_SOURCE_TYPE_READ, static_cast<uintptr_t>(fd), 0,
                                               dispatch_get_main_queue());
        if (!listenSource_) {
            ::close(fd);
            listenFd_ = -1;
            updateState(State::Disconnected, "无法创建监听事件源");
            return false;
        }
        dispatch_source_set_event_handler(listenSource_, ^{
          this->handleAccept();
        });
        dispatch_source_set_cancel_handler(listenSource_, ^{
          ::close(fd);
        });
        dispatch_resume(listenSource_);

        startHostBeaconSocket(discoveryPort, loopbackOnly);

        const std::string ip = loopbackOnly ? "127.0.0.1" : primaryLanIPv4();
        char msg[128];
        std::snprintf(msg, sizeof(msg), "等待加入 (%s:%u · 已开启广播)", ip.c_str(),
                      static_cast<unsigned>(boundPort_));
        updateState(State::HostingWaiting, msg);
        return true;
    }

    bool startDiscovery(std::uint16_t discoveryPort = kDefaultDiscoveryPort, bool loopbackOnly = false) {
        stopDiscovery();
        scanLoopbackOnly_ = loopbackOnly;
        scanTargetPort_ = discoveryPort ? discoveryPort : kDefaultDiscoveryPort;
        const int ufd = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (ufd < 0) return false;
        configureUdpSocketOptions(ufd);

        struct sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(loopbackOnly ? INADDR_LOOPBACK : INADDR_ANY);
        addr.sin_port = htons(loopbackOnly ? 0 : discoveryPort);
        if (::bind(ufd, reinterpret_cast<const struct sockaddr*>(&addr), sizeof(addr)) != 0) {
            // Fall back to ephemeral port so active probe-reply unicast still works even if port busy.
            addr.sin_port = htons(0);
            if (::bind(ufd, reinterpret_cast<const struct sockaddr*>(&addr), sizeof(addr)) != 0) {
                ::close(ufd);
                return false;
            }
        }
        struct sockaddr_in bound{};
        socklen_t blen = sizeof(bound);
        if (::getsockname(ufd, reinterpret_cast<struct sockaddr*>(&bound), &blen) == 0) {
            boundDiscoveryPort_ = ntohs(bound.sin_port);
        }

        scanUdpFd_ = ufd;
        scanReadSource_ = dispatch_source_create(DISPATCH_SOURCE_TYPE_READ, static_cast<uintptr_t>(ufd), 0,
                                                 dispatch_get_main_queue());
        if (!scanReadSource_) {
            ::close(ufd);
            scanUdpFd_ = -1;
            return false;
        }
        dispatch_source_set_event_handler(scanReadSource_, ^{
          this->handleDiscoveryRead();
        });
        dispatch_source_set_cancel_handler(scanReadSource_, ^{
          ::close(ufd);
        });
        dispatch_resume(scanReadSource_);

        sendDiscoveryProbe(advertisedGame_);
        return true;
    }

    void stopDiscovery() {
        quickJoinPending_ = false;
        if (quickJoinTimer_) {
            dispatch_source_cancel(quickJoinTimer_);
            quickJoinTimer_ = nil;
        }
        if (scanReadSource_) {
            dispatch_source_cancel(scanReadSource_);
            scanReadSource_ = nil;
            scanUdpFd_ = -1;
        } else if (scanUdpFd_ >= 0) {
            ::close(scanUdpFd_);
            scanUdpFd_ = -1;
        }
        boundDiscoveryPort_ = 0;
    }

    void sendDiscoveryProbe(lan_codec::LanGameKind preferred = lan_codec::LanGameKind::Billiards) {
        if (scanUdpFd_ < 0) return;
        const std::string pkt = lan_codec::encodeDiscoverProbe(preferred);
        sendUdpBroadcastOrLoopback(scanUdpFd_, pkt, scanTargetPort_, scanLoopbackOnly_);
    }

    bool quickScanAndJoin(lan_codec::LanGameKind preferredGame,
                          std::uint16_t discoveryPort = kDefaultDiscoveryPort, bool loopbackOnly = false) {
        pruneStaleRooms();
        for (const auto& rm : discoveredRooms_) {
            if (rm.game == preferredGame) {
                return join(rm.endpoint(), rm.tcpPort);
            }
        }
        if (!discoveredRooms_.empty()) {
            return join(discoveredRooms_.front().endpoint(), discoveredRooms_.front().tcpPort);
        }
        if (scanUdpFd_ < 0 || scanLoopbackOnly_ != loopbackOnly ||
            (discoveryPort != 0 && scanTargetPort_ != discoveryPort)) {
            if (!startDiscovery(discoveryPort, loopbackOnly)) {
                updateState(State::Disconnected, "无法启动局域网房间扫描");
                return false;
            }
        } else {
            sendDiscoveryProbe(preferredGame);
        }
        quickJoinPending_ = true;
        quickJoinGame_ = preferredGame;
        updateState(State::Disconnected, "正在扫描局域网房间…");
        if (quickJoinTimer_) {
            dispatch_source_cancel(quickJoinTimer_);
            quickJoinTimer_ = nil;
        }
        quickJoinTimer_ = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, dispatch_get_main_queue());
        if (quickJoinTimer_) {
            dispatch_source_set_timer(quickJoinTimer_, dispatch_time(DISPATCH_TIME_NOW, 2500 * NSEC_PER_MSEC),
                                      DISPATCH_TIME_FOREVER, 80 * NSEC_PER_MSEC);
            dispatch_source_set_event_handler(quickJoinTimer_, ^{
              if (this->quickJoinPending_) {
                  this->quickJoinPending_ = false;
                  if (this->state_ == State::Disconnected && !this->reconnectActive_) {
                      this->updateState(State::Disconnected, "未发现局域网房间 · 请先由房主创建或手动填IP");
                  }
              }
            });
            dispatch_resume(quickJoinTimer_);
        }
        return true;
    }

    bool join(const std::string& endpoint, std::uint16_t defaultPort = kDefaultPort) {
        quickJoinPending_ = false;
        stop();
        std::string ip;
        std::uint16_t port = defaultPort;
        if (!parseEndpoint(endpoint, ip, port, defaultPort)) {
            updateState(State::Disconnected, "IP:端口格式无效");
            return false;
        }
        lastHostIp_ = ip;
        lastHostPort_ = port;
        everConnectedGuest_ = false;
        reconnectAttempts_ = 0;
        reconnectActive_ = false;
        return beginConnectAttempt(ip, port, false);
    }

    void stop() {
        quickJoinPending_ = false;
        const bool wasActive = (state_ != State::Disconnected || reconnectActive_);
        cancelReconnectTimer();
        everConnectedGuest_ = false;
        reconnectAttempts_ = 0;
        reconnectActive_ = false;
        stopHostBeaconSocket();
        stopInternal(true);
        if (wasActive) {
            updateState(State::Disconnected, "局域网未连接");
        }
    }

    bool sendRuleSync(BilliardsRule rule) {
        if (state_ != State::ConnectedHost) return false;
        NSDictionary* msg = @{
            @"t" : @"rule",
            @"r" : @(static_cast<int>(rule)),
        };
        return sendDictionary(msg);
    }

    bool sendSnapshot(const BilliardsLanSnapshot& snap, const std::vector<SoundEvent>& sounds) {
        if (state_ != State::ConnectedHost) return false;
        auto num = [](float v) -> NSNumber* { return @(std::isfinite(v) ? v : 0.0f); };
        NSMutableArray* ballsArr = [NSMutableArray arrayWithCapacity:snap.balls.size()];
        for (const BilliardsLanBall& b : snap.balls) {
            [ballsArr addObject:@[
                num(b.normPos.x),
                num(b.normPos.y),
                @(b.number),
                @(static_cast<int>(b.group)),
                @(b.pocketed ? 1 : 0),
                @(b.tube),
                @(b.tubeSlot),
                num(b.dropProgress),
            ]];
        }
        NSMutableArray* sndArr = [NSMutableArray arrayWithCapacity:sounds.size()];
        for (const SoundEvent& se : sounds) {
            [sndArr addObject:@[
                @(static_cast<int>(se.kind)),
                num(se.gain),
                num(se.pan),
                num(se.pitch),
            ]];
        }
        NSString* noteStr = [NSString stringWithUTF8String:snap.statusNote.data()];
        if (!noteStr) noteStr = @"";
        NSDictionary* msg = @{
            @"t" : @"snap",
            @"st" : @(static_cast<int>(snap.state)),
            @"ru" : @(static_cast<int>(snap.rule)),
            @"cp" : @(snap.currentPlayer),
            @"wn" : @(snap.winner),
            @"sc" : @[ @(snap.scores[0]), @(snap.scores[1]) ],
            @"pg" : @[ @(static_cast<int>(snap.playerGroups[0])), @(static_cast<int>(snap.playerGroups[1])) ],
            @"sn" : @(snap.snookerTarget),
            @"sh" : @(snap.shots),
            @"pk" : @(snap.pocketedCount),
            @"wp" : @(snap.whitePocketed ? 1 : 0),
            @"w" : @[ num(snap.whiteNormPos.x), num(snap.whiteNormPos.y) ],
            @"pl" : @[ num(snap.placeNormPos.x), num(snap.placeNormPos.y) ],
            @"ad" : @[ num(snap.aimDir.x), num(snap.aimDir.y) ],
            @"to" : @[ num(snap.tipOffset.x), num(snap.tipOffset.y) ],
            @"pr" : num(snap.pullRatio),
            @"nt" : noteStr,
            @"b" : ballsArr,
            @"sd" : sndArr,
        };
        return sendDictionary(msg);
    }

    bool sendAction(const BilliardsRemoteAction& act) {
        if (state_ != State::ConnectedGuest) return false;
        auto num = [](float v) -> NSNumber* { return @(std::isfinite(v) ? v : 0.0f); };
        NSDictionary* msg = @{
            @"t" : @"act",
            @"k" : @(static_cast<int>(act.kind)),
            @"ad" : @[ num(act.aimDir.x), num(act.aimDir.y) ],
            @"pw" : num(act.power),
            @"to" : @[ num(act.tipOffset.x), num(act.tipOffset.y) ],
            @"pl" : @[ num(act.placeNormPos.x), num(act.placeNormPos.y) ],
            @"pr" : num(act.pullRatio),
        };
        return sendDictionary(msg);
    }

    bool sendHockeyInput(const AirHockeyRemoteInput& in) {
        if (state_ != State::ConnectedGuest) return false;
        return sendRawLine(lan_codec::encodeHockeyInput(in));
    }

    bool sendHockeyState(const AirHockeyLanState& st, std::span<const SoundEvent> sounds) {
        if (state_ != State::ConnectedHost) return false;
        return sendRawLine(lan_codec::encodeHockeyState(st, sounds));
    }

   private:
    static void configureUdpSocketOptions(int fd) {
        const int flags = ::fcntl(fd, F_GETFL, 0);
        if (flags >= 0) ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
        const int fdFlags = ::fcntl(fd, F_GETFD, 0);
        if (fdFlags >= 0) ::fcntl(fd, F_SETFD, fdFlags | FD_CLOEXEC);
        int one = 1;
        ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
#ifdef SO_NOSIGPIPE
        ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
        ::setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &one, sizeof(one));
    }

    static double steadyNowSec() { return [NSProcessInfo processInfo].systemUptime; }

    static void sendUdpBroadcastOrLoopback(int fd, const std::string& payload, std::uint16_t port,
                                           bool loopbackOnly) {
        if (fd < 0 || port == 0 || payload.empty()) return;
        struct sockaddr_in lo{};
        lo.sin_family = AF_INET;
        lo.sin_port = htons(port);
        lo.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        (void)::sendto(fd, payload.data(), payload.size(), 0, reinterpret_cast<const struct sockaddr*>(&lo),
                       sizeof(lo));
        if (loopbackOnly) return;

        struct sockaddr_in bcast{};
        bcast.sin_family = AF_INET;
        bcast.sin_port = htons(port);
        bcast.sin_addr.s_addr = htonl(INADDR_BROADCAST);
        (void)::sendto(fd, payload.data(), payload.size(), 0,
                       reinterpret_cast<const struct sockaddr*>(&bcast), sizeof(bcast));

        struct ifaddrs* ifaddr = nullptr;
        if (getifaddrs(&ifaddr) == 0 && ifaddr) {
            for (struct ifaddrs* ifa = ifaddr; ifa != nullptr; ifa = ifa->ifa_next) {
                if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET || !ifa->ifa_broadaddr) continue;
                if ((ifa->ifa_flags & IFF_UP) == 0 || (ifa->ifa_flags & IFF_BROADCAST) == 0) continue;
                struct sockaddr_in sb = *reinterpret_cast<const struct sockaddr_in*>(ifa->ifa_broadaddr);
                sb.sin_port = htons(port);
                (void)::sendto(fd, payload.data(), payload.size(), 0,
                               reinterpret_cast<const struct sockaddr*>(&sb), sizeof(sb));
            }
            freeifaddrs(ifaddr);
        }
    }

    lan_codec::BeaconPacket makeHostBeacon() const {
        lan_codec::BeaconPacket b{};
        b.roomId = roomId_;
        b.tcpPort = boundPort_;
        b.game = advertisedGame_;
        b.variant = advertisedVariant_;
        b.reconnectWaiting = hasHadPeer_ && (state_ == State::HostingWaiting);
        b.roomTag = roomTag_;
        return b;
    }

    void broadcastHostBeacon() {
        if (hostUdpFd_ < 0 || state_ != State::HostingWaiting || boundPort_ == 0) return;
        const std::string pkt = lan_codec::encodeBeacon(makeHostBeacon());
        sendUdpBroadcastOrLoopback(hostUdpFd_, pkt, hostBeaconTargetPort_, hostLoopbackOnly_);
    }

    void startHostBeaconSocket(std::uint16_t discoveryPort, bool loopbackOnly) {
        stopHostBeaconSocket();
        hostBeaconTargetPort_ = discoveryPort ? discoveryPort : kDefaultDiscoveryPort;
        const int ufd = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (ufd < 0) return;
        configureUdpSocketOptions(ufd);

        struct sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(loopbackOnly ? INADDR_LOOPBACK : INADDR_ANY);
        addr.sin_port = htons(discoveryPort);
        if (::bind(ufd, reinterpret_cast<const struct sockaddr*>(&addr), sizeof(addr)) != 0) {
            addr.sin_port = htons(0);
            if (::bind(ufd, reinterpret_cast<const struct sockaddr*>(&addr), sizeof(addr)) != 0) {
                ::close(ufd);
                return;
            }
        }
        struct sockaddr_in bound{};
        socklen_t blen = sizeof(bound);
        if (::getsockname(ufd, reinterpret_cast<struct sockaddr*>(&bound), &blen) == 0) {
            boundDiscoveryPort_ = ntohs(bound.sin_port);
            if (discoveryPort == 0) hostBeaconTargetPort_ = boundDiscoveryPort_;
        }

        hostUdpFd_ = ufd;
        hostUdpReadSource_ = dispatch_source_create(DISPATCH_SOURCE_TYPE_READ, static_cast<uintptr_t>(ufd), 0,
                                                    dispatch_get_main_queue());
        if (hostUdpReadSource_) {
            dispatch_source_set_event_handler(hostUdpReadSource_, ^{
              this->handleHostDiscoveryProbe();
            });
            dispatch_source_set_cancel_handler(hostUdpReadSource_, ^{
              ::close(ufd);
            });
            dispatch_resume(hostUdpReadSource_);
        }

        hostBeaconTimer_ = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, dispatch_get_main_queue());
        if (hostBeaconTimer_) {
            dispatch_source_set_timer(hostBeaconTimer_, dispatch_time(DISPATCH_TIME_NOW, 80 * NSEC_PER_MSEC),
                                      1200 * NSEC_PER_MSEC, 120 * NSEC_PER_MSEC);
            dispatch_source_set_event_handler(hostBeaconTimer_, ^{
              this->broadcastHostBeacon();
            });
            dispatch_resume(hostBeaconTimer_);
        }
    }

    void stopHostBeaconSocket() {
        if (hostBeaconTimer_) {
            dispatch_source_cancel(hostBeaconTimer_);
            hostBeaconTimer_ = nil;
        }
        if (hostUdpReadSource_) {
            dispatch_source_cancel(hostUdpReadSource_);
            hostUdpReadSource_ = nil;
            hostUdpFd_ = -1;
        } else if (hostUdpFd_ >= 0) {
            ::close(hostUdpFd_);
            hostUdpFd_ = -1;
        }
    }

    void handleHostDiscoveryProbe() {
        if (hostUdpFd_ < 0) return;
        char buf[1024];
        for (;;) {
            struct sockaddr_in sender{};
            socklen_t slen = sizeof(sender);
            const ssize_t n =
                ::recvfrom(hostUdpFd_, buf, sizeof(buf), 0, reinterpret_cast<struct sockaddr*>(&sender), &slen);
            if (n <= 0) break;
            if (state_ != State::HostingWaiting || boundPort_ == 0) continue;
            const auto dec = lan_codec::decodeLine(std::string_view(buf, static_cast<std::size_t>(n)));
            if (std::holds_alternative<lan_codec::DiscoverProbePacket>(dec)) {
                const std::string reply = lan_codec::encodeBeacon(makeHostBeacon());
                (void)::sendto(hostUdpFd_, reply.data(), reply.size(), 0,
                               reinterpret_cast<const struct sockaddr*>(&sender), slen);
            }
        }
    }

    void pruneStaleRooms() {
        const double now = steadyNowSec();
        const std::size_t before = discoveredRooms_.size();
        discoveredRooms_.erase(
            std::remove_if(discoveredRooms_.begin(), discoveredRooms_.end(),
                           [now](const lan_codec::DiscoveredLanRoom& r) { return now - r.lastSeenSeconds > 8.0; }),
            discoveredRooms_.end());
        if (discoveredRooms_.size() != before && onRoomsUpdated) {
            onRoomsUpdated(discoveredRooms_);
        }
    }

    void handleDiscoveryRead() {
        if (scanUdpFd_ < 0) return;
        char buf[1024];
        bool changed = false;
        for (;;) {
            struct sockaddr_in sender{};
            socklen_t slen = sizeof(sender);
            const ssize_t n =
                ::recvfrom(scanUdpFd_, buf, sizeof(buf), 0, reinterpret_cast<struct sockaddr*>(&sender), &slen);
            if (n <= 0) break;
            std::string_view sv(buf, static_cast<std::size_t>(n));
            while (!sv.empty() && (sv.back() == '\n' || sv.back() == '\r')) sv.remove_suffix(1);
            const auto dec = lan_codec::decodeLine(sv);
            const auto* bcn = std::get_if<lan_codec::BeaconPacket>(&dec);
            if (!bcn || bcn->roomId == roomId_) continue;  // ignore own room beacon

            char ipStr[INET_ADDRSTRLEN]{};
            if (!::inet_ntop(AF_INET, &sender.sin_addr, ipStr, sizeof(ipStr)) || ipStr[0] == '\0') continue;

            lan_codec::DiscoveredLanRoom room{};
            room.roomId = bcn->roomId;
            room.hostIp = ipStr;
            room.tcpPort = bcn->tcpPort;
            room.game = bcn->game;
            room.variant = bcn->variant;
            room.reconnectWaiting = bcn->reconnectWaiting;
            room.roomTag = bcn->roomTag.data();
            room.lastSeenSeconds = steadyNowSec();

            bool updated = false;
            for (auto& existing : discoveredRooms_) {
                if (existing.roomId == room.roomId ||
                    (existing.hostIp == room.hostIp && existing.tcpPort == room.tcpPort)) {
                    existing = room;
                    updated = true;
                    break;
                }
            }
            if (!updated && discoveredRooms_.size() < 16u) {
                discoveredRooms_.push_back(room);
            }
            changed = true;

            if (quickJoinPending_ && state_ == State::Disconnected) {
                quickJoinPending_ = false;
                if (quickJoinTimer_) {
                    dispatch_source_cancel(quickJoinTimer_);
                    quickJoinTimer_ = nil;
                }
                if (onRoomsUpdated) onRoomsUpdated(discoveredRooms_);
                join(room.endpoint(), room.tcpPort);
                return;
            }
        }
        pruneStaleRooms();
        if (changed && onRoomsUpdated) {
            onRoomsUpdated(discoveredRooms_);
        }
    }

    void cancelReconnectTimer() {
        if (reconnectTimer_) {
            dispatch_source_cancel(reconnectTimer_);
            reconnectTimer_ = nil;
        }
    }

    void scheduleGuestReconnect() {
        if (!autoReconnect_ || !everConnectedGuest_ || lastHostIp_.empty() || lastHostPort_ == 0 ||
            reconnectAttempts_ >= 8) {
            reconnectActive_ = false;
            stopInternal(false);
            updateState(State::Disconnected, "房主已断开连接");
            return;
        }
        ++reconnectAttempts_;
        reconnectActive_ = true;
        closePeerOnly();
        state_ = State::Disconnected;
        char msg[128]{};
        std::snprintf(msg, sizeof(msg), "连接中断 · 自动重连中 (%d/8)…", reconnectAttempts_);
        updateState(State::Disconnected, msg);

        cancelReconnectTimer();
        reconnectTimer_ = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, dispatch_get_main_queue());
        if (!reconnectTimer_) return;
        const unsigned waitMs = (reconnectAttempts_ == 1) ? std::min(reconnectDelayMs_, 300u) : reconnectDelayMs_;
        dispatch_source_set_timer(
            reconnectTimer_,
            dispatch_time(DISPATCH_TIME_NOW, static_cast<int64_t>(waitMs) * NSEC_PER_MSEC),
            DISPATCH_TIME_FOREVER, 40 * NSEC_PER_MSEC);
        dispatch_source_set_event_handler(reconnectTimer_, ^{
          if (!this->reconnectActive_) return;
          (void)this->beginConnectAttempt(this->lastHostIp_, this->lastHostPort_, true);
        });
        dispatch_resume(reconnectTimer_);
    }

    bool beginConnectAttempt(const std::string& ip, std::uint16_t port, bool isReconnect) {
        closePeerOnly();
        const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) {
            if (isReconnect) {
                scheduleGuestReconnect();
            } else {
                updateState(State::Disconnected, "创建连接套接字失败");
            }
            return false;
        }
        configureSocketOptions(fd);

        struct sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        ::inet_pton(AF_INET, ip.c_str(), &addr.sin_addr);

        const int rc = ::connect(fd, reinterpret_cast<const struct sockaddr*>(&addr), sizeof(addr));
        if (rc < 0 && errno != EINPROGRESS) {
            lastErrno_ = errno;
            ::close(fd);
            if (isReconnect) {
                scheduleGuestReconnect();
            } else {
                updateState(State::Disconnected, "无法连接房主地址");
            }
            return false;
        }
        lastErrno_ = 0;

        boundPort_ = port;
        peerFd_ = fd;
        recvBuf_.clear();
        sendBuf_.clear();

        if (rc == 0) {
            completeGuestConnected(ip, port, isReconnect);
            return true;
        }

        char cmsg[128];
        if (isReconnect) {
            std::snprintf(cmsg, sizeof(cmsg), "正在自动重连 %s:%u (%d/8)…", ip.c_str(),
                          static_cast<unsigned>(port), reconnectAttempts_);
        } else {
            std::snprintf(cmsg, sizeof(cmsg), "正在连接 %s:%u…", ip.c_str(), static_cast<unsigned>(port));
        }
        updateState(State::Connecting, cmsg);

        connectSource_ = dispatch_source_create(DISPATCH_SOURCE_TYPE_WRITE, static_cast<uintptr_t>(fd), 0,
                                                dispatch_get_main_queue());
        if (!connectSource_) {
            ::close(fd);
            peerFd_ = -1;
            updateState(State::Disconnected, "连接事件源创建失败");
            return false;
        }
        const std::string targetIp = ip;
        const std::uint16_t targetPort = port;
        dispatch_source_set_event_handler(connectSource_, ^{
          this->handleConnectReady(targetIp, targetPort, isReconnect);
        });
        dispatch_resume(connectSource_);

        connectTimer_ = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, dispatch_get_main_queue());
        if (connectTimer_) {
            const int64_t timeoutNs = isReconnect ? (2000LL * NSEC_PER_MSEC) : (5LL * NSEC_PER_SEC);
            dispatch_source_set_timer(connectTimer_, dispatch_time(DISPATCH_TIME_NOW, timeoutNs),
                                      DISPATCH_TIME_FOREVER, 80 * NSEC_PER_MSEC);
            dispatch_source_set_event_handler(connectTimer_, ^{
              if (this->state_ == State::Connecting) {
                  if (isReconnect) {
                      this->scheduleGuestReconnect();
                  } else {
                      this->stopInternal(false);
                      this->updateState(State::Disconnected, "连接超时，请检查房主 IP 与局域网");
                  }
              }
            });
            dispatch_resume(connectTimer_);
        }
        return true;
    }

    void completeGuestConnected(const std::string& ip, std::uint16_t port, bool wasReconnect) {
        cancelReconnectTimer();
        const bool resume = wasReconnect || everConnectedGuest_;
        everConnectedGuest_ = true;
        reconnectActive_ = false;
        reconnectAttempts_ = 0;
        attachPeerReadSource(peerFd_, false);
        char msg[128];
        std::snprintf(msg, sizeof(msg), "%s (%s:%u)", resume ? "已重连房主" : "已连房主", ip.c_str(),
                      static_cast<unsigned>(port));
        updateState(State::ConnectedGuest, msg);
        if (sessionToken_ != 0) {
            (void)sendRawLine(lan_codec::encodeSessionHello(sessionToken_, resume));
        }
    }

    static void configureSocketOptions(int fd) {
        const int flags = ::fcntl(fd, F_GETFL, 0);
        if (flags >= 0) ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
        const int fdFlags = ::fcntl(fd, F_GETFD, 0);
        if (fdFlags >= 0) ::fcntl(fd, F_SETFD, fdFlags | FD_CLOEXEC);
        int one = 1;
        ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        ::setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));
#ifdef SO_NOSIGPIPE
        ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
        ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    }

    void updateState(State s, const std::string& text) {
        state_ = s;
        statusText_ = text;
        if (onStatus) onStatus(s, text);
    }

    void closePeerOnly() {
        if (connectTimer_) {
            dispatch_source_cancel(connectTimer_);
            connectTimer_ = nil;
        }
        if (connectSource_) {
            dispatch_source_cancel(connectSource_);
            connectSource_ = nil;
        }
        if (writeSource_) {
            dispatch_source_cancel(writeSource_);
            writeSource_ = nil;
        }
        if (readSource_) {
            dispatch_source_cancel(readSource_);
            readSource_ = nil;
            peerFd_ = -1;  // readSource_ cancel handler closes fd
        } else if (peerFd_ >= 0) {
            ::close(peerFd_);
            peerFd_ = -1;
        }
        recvBuf_.clear();
        sendBuf_.clear();
    }

    void stopInternal(bool resetPort) {
        closePeerOnly();
        if (listenSource_) {
            dispatch_source_cancel(listenSource_);
            listenSource_ = nil;
            listenFd_ = -1;  // listenSource_ cancel handler closes fd
        } else if (listenFd_ >= 0) {
            ::close(listenFd_);
            listenFd_ = -1;
        }
        if (resetPort) boundPort_ = 0;
        state_ = State::Disconnected;
    }

    void handleAccept() {
        if (listenFd_ < 0) return;
        struct sockaddr_in peer{};
        socklen_t len = sizeof(peer);
        const int cfd = ::accept(listenFd_, reinterpret_cast<struct sockaddr*>(&peer), &len);
        if (cfd < 0) return;
        char host[INET_ADDRSTRLEN]{};
        ::inet_ntop(AF_INET, &peer.sin_addr, host, sizeof(host));
        if (peerFd_ >= 0) {
            handlePeerRead(true);
        }
        if (peerFd_ >= 0) {
            // If the same peer IP reconnects while the old TCP socket is half-open, replace it cleanly;
            // otherwise reject third-party connections in 1v1 mode.
            if (!lastPeerIp_.empty() && host[0] != '\0' && lastPeerIp_ == host) {
                closePeerOnly();
            } else {
                ::close(cfd);
                return;
            }
        }
        if (host[0] != '\0') lastPeerIp_ = host;
        configureSocketOptions(cfd);
        peerFd_ = cfd;
        recvBuf_.clear();
        sendBuf_.clear();
        attachPeerReadSource(cfd, true);

        const bool resumedPeer = hasHadPeer_;
        hasHadPeer_ = true;
        char msg[128];
        std::snprintf(msg, sizeof(msg), "%s (%s)", resumedPeer ? "对手已重连同步" : "对手已连接",
                      host[0] ? host : "客机");
        updateState(State::ConnectedHost, msg);
        (void)sendRawLine(lan_codec::encodeSessionHello(sessionToken_, resumedPeer));
    }

    void handleConnectReady(const std::string& ip, std::uint16_t port, bool isReconnect = false) {
        if (peerFd_ < 0 || state_ != State::Connecting) return;
        int err = 0;
        socklen_t elen = sizeof(err);
        if (::getsockopt(peerFd_, SOL_SOCKET, SO_ERROR, &err, &elen) != 0 || err != 0) {
            if (isReconnect) {
                scheduleGuestReconnect();
            } else {
                stopInternal(false);
                updateState(State::Disconnected, "连接被拒绝或不可达");
            }
            return;
        }
        if (connectTimer_) {
            dispatch_source_cancel(connectTimer_);
            connectTimer_ = nil;
        }
        if (connectSource_) {
            dispatch_source_cancel(connectSource_);
            connectSource_ = nil;
        }
        completeGuestConnected(ip, port, isReconnect);
    }

    void attachPeerReadSource(int fd, bool hostRole) {
        readSource_ = dispatch_source_create(DISPATCH_SOURCE_TYPE_READ, static_cast<uintptr_t>(fd), 0,
                                             dispatch_get_main_queue());
        if (!readSource_) return;
        dispatch_source_set_event_handler(readSource_, ^{
          this->handlePeerRead(hostRole);
        });
        dispatch_source_set_cancel_handler(readSource_, ^{
          ::close(fd);
        });
        dispatch_resume(readSource_);
    }

    void handlePeerDisconnected(bool hostRole) {
        closePeerOnly();
        if (hostRole && listenFd_ >= 0) {
            char msg[128];
            std::snprintf(msg, sizeof(msg), "对手断线 · 等待重连 (%s:%u)",
                          hostLoopbackOnly_ ? "127.0.0.1" : primaryLanIPv4().c_str(),
                          static_cast<unsigned>(boundPort_));
            updateState(State::HostingWaiting, msg);
            broadcastHostBeacon();
        } else if (!hostRole && autoReconnect_ && everConnectedGuest_) {
            scheduleGuestReconnect();
        } else {
            stopInternal(false);
            updateState(State::Disconnected, "房主已断开连接");
        }
    }

    void handlePeerRead(bool hostRole) {
        if (peerFd_ < 0) return;
        char chunk[4096];
        for (;;) {
            const ssize_t n = ::recv(peerFd_, chunk, sizeof(chunk), 0);
            if (n > 0) {
                recvBuf_.append(chunk, static_cast<std::size_t>(n));
                if (recvBuf_.size() > 65536u) {
                    handlePeerDisconnected(hostRole);
                    return;
                }
            } else if (n == 0) {
                handlePeerDisconnected(hostRole);
                return;
            } else {
                if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                handlePeerDisconnected(hostRole);
                return;
            }
        }

        std::size_t pos = 0;
        for (;;) {
            const auto nl = recvBuf_.find('\n', pos);
            if (nl == std::string::npos) break;
            const std::size_t lineLen = nl - pos;
            if (lineLen > 0 && lineLen <= 32768u) {
                parseLine(recvBuf_.data() + pos, lineLen);
            }
            pos = nl + 1;
        }
        if (pos > 0) recvBuf_.erase(0, pos);
    }

    void trimSendBacklog(std::size_t incomingBytes) {
        if (sendBuf_.size() + incomingBytes <= 65536u) return;
        // Preserve any partially-transmitted line up to the first '\n' so wire JSON framing never breaks.
        const auto firstNl = sendBuf_.find('\n');
        if (firstNl != std::string::npos && firstNl + 1u < 65536u) {
            sendBuf_.erase(firstNl + 1u);
        } else if (firstNl == std::string::npos) {
            return;
        }
    }

    bool sendRawLine(const std::string& line) {
        if (peerFd_ < 0 || !connected() || line.empty() || line.size() > 30000u) return false;
        trimSendBacklog(line.size());
        if (sendBuf_.size() + line.size() > 98304u) return false;
        sendBuf_.append(line);
        flushSendBuffer();
        return true;
    }

    bool sendDictionary(NSDictionary* dict) {
        if (peerFd_ < 0 || !connected()) return false;
        NSError* err = nil;
        NSData* data = [NSJSONSerialization dataWithJSONObject:dict options:0 error:&err];
        if (!data || err || data.length == 0 || data.length > 30000u) return false;
        trimSendBacklog(data.length + 1u);
        if (sendBuf_.size() + data.length + 1u > 98304u) return false;
        sendBuf_.append(static_cast<const char*>(data.bytes), data.length);
        sendBuf_.push_back('\n');
        flushSendBuffer();
        return true;
    }

    void flushSendBuffer() {
        if (peerFd_ < 0 || sendBuf_.empty()) return;
        while (!sendBuf_.empty()) {
            const ssize_t sent = ::send(peerFd_, sendBuf_.data(), sendBuf_.size(), 0);
            if (sent > 0) {
                sendBuf_.erase(0, static_cast<std::size_t>(sent));
            } else {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    armWriteSource();
                    return;
                }
                handlePeerDisconnected(state_ == State::ConnectedHost);
                return;
            }
        }
        if (writeSource_) {
            dispatch_source_cancel(writeSource_);
            writeSource_ = nil;
        }
    }

    void armWriteSource() {
        if (writeSource_ || peerFd_ < 0) return;
        writeSource_ = dispatch_source_create(DISPATCH_SOURCE_TYPE_WRITE, static_cast<uintptr_t>(peerFd_), 0,
                                              dispatch_get_main_queue());
        if (!writeSource_) return;
        dispatch_source_set_event_handler(writeSource_, ^{
          this->flushSendBuffer();
        });
        dispatch_resume(writeSource_);
    }

    static Vec2 readVec2(id obj, Vec2 fallback = {}) {
        if (![obj isKindOfClass:NSArray.class]) return fallback;
        NSArray* arr = static_cast<NSArray*>(obj);
        if (arr.count < 2) return fallback;
        const float x = [arr[0] respondsToSelector:@selector(floatValue)] ? [arr[0] floatValue] : fallback.x;
        const float y = [arr[1] respondsToSelector:@selector(floatValue)] ? [arr[1] floatValue] : fallback.y;
        return (std::isfinite(x) && std::isfinite(y)) ? Vec2{x, y} : fallback;
    }

    void parseLine(const char* bytes, std::size_t len) {
        NSData* data = [NSData dataWithBytesNoCopy:const_cast<char*>(bytes) length:len freeWhenDone:NO];
        id obj = [NSJSONSerialization JSONObjectWithData:data options:0 error:nil];
        if (![obj isKindOfClass:NSDictionary.class]) return;
        NSDictionary* d = static_cast<NSDictionary*>(obj);
        NSString* type = [d[@"t"] isKindOfClass:NSString.class] ? d[@"t"] : nil;
        if (!type) return;

        if ([type isEqualToString:@"hlo"]) {
            const auto dec = lan_codec::decodeLine(std::string_view(bytes, len));
            if (const auto* hp = std::get_if<lan_codec::SessionHelloPacket>(&dec)) {
                if (state_ == State::ConnectedGuest && hp->sessionToken != 0) {
                    sessionToken_ = hp->sessionToken;
                }
            }
            return;
        }

        if ([type isEqualToString:@"hk_in"] && state_ == State::ConnectedHost) {
            const auto dec = lan_codec::decodeLine(std::string_view(bytes, len));
            if (const auto* pkt = std::get_if<lan_codec::HockeyInputPacket>(&dec)) {
                if (onHockeyInput) onHockeyInput(pkt->input);
            }
            return;
        }

        if ([type isEqualToString:@"hk_st"] && state_ == State::ConnectedGuest) {
            const auto dec = lan_codec::decodeLine(std::string_view(bytes, len));
            if (const auto* pkt = std::get_if<lan_codec::HockeyStatePacket>(&dec)) {
                if (onHockeyState) onHockeyState(pkt->state, pkt->sounds);
            }
            return;
        }

        if ([type isEqualToString:@"rule"] && state_ == State::ConnectedGuest) {
            const int r = [d[@"r"] intValue];
            if (r >= 0 && r <= 2 && onRuleSync) {
                onRuleSync(static_cast<BilliardsRule>(r));
            }
            return;
        }

        if ([type isEqualToString:@"act"] && state_ == State::ConnectedHost) {
            BilliardsRemoteAction act;
            const int k = [d[@"k"] intValue];
            if (k < 0 || k > 2) return;
            act.kind = static_cast<BilliardsRemoteAction::Kind>(k);
            act.aimDir = readVec2(d[@"ad"], {1.0f, 0.0f});
            act.power = std::clamp([d[@"pw"] floatValue], 0.0f, 1.0f);
            act.tipOffset = readVec2(d[@"to"], {0.0f, 0.0f});
            act.placeNormPos = readVec2(d[@"pl"], {0.25f, 0.5f});
            act.pullRatio = std::clamp([d[@"pr"] floatValue], 0.0f, 1.0f);
            if (onAction) onAction(act);
            return;
        }

        if ([type isEqualToString:@"snap"] && state_ == State::ConnectedGuest) {
            BilliardsLanSnapshot snap;
            const int st = [d[@"st"] intValue];
            if (st >= 0 && st <= 4) snap.state = static_cast<BilliardsState>(st);
            const int ru = [d[@"ru"] intValue];
            if (ru >= 0 && ru <= 2) snap.rule = static_cast<BilliardsRule>(ru);
            snap.currentPlayer = [d[@"cp"] intValue] == 1 ? 1 : 0;
            snap.winner = [d[@"wn"] intValue];
            if ([d[@"sc"] isKindOfClass:NSArray.class] && [d[@"sc"] count] >= 2) {
                snap.scores[0] = [d[@"sc"][0] intValue];
                snap.scores[1] = [d[@"sc"][1] intValue];
            }
            if ([d[@"pg"] isKindOfClass:NSArray.class] && [d[@"pg"] count] >= 2) {
                const int g0 = [d[@"pg"][0] intValue];
                const int g1 = [d[@"pg"][1] intValue];
                if (g0 >= 0 && g0 <= 5) snap.playerGroups[0] = static_cast<BallGroup>(g0);
                if (g1 >= 0 && g1 <= 5) snap.playerGroups[1] = static_cast<BallGroup>(g1);
            }
            snap.snookerTarget = [d[@"sn"] intValue];
            snap.shots = [d[@"sh"] intValue];
            snap.pocketedCount = [d[@"pk"] intValue];
            snap.whitePocketed = [d[@"wp"] intValue] != 0;
            snap.whiteNormPos = readVec2(d[@"w"], {0.25f, 0.5f});
            snap.placeNormPos = readVec2(d[@"pl"], {0.25f, 0.5f});
            snap.aimDir = readVec2(d[@"ad"], {1.0f, 0.0f});
            snap.tipOffset = readVec2(d[@"to"], {0.0f, 0.0f});
            snap.pullRatio = std::clamp([d[@"pr"] floatValue], 0.0f, 1.0f);
            if ([d[@"nt"] isKindOfClass:NSString.class]) {
                const char* utf8 = [d[@"nt"] UTF8String];
                if (utf8) std::snprintf(snap.statusNote.data(), snap.statusNote.size(), "%s", utf8);
            }
            if ([d[@"b"] isKindOfClass:NSArray.class]) {
                NSArray* ba = static_cast<NSArray*>(d[@"b"]);
                const NSUInteger cap = std::min<NSUInteger>(ba.count, 91u);
                snap.balls.reserve(cap);
                for (NSUInteger i = 0; i < cap; ++i) {
                    if (![ba[i] isKindOfClass:NSArray.class]) continue;
                    NSArray* row = static_cast<NSArray*>(ba[i]);
                    if (row.count < 8) continue;
                    BilliardsLanBall lb;
                    lb.normPos = {
                        [row[0] floatValue],
                        [row[1] floatValue],
                    };
                    if (!lb.normPos.isFinite()) lb.normPos = {0.5f, 0.5f};
                    lb.number = [row[2] intValue];
                    const int grp = [row[3] intValue];
                    if (grp >= 0 && grp <= 5) lb.group = static_cast<BallGroup>(grp);
                    lb.pocketed = [row[4] intValue] != 0;
                    lb.tube = [row[5] intValue] == 1 ? 1 : 0;
                    lb.tubeSlot = [row[6] intValue];
                    lb.dropProgress = std::clamp([row[7] floatValue], 0.0f, 1.0f);
                    snap.balls.push_back(lb);
                }
            }
            std::vector<SoundEvent> sounds;
            if ([d[@"sd"] isKindOfClass:NSArray.class]) {
                NSArray* sa = static_cast<NSArray*>(d[@"sd"]);
                const NSUInteger sCap = std::min<NSUInteger>(sa.count, 16u);
                for (NSUInteger i = 0; i < sCap; ++i) {
                    if (![sa[i] isKindOfClass:NSArray.class]) continue;
                    NSArray* srow = static_cast<NSArray*>(sa[i]);
                    if (srow.count < 4) continue;
                    const int sk = [srow[0] intValue];
                    if (sk < 0 || sk > 3) continue;
                    SoundEvent ev;
                    ev.kind = static_cast<SoundKind>(sk);
                    ev.gain = std::clamp([srow[1] floatValue], 0.05f, 1.0f);
                    ev.pan = std::clamp([srow[2] floatValue], -0.90f, 0.90f);
                    ev.pitch = std::clamp([srow[3] floatValue], 0.90f, 1.10f);
                    sounds.push_back(ev);
                }
            }
            if (onSnapshot) onSnapshot(snap, sounds);
        }
    }

    int listenFd_ = -1;
    int peerFd_ = -1;
    int hostUdpFd_ = -1;
    int scanUdpFd_ = -1;
    int lastErrno_ = 0;
    std::uint16_t boundPort_ = 0;
    std::uint16_t boundDiscoveryPort_ = 0;
    std::uint16_t hostBeaconTargetPort_ = kDefaultDiscoveryPort;
    std::uint16_t scanTargetPort_ = kDefaultDiscoveryPort;
    std::uint16_t lastHostPort_ = 0;
    std::string lastHostIp_;
    std::string lastPeerIp_;
    std::uint32_t roomId_ = 0;
    std::uint32_t sessionToken_ = 0;
    std::array<char, 24> roomTag_{};
    lan_codec::LanGameKind advertisedGame_ = lan_codec::LanGameKind::Billiards;
    int advertisedVariant_ = 0;
    bool hostLoopbackOnly_ = false;
    bool scanLoopbackOnly_ = false;
    bool hasHadPeer_ = false;
    bool autoReconnect_ = true;
    bool everConnectedGuest_ = false;
    bool reconnectActive_ = false;
    int reconnectAttempts_ = 0;
    unsigned reconnectDelayMs_ = 1200;
    bool quickJoinPending_ = false;
    lan_codec::LanGameKind quickJoinGame_ = lan_codec::LanGameKind::Billiards;
    std::vector<lan_codec::DiscoveredLanRoom> discoveredRooms_;
    State state_ = State::Disconnected;
    std::string statusText_ = "局域网未连接";
    std::string recvBuf_;
    std::string sendBuf_;
    dispatch_source_t listenSource_ = nil;
    dispatch_source_t connectSource_ = nil;
    dispatch_source_t connectTimer_ = nil;
    dispatch_source_t readSource_ = nil;
    dispatch_source_t writeSource_ = nil;
    dispatch_source_t hostUdpReadSource_ = nil;
    dispatch_source_t hostBeaconTimer_ = nil;
    dispatch_source_t scanReadSource_ = nil;
    dispatch_source_t quickJoinTimer_ = nil;
    dispatch_source_t reconnectTimer_ = nil;
};

}  // namespace dpp::mac
