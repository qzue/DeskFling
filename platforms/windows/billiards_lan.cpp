#define _WINSOCK_DEPRECATED_NO_WARNINGS
// clang-format off
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
// clang-format on

#include "platforms/windows/billiards_lan.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string_view>

#include "core/modes/billiards_lan_codec.h"

namespace dpp::winmirror {

namespace {
SOCKET toSock(std::uintptr_t v) noexcept { return static_cast<SOCKET>(v); }
std::uintptr_t fromSock(SOCKET s) noexcept { return static_cast<std::uintptr_t>(s); }

std::uint32_t makeRandomId() noexcept {
    static std::atomic<std::uint32_t> counter{1u};
    const std::uint32_t seq = counter.fetch_add(1u, std::memory_order_relaxed);
    LARGE_INTEGER qpc{};
    QueryPerformanceCounter(&qpc);
    const std::uint64_t t = GetTickCount64() ^ static_cast<std::uint64_t>(qpc.QuadPart) ^
                            (static_cast<std::uint64_t>(GetCurrentProcessId()) << 16u) ^
                            (static_cast<std::uint64_t>(seq) * 0x9E3779B97F4A7C15ULL);
    std::uint32_t x = static_cast<std::uint32_t>(t ^ (t >> 29u)) | 1u;
    x ^= x << 13u;
    x ^= x >> 17u;
    x ^= x << 5u;
    return ((((x & 0x7FFFFF00u) + 101u) ^ (seq * 2654435761u)) & 0x7FFFFFFFu) | 1u;
}

void sendUdpPacket(SOCKET s, const std::string& payload, std::uint16_t port, bool loopbackOnly) noexcept {
    if (s == INVALID_SOCKET || port == 0 || payload.empty()) return;
    sockaddr_in lo{};
    lo.sin_family = AF_INET;
    lo.sin_port = htons(port);
    lo.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    (void)::sendto(s, payload.data(), static_cast<int>(payload.size()), 0,
                   reinterpret_cast<const sockaddr*>(&lo), sizeof(lo));
    if (loopbackOnly) return;

    sockaddr_in bcast{};
    bcast.sin_family = AF_INET;
    bcast.sin_port = htons(port);
    bcast.sin_addr.s_addr = htonl(INADDR_BROADCAST);
    (void)::sendto(s, payload.data(), static_cast<int>(payload.size()), 0,
                   reinterpret_cast<const sockaddr*>(&bcast), sizeof(bcast));

    // Also send directed subnet broadcasts (e.g. 192.168.x.255) across all active physical LAN NICs
    // so VPN / TUN / WSL2 virtual adapters never swallow 255.255.255.255 broadcast discovery.
    INTERFACE_INFO ifList[32]{};
    DWORD bytesRet = 0;
    if (::WSAIoctl(s, SIO_GET_INTERFACE_LIST, nullptr, 0, ifList, sizeof(ifList), &bytesRet, nullptr,
                   nullptr) == 0) {
        const std::size_t count = bytesRet / sizeof(INTERFACE_INFO);
        for (std::size_t i = 0; i < count; ++i) {
            const u_long flags = ifList[i].iiFlags;
            if ((flags & IFF_UP) == 0 || (flags & IFF_LOOPBACK) != 0) continue;
            const std::uint32_t ipHost = ntohl(ifList[i].iiAddress.AddressIn.sin_addr.s_addr);
            const std::uint32_t maskHost = ntohl(ifList[i].iiNetmask.AddressIn.sin_addr.s_addr);
            if (ipHost == 0 || (ipHost & 0xFFFF0000u) == 0xA9FE0000u ||
                (ipHost & 0xFFFE0000u) == 0xC6120000u) {
                continue;  // skip link-local 169.254/16 and FakeIP/TUN 198.18/15
            }
            std::uint32_t subBcast = 0;
            if (maskHost != 0 && maskHost != 0xFFFFFFFFu) {
                subBcast = ipHost | (~maskHost);
            } else if ((flags & IFF_BROADCAST) != 0) {
                subBcast = ntohl(ifList[i].iiBroadcastAddress.AddressIn.sin_addr.s_addr);
            }
            if (subBcast == 0 || subBcast == INADDR_BROADCAST || subBcast == INADDR_LOOPBACK) continue;
            sockaddr_in sb{};
            sb.sin_family = AF_INET;
            sb.sin_port = htons(port);
            sb.sin_addr.s_addr = htonl(subBcast);
            (void)::sendto(s, payload.data(), static_cast<int>(payload.size()), 0,
                           reinterpret_cast<const sockaddr*>(&sb), sizeof(sb));
        }
    }
}
}  // namespace

BilliardsLan::BilliardsLan() {
    roomId_ = makeRandomId();
    std::snprintf(roomTag_.data(), roomTag_.size(), "Win-%04X", static_cast<unsigned>(roomId_ & 0xFFFFu));
}

BilliardsLan::~BilliardsLan() {
    stopDiscovery();
    stopHostBeaconSocket();
    stopInternal(true);
    if (wsaStarted_) {
        WSACleanup();
        wsaStarted_ = false;
    }
}

bool BilliardsLan::ensureWsaStarted() {
    if (wsaStarted_) return true;
    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return false;
    wsaStarted_ = true;
    return true;
}

void BilliardsLan::setNotifyWindow(std::uintptr_t hwnd) noexcept {
    notifyHwnd_ = hwnd;
    if (listenSock_ != kInvalidSocket) {
        configureSocket(listenSock_, FD_ACCEPT | FD_CLOSE);
    }
    if (peerSock_ != kInvalidSocket) {
        configureSocket(peerSock_, FD_CONNECT | FD_READ | FD_WRITE | FD_CLOSE);
    }
    if (hostUdpSock_ != kInvalidSocket) {
        configureUdpSocket(hostUdpSock_);
    }
    if (scanUdpSock_ != kInvalidSocket) {
        configureUdpSocket(scanUdpSock_);
    }
}

void BilliardsLan::configureSocket(std::uintptr_t raw, long asyncEvents) {
    const SOCKET s = toSock(raw);
    if (s == INVALID_SOCKET) return;
    u_long nonBlock = 1;
    ioctlsocket(s, FIONBIO, &nonBlock);
    BOOL one = TRUE;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&one), sizeof(one));
    setsockopt(s, SOL_SOCKET, SO_KEEPALIVE, reinterpret_cast<const char*>(&one), sizeof(one));
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&one), sizeof(one));
    if (notifyHwnd_ != 0) {
        WSAAsyncSelect(s, reinterpret_cast<HWND>(notifyHwnd_), kSocketMessage, asyncEvents);
    }
}

