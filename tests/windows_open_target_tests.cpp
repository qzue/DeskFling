#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>

#include "platforms/windows/open_target.h"

using namespace dpp::winmirror;

TEST(WindowsOpen, ReleasesBeforeValidationAndSubmitsOnlyOncePerSelection) {
    OpenRequest request;
    OpenTarget target;
    target.path = L"synthetic target";
    const auto token = request.queue(target);
    EXPECT_NE(token, 0u);
    EXPECT_EQ(request.queue(target), 0u);
    std::vector<int> order;
    auto active = [] { return true; };
    auto release = [&] {
        order.push_back(1);
        return true;
    };
    auto valid = [&](const OpenTarget& t) {
        order.push_back(2);
        return t.path == target.path;
    };
    auto submit = [&](const OpenTarget&) {
        order.push_back(3);
        EXPECT_EQ(request.queue(target), 0u);  // Shell's nested message loop cannot queue another open.
        EXPECT_EQ(request.dispatch(token, active, release, valid, [](const auto&) { return true; }),
                  OpenResult::ignored);
        return true;
    };
    EXPECT_EQ(request.dispatch(token, active, release, valid, submit), OpenResult::accepted);
    EXPECT_EQ(order, (std::vector<int>{1, 2, 3}));
    EXPECT_EQ(request.dispatch(token, active, release, valid, submit), OpenResult::ignored);
    const auto second = request.queue(target);
    EXPECT_NE(second, token);
    EXPECT_EQ(request.dispatch(token, active, release, valid, submit), OpenResult::ignored);
    EXPECT_EQ(request.dispatch(second, active, release, valid, submit), OpenResult::accepted);
    EXPECT_EQ(order.size(), 6u);
}

TEST(WindowsOpen, CancelTimeoutInvalidTargetAndFailedReleaseNeverSubmit) {
    OpenRequest request;
    OpenTarget target;
    auto yes = [] { return true; };
    auto valid = [](const auto&) { return true; };
    auto forbidden = [](const auto&) {
        ADD_FAILURE() << "unexpected real-open substitute";
        return true;
    };
    auto token = request.queue(target);
    request.cancel();
    EXPECT_EQ(request.dispatch(token, yes, yes, valid, forbidden), OpenResult::ignored);
    token = request.queue(target);
    EXPECT_EQ(request.dispatch(token, [] { return false; }, yes, valid, forbidden), OpenResult::cancelled);
    token = request.queue(target);
    EXPECT_EQ(request.dispatch(token, yes, [] { return false; }, valid, forbidden), OpenResult::cancelled);
    token = request.queue(target);
    EXPECT_EQ(
        request.dispatch(token, yes, yes, [](const auto&) { return false; }, forbidden), OpenResult::invalid);
    bool alive = true;
    token = request.queue(target);
    EXPECT_EQ(request.dispatch(
                  token, [&] { return alive; },
                  [&] {
                      alive = false;
                      return true;
                  },
                  valid, forbidden),
              OpenResult::cancelled);
    token = request.queue(target);
    EXPECT_EQ(request.dispatch(token, yes, yes, valid, [](const auto&) { return false; }),
              OpenResult::rejected);
    EXPECT_FALSE(request.busy());
}

class WindowsOpenFiles : public testing::Test {
   protected:
    HRESULT com = E_FAIL;
    std::filesystem::path dir;
    void SetUp() override {
        com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        ASSERT_TRUE(SUCCEEDED(com));
        GUID guid{};
        ASSERT_TRUE(SUCCEEDED(CoCreateGuid(&guid)));
        wchar_t id[40]{};
        ASSERT_GT(StringFromGUID2(guid, id, 40), 0);
        dir = std::filesystem::temp_directory_path() / (std::wstring(L"DPP-open-test-") + id);
        ASSERT_TRUE(std::filesystem::create_directory(dir));
    }
    void TearDown() override {
        if (!dir.empty()) std::filesystem::remove_all(dir);
        if (SUCCEEDED(com)) CoUninitialize();
    }
    void link(const std::filesystem::path& path, const std::filesystem::path& target,
              const wchar_t* args = L"", bool runAs = false) {
        Microsoft::WRL::ComPtr<IShellLinkW> shellLink;
        ASSERT_TRUE(SUCCEEDED(
            CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&shellLink))));
        ASSERT_TRUE(SUCCEEDED(shellLink->SetPath(target.c_str())));
        ASSERT_TRUE(SUCCEEDED(shellLink->SetArguments(args)));
        if (runAs) {
            Microsoft::WRL::ComPtr<IShellLinkDataList> data;
            ASSERT_TRUE(SUCCEEDED(shellLink.As(&data)));
            DWORD flags = 0;
            ASSERT_TRUE(SUCCEEDED(data->GetFlags(&flags)));
            ASSERT_TRUE(SUCCEEDED(data->SetFlags(flags | SLDF_RUNAS_USER)));
        }
        Microsoft::WRL::ComPtr<IPersistFile> file;
        ASSERT_TRUE(SUCCEEDED(shellLink.As(&file)));
        ASSERT_TRUE(SUCCEEDED(file->Save(path.c_str(), TRUE)));
    }
};

