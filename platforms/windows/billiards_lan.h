#pragma once
// Event-driven UI-thread LAN TCP transport for 2-player Desktop Billiards on Windows.
// Uses non-blocking Winsock2 + optional WSAAsyncSelect window notifications (zero threads/busy-loops).
// Wire-compatible with macOS BilliardsLan (TCP port 39457, newline-delimited UTF-8 JSON).
// Transmits ONLY normalized [0,1] table coordinates, ball numbers/groups, turn/score state,
// and procedural sound triggers — never transmits filenames, paths, icons, or display IDs.

#include <array>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <vector>

#include "core/modes/air_hockey_mode.h"
#include "core/modes/billiards_lan_codec.h"
#include "core/modes/billiards_mode.h"

namespace dpp::winmirror {

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
    static constexpr unsigned kSocketMessage = 0x8010u;  // WM_APP + 16

    std::function<void(State, const std::string&)> onStatus;
    std::function<void(const BilliardsLanSnapshot&, const std::vector<SoundEvent>&)> onSnapshot;
    std::function<void(const BilliardsRemoteAction&)> onAction;
    std::function<void(BilliardsRule)> onRuleSync;
    std::function<void(const AirHockeyRemoteInput&)> onHockeyInput;
    std::function<void(const AirHockeyLanState&, const std::vector<SoundEvent>&)> onHockeyState;
    std::function<void(const std::vector<lan_codec::DiscoveredLanRoom>&)> onRoomsUpdated;

    BilliardsLan();
    ~BilliardsLan();
    BilliardsLan(const BilliardsLan&) = delete;
    BilliardsLan& operator=(const BilliardsLan&) = delete;

    void setNotifyWindow(std::uintptr_t hwnd) noexcept;

    [[nodiscard]] State state() const noexcept { return state_; }
    [[nodiscard]] bool connected() const noexcept {
        return state_ == State::ConnectedHost || state_ == State::ConnectedGuest;
    }
    [[nodiscard]] bool isHost() const noexcept {
        return state_ == State::HostingWaiting || state_ == State::ConnectedHost;
    }
    [[nodiscard]] bool isGuest() const noexcept {
        return state_ == State::Connecting || state_ == State::ConnectedGuest;
    }
    [[nodiscard]] std::uint16_t boundPort() const noexcept { return boundPort_; }
    [[nodiscard]] std::uint16_t boundDiscoveryPort() const noexcept { return boundDiscoveryPort_; }
    [[nodiscard]] bool discovering() const noexcept { return scanUdpSock_ != kInvalidSocket; }
    [[nodiscard]] int lastError() const noexcept { return lastError_; }
    [[nodiscard]] const std::string& statusText() const noexcept { return statusText_; }
    [[nodiscard]] bool reconnecting() const noexcept { return reconnectActive_; }
    [[nodiscard]] bool needsFastPoll() const noexcept {
        return state_ == State::Connecting || reconnectActive_ || quickJoinPending_;
    }
    [[nodiscard]] int reconnectAttempt() const noexcept { return reconnectAttempts_; }
    [[nodiscard]] std::uint32_t sessionToken() const noexcept { return sessionToken_; }
    void setAutoReconnect(bool enabled) noexcept { autoReconnect_ = enabled; }
    void setReconnectDelayMs(unsigned delayMs) noexcept;
    void setAdvertisedGame(lan_codec::LanGameKind game, int variant);
    [[nodiscard]] const std::vector<lan_codec::DiscoveredLanRoom>& discoveredRooms() const noexcept {
        return discoveredRooms_;
    }

    [[nodiscard]] static std::string primaryLanIPv4();
    [[nodiscard]] static bool parseEndpoint(const std::string& raw, std::string& outIp,
                                            std::uint16_t& outPort, std::uint16_t defaultPort = kDefaultPort);