void BilliardsLan::configureUdpSocket(std::uintptr_t raw) {
    const SOCKET s = toSock(raw);
    if (s == INVALID_SOCKET) return;
    u_long nonBlock = 1;
    ioctlsocket(s, FIONBIO, &nonBlock);
    BOOL one = TRUE;
    // Do NOT set SO_REUSEADDR on WinSock UDP: on Windows, SO_REUSEADDR allows a second local
    // socket to hijack unicast UDP packets on the same port instead of falling back to port 0.
    setsockopt(s, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&one), sizeof(one));
#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif
    BOOL reportConnReset = FALSE;
    DWORD bytesRet = 0;
    (void)::WSAIoctl(s, SIO_UDP_CONNRESET, &reportConnReset, sizeof(reportConnReset), nullptr, 0, &bytesRet,
                     nullptr, nullptr);
    if (notifyHwnd_ != 0) {
        WSAAsyncSelect(s, reinterpret_cast<HWND>(notifyHwnd_), kSocketMessage, FD_READ);
    }
}

void BilliardsLan::setReconnectDelayMs(unsigned delayMs) noexcept {
    reconnectDelayMs_ = std::clamp(delayMs, 40u, 5000u);
}

void BilliardsLan::setAdvertisedGame(lan_codec::LanGameKind game, int variant) {
    advertisedGame_ = game;
    advertisedVariant_ = variant;
    if (state_ == State::HostingWaiting) broadcastHostBeacon();
}

