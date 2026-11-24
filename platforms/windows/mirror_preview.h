#pragma once
#include "core/scene/playground.h"
#include "platforms/display_migration.h"
#include "probe_samples.h"

namespace dpp::winmirror {
// Restricted desktop ListView adapter. Never an arbitrary ancestor HWND.
// This is a runtime-checked Windows Shell implementation convention, not a
// promise that every Shell folder view is a SysListView32 or uses these units.
struct ViewContract {
    HWND shell = nullptr, list = nullptr;
    HMONITOR monitor = nullptr;
    RECT work{}, bounds{}, listClient{}, listScreen{};
    DWORD process = 0, flags = 0;
    LONG_PTR listStyle = 0;
    UINT dpi = 0;
    HRESULT iconSizeHr = E_PENDING, sizedImagesHr = E_PENDING;
    FOLDERVIEWMODE viewMode = FVM_AUTO;
    int iconPixels = 0;  // raw view configuration, not a proved physical image size
    SIZE systemImagePixels{};
    HRESULT systemImageHr = E_PENDING;
    std::vector<SIZE> bitmapPixels;
    std::vector<bool> shortcutItems;
    unsigned shortcutOverlaysOmitted = 0;
    bool sizedImagesReady = false;
    const char* sizedImageStage = "not_attempted";
    std::size_t sizedImageAttempt = 0;
    int expectedMonitorCount = 1;
    bool multiMonitorSession = false;
};
// A pending Shell resize invalidates the candidate without extending the transaction deadline.
bool migrationListBoundsReady(DisplayMigration& migration, const ViewContract& sample, double now);
// Copies the supplied icon into an owned premultiplied bitmap; never owns the HICON.
winprobe::Bitmap bitmapFromIcon(HICON icon);
bool snapshotUsable(const winprobe::Report& report);
POINT mapPoint(HWND from, HWND to, POINT point);
ViewContract inspectContract(winprobe::DesktopView& desktop, const winprobe::Report& report);
bool contractStillValid(const ViewContract& contract, bool ownsVisibility = false);
bool matchesValidatedMarkerConfiguration(const winprobe::Report& report, const ViewContract& contract,
                                         UINT previewDpi);
int startupCalibrationPixels(const winprobe::Report& report, const ViewContract& contract, UINT previewDpi);
SceneSetup makePlaybackScene(const winprobe::Report& report, const std::vector<Vec2>& anchors, Vec2 workArea,
                             int iconPixels, unsigned dpi, int manualOffsetYPixels = 0);
// Pointer input is rejected until an explicit per-run interaction transition.
class PreviewPlayback {
   public:
    explicit PreviewPlayback(SceneSetup setup);
    void startGravity(bool staticCopiesConfirmed);
    bool tick(double seconds);
    void togglePause();
    void reset();
    void enterInteraction(bool playbackConfirmed);
    void leaveInteraction() noexcept;
    void pointer(const InputEvent& event);
    Playground* game() const { return game_.get(); }
    void commitMigration(std::unique_ptr<Playground> candidate) {
        if (!candidate || !game_) throw std::logic_error("invalid migration commit");
        game_ = std::move(candidate);
        interacting_ = false;
    }
    bool interacting() const { return interacting_; }
    void stop() noexcept;
    const DrawList& drawList() const;
    bool gravityStarted() const { return game_ != nullptr; }
    bool idle() const { return !game_ || game_->idle(); }
    bool paused() const { return game_ && game_->paused(); }
    bool grabbing() const { return game_ && game_->mode().stats().grabbing; }

   private:
    SceneSetup setup_;
    DrawList static_;
    std::unique_ptr<Playground> game_;
    bool interacting_ = false;
};
// Copies/playback only on the measured marker configuration. Other environments
// remain marker-only. Gravity additionally requires this snapshot's static check.
int runMarkers(const std::wstring& newReport, bool protectedMode = false, bool startupExperiment = false,
               std::string* outStage = nullptr, HRESULT* outHr = nullptr);
}  // namespace dpp::winmirror