    bool host(std::uint16_t port = kDefaultPort, bool loopbackOnly = false,
              std::uint16_t discoveryPort = kDefaultDiscoveryPort);
    bool join(const std::string& endpoint, std::uint16_t defaultPort = kDefaultPort);
    bool startDiscovery(std::uint16_t discoveryPort = kDefaultDiscoveryPort, bool loopbackOnly = false);
    void stopDiscovery();
    void sendDiscoveryProbe(lan_codec::LanGameKind preferred = lan_codec::LanGameKind::Billiards);
    bool quickScanAndJoin(lan_codec::LanGameKind preferredGame,
                          std::uint16_t discoveryPort = kDefaultDiscoveryPort, bool loopbackOnly = false);
    void stop();

    // Processes pending non-blocking socket accept/connect/read/write events and connect timeouts.
    void poll();

    bool sendRuleSync(BilliardsRule rule);
    bool sendSnapshot(const BilliardsLanSnapshot& snap, std::span<const SoundEvent> sounds);
    bool sendAction(const BilliardsRemoteAction& act);
    bool sendHockeyInput(const AirHockeyRemoteInput& in);
    bool sendHockeyState(const AirHockeyLanState& st, std::span<const SoundEvent> sounds);

   private:
    static constexpr std::uintptr_t kInvalidSocket = ~std::uintptr_t{0};

    bool ensureWsaStarted();
    void configureSocket(std::uintptr_t s, long asyncEvents);
    void configureUdpSocket(std::uintptr_t s);
    void updateState(State s, const std::string& text);
    void closePeerOnly();
    void stopInternal(bool resetPort);
    void startHostBeaconSocket(std::uint16_t discoveryPort, bool loopbackOnly);
    void stopHostBeaconSocket();
    void broadcastHostBeacon();
    void handleHostDiscoveryProbe();
    void handleDiscoveryRead();
    void pruneStaleRooms();
    bool beginConnectAttempt(const std::string& ip, std::uint16_t port, bool isReconnect);
    void scheduleGuestReconnect();
    void handleAccept();
    void handleConnectProgress();
    void handlePeerDisconnected(bool hostRole);
    void handlePeerRead(bool hostRole);
    void trimSendBacklog(std::size_t incomingBytes);
    bool queueRawLine(const std::string& line);
    void flushSendBuffer();
    void parseLine(const char* bytes, std::size_t len);

    bool wsaStarted_ = false;
    std::uintptr_t notifyHwnd_ = 0;
    std::uintptr_t listenSock_ = kInvalidSocket;
    std::uintptr_t peerSock_ = kInvalidSocket;
    std::uintptr_t hostUdpSock_ = kInvalidSocket;
    std::uintptr_t scanUdpSock_ = kInvalidSocket;
    int lastError_ = 0;
    std::uint16_t boundPort_ = 0;
    std::uint16_t boundDiscoveryPort_ = 0;
    std::uint16_t hostBeaconTargetPort_ = kDefaultDiscoveryPort;
    std::uint16_t scanTargetPort_ = kDefaultDiscoveryPort;
    std::uint16_t targetPort_ = 0;
    std::string targetIp_;
    std::string lastPeerIp_;
    std::uint64_t connectDeadlineMs_ = 0;
    std::uint64_t nextBeaconMs_ = 0;
    std::uint64_t reconnectNextMs_ = 0;
    std::uint64_t quickJoinDeadlineMs_ = 0;
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
    bool isReconnectAttempt_ = false;
    int reconnectAttempts_ = 0;
    unsigned reconnectDelayMs_ = 1200;
    bool quickJoinPending_ = false;
    lan_codec::LanGameKind quickJoinGame_ = lan_codec::LanGameKind::Billiards;
    std::vector<lan_codec::DiscoveredLanRoom> discoveredRooms_;
    State state_ = State::Disconnected;
    std::string statusText_ = "局域网未连接";
    std::string recvBuf_;
    std::string sendBuf_;
};

}  // namespace dpp::winmirror