std::string BilliardsLan::primaryLanIPv4() {
    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return "127.0.0.1";
    std::string result = "127.0.0.1";
    const SOCKET udp = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (udp != INVALID_SOCKET) {
        int bestRank = -1;
        INTERFACE_INFO ifList[32]{};
        DWORD bytesRet = 0;
        if (::WSAIoctl(udp, SIO_GET_INTERFACE_LIST, nullptr, 0, ifList, sizeof(ifList), &bytesRet, nullptr,
                       nullptr) == 0) {
            const std::size_t count = bytesRet / sizeof(INTERFACE_INFO);
            for (std::size_t i = 0; i < count; ++i) {
                const u_long flags = ifList[i].iiFlags;
                if ((flags & IFF_UP) == 0 || (flags & IFF_LOOPBACK) != 0) continue;
                const std::uint32_t ipHost = ntohl(ifList[i].iiAddress.AddressIn.sin_addr.s_addr);
                if (ipHost == 0 || (ipHost & 0xFF000000u) == 0x7F000000u ||
                    (ipHost & 0xFFFF0000u) == 0xA9FE0000u || (ipHost & 0xFFFE0000u) == 0xC6120000u) {
                    continue;  // skip loopback, link-local 169.254/16, and Clash/TUN 198.18/15
                }
                int rank = 1;
                if ((ipHost & 0xFFFF0000u) == 0xC0A80000u)
                    rank = 4;  // 192.168.0.0/16 physical home/office LAN
                else if ((ipHost & 0xFF000000u) == 0x0A000000u)
                    rank = 3;  // 10.0.0.0/8 LAN
                else if ((ipHost & 0xFFF00000u) == 0xAC100000u)
                    rank = 2;  // 172.16.0.0/12 (lower than 192.168/10 to avoid WSL2 vEthernet)
                if (rank > bestRank) {
                    char host[INET_ADDRSTRLEN]{};
                    if (::inet_ntop(AF_INET, &ifList[i].iiAddress.AddressIn.sin_addr, host, sizeof(host)) &&
                        host[0] != '\0') {
                        bestRank = rank;
                        result = host;
                    }
                }
            }
        }
        if (bestRank < 0) {
            sockaddr_in remote{};
            remote.sin_family = AF_INET;
            remote.sin_port = htons(80);
            ::inet_pton(AF_INET, "192.168.1.1", &remote.sin_addr);
            if (::connect(udp, reinterpret_cast<const sockaddr*>(&remote), sizeof(remote)) == 0) {
                sockaddr_in local{};
                int len = static_cast<int>(sizeof(local));
                if (::getsockname(udp, reinterpret_cast<sockaddr*>(&local), &len) == 0) {
                    char host[INET_ADDRSTRLEN]{};
                    if (::inet_ntop(AF_INET, &local.sin_addr, host, sizeof(host)) && host[0] != '\0' &&
                        std::strncmp(host, "0.", 2) != 0 && std::strncmp(host, "169.254.", 8) != 0 &&
                        std::strncmp(host, "198.18.", 7) != 0 && std::strncmp(host, "198.19.", 7) != 0) {
                        result = host;
                    }
                }
            }
        }
        ::closesocket(udp);
    }
    WSACleanup();
    return result;
}

bool BilliardsLan::parseEndpoint(const std::string& raw, std::string& outIp, std::uint16_t& outPort,
                                 std::uint16_t defaultPort) {
    std::string s = raw;
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r' || s.front() == '\n'))
        s.erase(s.begin());
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' || s.back() == '\n'))
        s.pop_back();
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
    in_addr addr{};
    return ::inet_pton(AF_INET, outIp.c_str(), &addr) == 1;
}

void BilliardsLan::updateState(State s, const std::string& text) {
    state_ = s;
    statusText_ = text;
    if (onStatus) onStatus(s, text);
}

void BilliardsLan::closePeerOnly() {
    if (peerSock_ != kInvalidSocket) {
        const SOCKET s = toSock(peerSock_);
        if (notifyHwnd_ != 0) WSAAsyncSelect(s, reinterpret_cast<HWND>(notifyHwnd_), 0, 0);
        ::closesocket(s);
        peerSock_ = kInvalidSocket;
    }
    connectDeadlineMs_ = 0;
    recvBuf_.clear();
    sendBuf_.clear();
}

void BilliardsLan::stopInternal(bool resetPort) {
    closePeerOnly();
    if (listenSock_ != kInvalidSocket) {
        const SOCKET s = toSock(listenSock_);
        if (notifyHwnd_ != 0) WSAAsyncSelect(s, reinterpret_cast<HWND>(notifyHwnd_), 0, 0);
        ::closesocket(s);
        listenSock_ = kInvalidSocket;
    }
    if (resetPort) boundPort_ = 0;
    state_ = State::Disconnected;
}