TEST_F(WindowsOpenFiles, FileAndFolderBindingsRejectReplacedIdentity) {
    const auto file = dir / L"space 中文.json";
    {
        std::ofstream out(file);
        out << "{}";
    }
    auto target = bindOpenPath(file.wstring(), false);
    auto folder = bindOpenPath(dir.wstring(), false);
    ASSERT_TRUE(target);
    ASSERT_TRUE(folder);
    EXPECT_TRUE(validOpenTarget(*target));
    EXPECT_TRUE(validOpenTarget(*folder));
    // Keep old object alive under a different name, preventing file-ID reuse in the fixture.
    std::filesystem::rename(file, dir / L"old.json");
    {
        std::ofstream out(file);
        out << "{}";
    }
    EXPECT_FALSE(validOpenTarget(*target));
    EXPECT_TRUE(validOpenTarget(*folder));
}

TEST_F(WindowsOpenFiles, SimpleLinkRetainsSourceAndDestinationIdentity) {
    const auto destination = dir / L"folder with spaces";
    ASSERT_TRUE(std::filesystem::create_directory(destination));
    const auto source = dir / L"sample.lnk";
    link(source, destination);
    auto target = bindOpenPath(source.wstring(), true);
    ASSERT_TRUE(target);
    EXPECT_TRUE(std::filesystem::equivalent(target->path, destination));
    EXPECT_TRUE(validOpenTarget(*target));
    // Source mutation is rejected even if the original destination still exists.
    link(source, dir);
    EXPECT_FALSE(validOpenTarget(*target));
    link(source, destination);
    target = bindOpenPath(source.wstring(), true);
    ASSERT_TRUE(target);
    std::filesystem::rename(destination, dir / L"old-folder");
    ASSERT_TRUE(std::filesystem::create_directory(destination));
    EXPECT_FALSE(validOpenTarget(*target));
}

TEST_F(WindowsOpenFiles, LinksWithArgumentsElevationOrMissingDestinationAreDisabled) {
    const auto source = dir / L"sample.lnk";
    link(source, dir, L"--anything");
    EXPECT_FALSE(bindOpenPath(source.wstring(), true));
    link(source, dir, L"", true);
    EXPECT_FALSE(bindOpenPath(source.wstring(), true));
    link(source, dir / L"missing");
    EXPECT_FALSE(bindOpenPath(source.wstring(), true));
    EXPECT_FALSE(bindOpenTarget(nullptr, nullptr));
}

#include "platforms/mirror_hit.h"
TEST(WindowsOpen, HitUsesMovedRotatedTopmostSpriteAndIgnoresLabels) {
    dpp::DrawList draw;
    draw.add(dpp::SpriteItem{dpp::TextureId{1}, {300, 400}, {80, 20}, 1.5707963f});
    EXPECT_EQ(dpp::mirrorHit(draw, {300, 430}), dpp::TextureId{1});
    EXPECT_EQ(dpp::mirrorHit(draw, {330, 400}), dpp::kNoTexture);
    draw.addText({300, 430}, "display name is not identity");
    EXPECT_EQ(dpp::mirrorHit(draw, {300, 430}), dpp::TextureId{1});
    draw.add(dpp::SpriteItem{dpp::TextureId{2}, {300, 430}, {20, 20}});
    EXPECT_EQ(dpp::mirrorHit(draw, {300, 430}), dpp::TextureId{2});
    EXPECT_EQ(dpp::mirrorHit(draw, {10, 10}), dpp::kNoTexture);
}