void BilliardsLan::startHostBeaconSocket(std::uint16_t discoveryPort, bool loopbackOnly) {
    stopHostBeaconSocket();
    hostBeaconTargetPort_ = discoveryPort ? discoveryPort : kDefaultDiscoveryPort;
    const SOCKET ufd = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (ufd == INVALID_SOCKET) return;
    configureUdpSocket(fromSock(ufd));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(loopbackOnly ? INADDR_LOOPBACK : INADDR_ANY);
    addr.sin_port = htons(discoveryPort);
    if (::bind(ufd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0) {
        addr.sin_port = htons(0);
        if (::bind(ufd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0) {
            ::closesocket(ufd);
            return;
        }
    }
    sockaddr_in bound{};
    int blen = static_cast<int>(sizeof(bound));
    if (::getsockname(ufd, reinterpret_cast<sockaddr*>(&bound), &blen) == 0) {
        boundDiscoveryPort_ = ntohs(bound.sin_port);
        if (discoveryPort == 0) hostBeaconTargetPort_ = boundDiscoveryPort_;
    }
    hostUdpSock_ = fromSock(ufd);
    nextBeaconMs_ = GetTickCount64();
    broadcastHostBeacon();
}

void BilliardsLan::stopHostBeaconSocket() {
    if (hostUdpSock_ != kInvalidSocket) {
        const SOCKET s = toSock(hostUdpSock_);
        if (notifyHwnd_ != 0) WSAAsyncSelect(s, reinterpret_cast<HWND>(notifyHwnd_), 0, 0);
        ::closesocket(s);
        hostUdpSock_ = kInvalidSocket;
    }
    nextBeaconMs_ = 0;
}

void BilliardsLan::broadcastHostBeacon() {
    if (hostUdpSock_ == kInvalidSocket || state_ != State::HostingWaiting || boundPort_ == 0) return;
    lan_codec::BeaconPacket b{};
    b.roomId = roomId_;
    b.tcpPort = boundPort_;
    b.game = advertisedGame_;
    b.variant = advertisedVariant_;
    b.reconnectWaiting = hasHadPeer_ && (state_ == State::HostingWaiting);
    b.roomTag = roomTag_;
    const std::string pkt = lan_codec::encodeBeacon(b);
    sendUdpPacket(toSock(hostUdpSock_), pkt, hostBeaconTargetPort_, hostLoopbackOnly_);
    nextBeaconMs_ = GetTickCount64() + 1200ULL;
}

void BilliardsLan::handleHostDiscoveryProbe() {
    if (hostUdpSock_ == kInvalidSocket) return;
    const SOCKET s = toSock(hostUdpSock_);
    char buf[1024];
    for (;;) {
        sockaddr_in sender{};
        int slen = static_cast<int>(sizeof(sender));
        const int n = ::recvfrom(s, buf, static_cast<int>(sizeof(buf)), 0,
                                 reinterpret_cast<sockaddr*>(&sender), &slen);
        if (n <= 0) {
            if (n < 0) {
                const int err = WSAGetLastError();
                if (err == WSAECONNRESET || err == WSAEINTR) continue;
            }
            break;
        }
        if (state_ != State::HostingWaiting || boundPort_ == 0) continue;
        std::string_view sv(buf, static_cast<std::size_t>(n));
        while (!sv.empty() && (sv.back() == '\n' || sv.back() == '\r')) sv.remove_suffix(1);
        const auto dec = lan_codec::decodeLine(sv);
        if (std::holds_alternative<lan_codec::DiscoverProbePacket>(dec)) {
            lan_codec::BeaconPacket b{};
            b.roomId = roomId_;
            b.tcpPort = boundPort_;
            b.game = advertisedGame_;
            b.variant = advertisedVariant_;
            b.reconnectWaiting = hasHadPeer_;
            b.roomTag = roomTag_;
            const std::string reply = lan_codec::encodeBeacon(b);
            (void)::sendto(s, reply.data(), static_cast<int>(reply.size()), 0,
                           reinterpret_cast<const sockaddr*>(&sender), slen);
        }
    }
}

bool BilliardsLan::startDiscovery(std::uint16_t discoveryPort, bool loopbackOnly) {
    stopDiscovery();
    if (!ensureWsaStarted()) return false;
    scanLoopbackOnly_ = loopbackOnly;
    scanTargetPort_ = discoveryPort ? discoveryPort : kDefaultDiscoveryPort;
    const SOCKET ufd = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (ufd == INVALID_SOCKET) return false;
    configureUdpSocket(fromSock(ufd));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(loopbackOnly ? INADDR_LOOPBACK : INADDR_ANY);
    addr.sin_port = htons(discoveryPort);
    if (::bind(ufd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0) {
        addr.sin_port = htons(0);
        if (::bind(ufd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0) {
            ::closesocket(ufd);
            return false;
        }
    }
    sockaddr_in bound{};
    int blen = static_cast<int>(sizeof(bound));
    if (::getsockname(ufd, reinterpret_cast<sockaddr*>(&bound), &blen) == 0) {
        boundDiscoveryPort_ = ntohs(bound.sin_port);
    }
    scanUdpSock_ = fromSock(ufd);
    sendDiscoveryProbe(advertisedGame_);
    return true;
}

void BilliardsLan::stopDiscovery() {
    quickJoinPending_ = false;
    quickJoinDeadlineMs_ = 0;
    if (scanUdpSock_ != kInvalidSocket) {
        const SOCKET s = toSock(scanUdpSock_);
        if (notifyHwnd_ != 0) WSAAsyncSelect(s, reinterpret_cast<HWND>(notifyHwnd_), 0, 0);
        ::closesocket(s);
        scanUdpSock_ = kInvalidSocket;
    }
    boundDiscoveryPort_ = 0;
}

void BilliardsLan::sendDiscoveryProbe(lan_codec::LanGameKind preferred) {
    if (scanUdpSock_ == kInvalidSocket) return;
    const std::string pkt = lan_codec::encodeDiscoverProbe(preferred);
    sendUdpPacket(toSock(scanUdpSock_), pkt, scanTargetPort_, scanLoopbackOnly_);
}

bool BilliardsLan::quickScanAndJoin(lan_codec::LanGameKind preferredGame, std::uint16_t discoveryPort,
                                    bool loopbackOnly) {
    pruneStaleRooms();
    for (const auto& rm : discoveredRooms_) {
        if (rm.game == preferredGame) return join(rm.endpoint(), rm.tcpPort);
    }
    if (!discoveredRooms_.empty()) {
        return join(discoveredRooms_.front().endpoint(), discoveredRooms_.front().tcpPort);
    }
    if (scanUdpSock_ == kInvalidSocket || scanLoopbackOnly_ != loopbackOnly ||
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
    quickJoinDeadlineMs_ = GetTickCount64() + 2500ULL;
    updateState(State::Disconnected, "正在扫描局域网房间…");
    return true;
}

void BilliardsLan::pruneStaleRooms() {
    const double nowSec = static_cast<double>(GetTickCount64()) * 0.001;
    const std::size_t before = discoveredRooms_.size();
    discoveredRooms_.erase(
        std::remove_if(discoveredRooms_.begin(), discoveredRooms_.end(),
                       [nowSec](const lan_codec::DiscoveredLanRoom& r) {
                           return nowSec - r.lastSeenSeconds > 8.0;
                       }),
        discoveredRooms_.end());
    if (discoveredRooms_.size() != before && onRoomsUpdated) {
        onRoomsUpdated(discoveredRooms_);
    }
}

void BilliardsLan::handleDiscoveryRead() {
    if (scanUdpSock_ == kInvalidSocket) return;
    const SOCKET s = toSock(scanUdpSock_);
    char buf[1024];
    bool changed = false;
    for (;;) {
        sockaddr_in sender{};
        int slen = static_cast<int>(sizeof(sender));
        const int n = ::recvfrom(s, buf, static_cast<int>(sizeof(buf)), 0,
                                 reinterpret_cast<sockaddr*>(&sender), &slen);
        if (n <= 0) {
            if (n < 0) {
                const int err = WSAGetLastError();
                if (err == WSAECONNRESET || err == WSAEINTR) continue;
            }
            break;
        }
        std::string_view sv(buf, static_cast<std::size_t>(n));
        while (!sv.empty() && (sv.back() == '\n' || sv.back() == '\r')) sv.remove_suffix(1);
        const auto dec = lan_codec::decodeLine(sv);
        const auto* bcn = std::get_if<lan_codec::BeaconPacket>(&dec);
        if (!bcn || bcn->roomId == roomId_) continue;

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
        room.lastSeenSeconds = static_cast<double>(GetTickCount64()) * 0.001;

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
            quickJoinDeadlineMs_ = 0;
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

bool BilliardsLan::host(std::uint16_t port, bool loopbackOnly, std::uint16_t discoveryPort) {
    stop();
    if (!ensureWsaStarted()) {
        updateState(State::Disconnected, "网络子系统初始化失败");
        return false;
    }
    hostLoopbackOnly_ = loopbackOnly;
    hasHadPeer_ = false;
    lastPeerIp_.clear();
    sessionToken_ = makeRandomId();
    const SOCKET fd = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd == INVALID_SOCKET) {
        lastError_ = WSAGetLastError();
        updateState(State::Disconnected, "创建监听套接字失败");
        return false;
    }
    configureSocket(fromSock(fd), FD_ACCEPT | FD_CLOSE);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(loopbackOnly ? INADDR_LOOPBACK : INADDR_ANY);
    addr.sin_port = htons(port);
    if (::bind(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0 || ::listen(fd, 1) != 0) {
        lastError_ = WSAGetLastError();
        ::closesocket(fd);
        updateState(State::Disconnected, "端口已被占用或无法监听");
        return false;
    }

    sockaddr_in bound{};
    int blen = static_cast<int>(sizeof(bound));
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&bound), &blen) == 0) {
        boundPort_ = ntohs(bound.sin_port);
    } else {
        boundPort_ = port;
    }
    listenSock_ = fromSock(fd);
    lastError_ = 0;

    startHostBeaconSocket(discoveryPort, loopbackOnly);

    const std::string ip = loopbackOnly ? "127.0.0.1" : primaryLanIPv4();
    char msg[128]{};
    std::snprintf(msg, sizeof(msg), "等待加入 (%s:%u · 已开启广播)", ip.c_str(),
                  static_cast<unsigned>(boundPort_));
    updateState(State::HostingWaiting, msg);
    return true;
}

bool BilliardsLan::beginConnectAttempt(const std::string& ip, std::uint16_t port, bool isReconnect) {
    closePeerOnly();
    const SOCKET fd = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd == INVALID_SOCKET) {
        lastError_ = WSAGetLastError();
        if (isReconnect) {
            scheduleGuestReconnect();
        } else {
            updateState(State::Disconnected, "创建连接套接字失败");
        }
        return false;
    }
    configureSocket(fromSock(fd), FD_CONNECT | FD_READ | FD_WRITE | FD_CLOSE);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    ::inet_pton(AF_INET, ip.c_str(), &addr.sin_addr);

    const int rc = ::connect(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr));
    if (rc == SOCKET_ERROR) {
        const int err = WSAGetLastError();
        if (err != WSAEWOULDBLOCK && err != WSAEINPROGRESS && err != WSAEINVAL) {
            lastError_ = err;
            ::closesocket(fd);
            if (isReconnect) {
                scheduleGuestReconnect();
            } else {
                updateState(State::Disconnected, "无法连接房主地址");
            }
            return false;
        }
    }
    lastError_ = 0;
    boundPort_ = port;
    targetIp_ = ip;
    targetPort_ = port;
    isReconnectAttempt_ = isReconnect;
    peerSock_ = fromSock(fd);
    recvBuf_.clear();
    sendBuf_.clear();

    if (rc == 0) {
        const bool resume = isReconnect || everConnectedGuest_;
        everConnectedGuest_ = true;
        reconnectActive_ = false;
        reconnectAttempts_ = 0;
        reconnectNextMs_ = 0;
        char msg[128]{};
        std::snprintf(msg, sizeof(msg), "%s (%s:%u)", resume ? "已重连房主" : "已连房主", ip.c_str(),
                      static_cast<unsigned>(port));
        updateState(State::ConnectedGuest, msg);
        if (sessionToken_ != 0) {
            (void)queueRawLine(lan_codec::encodeSessionHello(sessionToken_, resume));
        }
        return true;
    }

    connectDeadlineMs_ = GetTickCount64() + (isReconnect ? 2000ULL : 5000ULL);
    char cmsg[128]{};
    if (isReconnect) {
        std::snprintf(cmsg, sizeof(cmsg), "正在自动重连 %s:%u (%d/8)…", ip.c_str(),
                      static_cast<unsigned>(port), reconnectAttempts_);
    } else {
        std::snprintf(cmsg, sizeof(cmsg), "正在连接 %s:%u…", ip.c_str(), static_cast<unsigned>(port));
    }
    updateState(State::Connecting, cmsg);
    return true;
}

void BilliardsLan::scheduleGuestReconnect() {
    if (!autoReconnect_ || !everConnectedGuest_ || targetIp_.empty() || targetPort_ == 0 ||
        reconnectAttempts_ >= 8) {
        reconnectActive_ = false;
        reconnectNextMs_ = 0;
        stopInternal(false);
        updateState(State::Disconnected, "房主已断开连接");
        return;
    }
    ++reconnectAttempts_;
    reconnectActive_ = true;
    closePeerOnly();
    state_ = State::Disconnected;
    const unsigned waitMs = (reconnectAttempts_ == 1) ? std::min(reconnectDelayMs_, 300u) : reconnectDelayMs_;
    reconnectNextMs_ = GetTickCount64() + waitMs;
    char msg[128]{};
    std::snprintf(msg, sizeof(msg), "连接中断 · 自动重连中 (%d/8)…", reconnectAttempts_);
    updateState(State::Disconnected, msg);
}

bool BilliardsLan::join(const std::string& endpoint, std::uint16_t defaultPort) {
    quickJoinPending_ = false;
    quickJoinDeadlineMs_ = 0;
    stop();
    if (!ensureWsaStarted()) {
        updateState(State::Disconnected, "网络子系统初始化失败");
        return false;
    }
    std::string ip;
    std::uint16_t port = defaultPort;
    if (!parseEndpoint(endpoint, ip, port, defaultPort)) {
        updateState(State::Disconnected, "IP:端口格式无效");
        return false;
    }
    everConnectedGuest_ = false;
    reconnectAttempts_ = 0;
    reconnectActive_ = false;
    reconnectNextMs_ = 0;
    return beginConnectAttempt(ip, port, false);
}

void BilliardsLan::stop() {
    quickJoinPending_ = false;
    quickJoinDeadlineMs_ = 0;
    const bool wasActive = (state_ != State::Disconnected || reconnectActive_);
    everConnectedGuest_ = false;
    reconnectAttempts_ = 0;
    reconnectActive_ = false;
    reconnectNextMs_ = 0;
    stopHostBeaconSocket();
    stopInternal(true);
    if (wasActive) {
        updateState(State::Disconnected, "局域网未连接");
    }
}

void BilliardsLan::poll() {
    const std::uint64_t nowMs = GetTickCount64();
    if (hostUdpSock_ != kInvalidSocket) {
        handleHostDiscoveryProbe();
        if (state_ == State::HostingWaiting && nextBeaconMs_ > 0 && nowMs >= nextBeaconMs_) {
            broadcastHostBeacon();
        }
    }
    if (scanUdpSock_ != kInvalidSocket) {
        handleDiscoveryRead();
        if (quickJoinPending_ && quickJoinDeadlineMs_ > 0 && nowMs >= quickJoinDeadlineMs_) {
            quickJoinPending_ = false;
            quickJoinDeadlineMs_ = 0;
            if (state_ == State::Disconnected && !reconnectActive_) {
                updateState(State::Disconnected, "未发现局域网房间 · 请先由房主创建或手动填IP");
            }
        }
    }
    if (reconnectActive_ && state_ == State::Disconnected && reconnectNextMs_ > 0 &&
        nowMs >= reconnectNextMs_) {
        reconnectNextMs_ = 0;
        (void)beginConnectAttempt(targetIp_, targetPort_, true);
    }
    if (state_ == State::HostingWaiting || state_ == State::ConnectedHost) {
        handleAccept();
    }
    if (state_ == State::Connecting) {
        handleConnectProgress();
    }
    if (state_ == State::ConnectedHost || state_ == State::ConnectedGuest) {
        flushSendBuffer();
        if (peerSock_ != kInvalidSocket) {
            handlePeerRead(state_ == State::ConnectedHost);
        }
    }
}

void BilliardsLan::handleAccept() {
    if (listenSock_ == kInvalidSocket) return;
    sockaddr_in peer{};
    int len = static_cast<int>(sizeof(peer));
    const SOCKET cfd = ::accept(toSock(listenSock_), reinterpret_cast<sockaddr*>(&peer), &len);
    if (cfd == INVALID_SOCKET) return;
    char hostIp[INET_ADDRSTRLEN]{};
    ::inet_ntop(AF_INET, &peer.sin_addr, hostIp, sizeof(hostIp));
    if (peerSock_ != kInvalidSocket) {
        handlePeerRead(true);
    }
    if (peerSock_ != kInvalidSocket) {
        if (!lastPeerIp_.empty() && hostIp[0] != '\0' && lastPeerIp_ == hostIp) {
            closePeerOnly();
        } else {
            ::closesocket(cfd);
            return;
        }
    }
    if (hostIp[0] != '\0') lastPeerIp_ = hostIp;
    peerSock_ = fromSock(cfd);
    configureSocket(peerSock_, FD_READ | FD_WRITE | FD_CLOSE);
    recvBuf_.clear();
    sendBuf_.clear();

    const bool resumedPeer = hasHadPeer_;
    hasHadPeer_ = true;
    char msg[128]{};
    std::snprintf(msg, sizeof(msg), "%s (%s)", resumedPeer ? "对手已重连同步" : "对手已连接",
                  hostIp[0] ? hostIp : "客机");
    updateState(State::ConnectedHost, msg);
    (void)queueRawLine(lan_codec::encodeSessionHello(sessionToken_, resumedPeer));
}

void BilliardsLan::handleConnectProgress() {
    if (peerSock_ == kInvalidSocket || state_ != State::Connecting) return;
    const SOCKET s = toSock(peerSock_);
    fd_set writeFds{};
    fd_set errFds{};
    FD_ZERO(&writeFds);
    FD_ZERO(&errFds);
    FD_SET(s, &writeFds);
    FD_SET(s, &errFds);
    timeval tv{0, 0};
    const int rc = ::select(0, nullptr, &writeFds, &errFds, &tv);
    if (rc > 0) {
        int soErr = 0;
        int elen = static_cast<int>(sizeof(soErr));
        const bool ok = !FD_ISSET(s, &errFds) && FD_ISSET(s, &writeFds) &&
                        ::getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&soErr), &elen) == 0 &&
                        soErr == 0;
        if (!ok) {
            lastError_ = soErr;
            if (isReconnectAttempt_) {
                scheduleGuestReconnect();
            } else {
                stopInternal(false);
                updateState(State::Disconnected, "连接被拒绝或不可达");
            }
            return;
        }
        connectDeadlineMs_ = 0;
        const bool resume = isReconnectAttempt_ || everConnectedGuest_;
        everConnectedGuest_ = true;
        reconnectActive_ = false;
        reconnectAttempts_ = 0;
        reconnectNextMs_ = 0;
        char msg[128]{};
        std::snprintf(msg, sizeof(msg), "%s (%s:%u)", resume ? "已重连房主" : "已连房主", targetIp_.c_str(),
                      static_cast<unsigned>(targetPort_));
        updateState(State::ConnectedGuest, msg);
        if (sessionToken_ != 0) {
            (void)queueRawLine(lan_codec::encodeSessionHello(sessionToken_, resume));
        }
        return;
    }
    if (connectDeadlineMs_ > 0 && GetTickCount64() >= connectDeadlineMs_) {
        if (isReconnectAttempt_) {
            scheduleGuestReconnect();
        } else {
            stopInternal(false);
            updateState(State::Disconnected, "连接超时，请检查房主 IP 与局域网");
        }
    }
}

void BilliardsLan::handlePeerDisconnected(bool hostRole) {
    closePeerOnly();
    if (hostRole && listenSock_ != kInvalidSocket) {
        char msg[128]{};
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

void BilliardsLan::handlePeerRead(bool hostRole) {
    if (peerSock_ == kInvalidSocket) return;
    const SOCKET s = toSock(peerSock_);
    char chunk[4096];
    for (;;) {
        const int n = ::recv(s, chunk, static_cast<int>(sizeof(chunk)), 0);
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
            const int err = WSAGetLastError();
            if (err == WSAEWOULDBLOCK || err == WSAEINTR) break;
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
            if (peerSock_ == kInvalidSocket) return;
        }
        pos = nl + 1u;
    }
    if (pos > 0) recvBuf_.erase(0, pos);
}

void BilliardsLan::trimSendBacklog(std::size_t incomingBytes) {
    if (sendBuf_.size() + incomingBytes <= 65536u) return;
    // Preserve any partially-transmitted JSON line up to the first '\n' so TCP framing never corrupts.
    const auto firstNl = sendBuf_.find('\n');
    if (firstNl != std::string::npos && firstNl + 1u < 65536u) {
        sendBuf_.erase(firstNl + 1u);
    }
}

bool BilliardsLan::queueRawLine(const std::string& line) {
    if (peerSock_ == kInvalidSocket || !connected() || line.empty() || line.size() > 30000u) return false;
    trimSendBacklog(line.size());
    if (sendBuf_.size() + line.size() > 98304u) return false;
    sendBuf_.append(line);
    flushSendBuffer();
    return true;
}

void BilliardsLan::flushSendBuffer() {
    if (peerSock_ == kInvalidSocket || sendBuf_.empty()) return;
    const SOCKET s = toSock(peerSock_);
    while (!sendBuf_.empty()) {
        const int toSend = static_cast<int>(std::min<std::size_t>(sendBuf_.size(), 32768u));
        const int sent = ::send(s, sendBuf_.data(), toSend, 0);
        if (sent > 0) {
            sendBuf_.erase(0, static_cast<std::size_t>(sent));
        } else {
            const int err = WSAGetLastError();
            if (err == WSAEWOULDBLOCK || err == WSAEINTR) return;
            handlePeerDisconnected(state_ == State::ConnectedHost);
            return;
        }
    }
}

bool BilliardsLan::sendRuleSync(BilliardsRule rule) {
    if (state_ != State::ConnectedHost) return false;
    return queueRawLine(lan_codec::encodeRule(rule));
}

bool BilliardsLan::sendSnapshot(const BilliardsLanSnapshot& snap, std::span<const SoundEvent> sounds) {
    if (state_ != State::ConnectedHost) return false;
    return queueRawLine(lan_codec::encodeSnapshot(snap, sounds));
}

bool BilliardsLan::sendAction(const BilliardsRemoteAction& act) {
    if (state_ != State::ConnectedGuest) return false;
    return queueRawLine(lan_codec::encodeAction(act));
}

bool BilliardsLan::sendHockeyInput(const AirHockeyRemoteInput& in) {
    if (state_ != State::ConnectedGuest) return false;
    return queueRawLine(lan_codec::encodeHockeyInput(in));
}

bool BilliardsLan::sendHockeyState(const AirHockeyLanState& st, std::span<const SoundEvent> sounds) {
    if (state_ != State::ConnectedHost) return false;
    return queueRawLine(lan_codec::encodeHockeyState(st, sounds));
}

void BilliardsLan::parseLine(const char* bytes, std::size_t len) {
    const auto pkt = lan_codec::decodeLine(std::string_view(bytes, len));
    if (const auto* hp = std::get_if<lan_codec::SessionHelloPacket>(&pkt)) {
        if (state_ == State::ConnectedGuest && hp->sessionToken != 0) {
            sessionToken_ = hp->sessionToken;
        }
        return;
    }
    if (const auto* rp = std::get_if<lan_codec::RulePacket>(&pkt)) {
        if (state_ == State::ConnectedGuest && onRuleSync) onRuleSync(rp->rule);
        return;
    }
    if (const auto* ap = std::get_if<lan_codec::ActionPacket>(&pkt)) {
        if (state_ == State::ConnectedHost && onAction) onAction(ap->action);
        return;
    }
    if (const auto* sp = std::get_if<lan_codec::SnapshotPacket>(&pkt)) {
        if (state_ == State::ConnectedGuest && onSnapshot) onSnapshot(sp->snapshot, sp->sounds);
        return;
    }
    if (const auto* hi = std::get_if<lan_codec::HockeyInputPacket>(&pkt)) {
        if (state_ == State::ConnectedHost && onHockeyInput) onHockeyInput(hi->input);
        return;
    }
    if (const auto* hs = std::get_if<lan_codec::HockeyStatePacket>(&pkt)) {
        if (state_ == State::ConnectedGuest && onHockeyState) onHockeyState(hs->state, hs->sounds);
        return;
    }
}

}  // namespace dpp::winmirror
