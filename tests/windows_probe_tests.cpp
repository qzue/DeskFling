#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <type_traits>

#include "platforms/windows/probe_samples.h"

using namespace dpp::winprobe;
namespace {
Sample fixture(const char* id, POINT p) {
    Sample s;
    s.id = id;
    s.localName = L"C:\\Users\\private-person\\Desktop\\private-name.txt";
    s.itemHr = s.createHr = s.nameHr = s.factoryHr = s.imageHr = S_OK;
    s.bitmap.reset(CreateBitmap(8, 8, 1, 32, nullptr));
    setPosition(s, S_OK, p);
    return s;
}
}  // namespace
TEST(WindowsProbe, ExplicitSampleArgumentsOnly) {
    EXPECT_EQ(parseOptions({}).samples, 0);
    EXPECT_TRUE(parseOptions({L"--help"}).help);
    auto options = parseOptions({L"--samples", L"5", L"--report", L"C:\\test folder\\new.json"});
    EXPECT_EQ(options.samples, 5);
    EXPECT_EQ(options.report, L"C:\\test folder\\new.json");
    EXPECT_THROW(parseOptions({L"--samples", L"0", L"--report", L"a"}), std::invalid_argument);
    EXPECT_THROW(parseOptions({L"--samples", L"3"}), std::invalid_argument);
    EXPECT_THROW(parseOptions({L"--report", L"a"}), std::invalid_argument);
    EXPECT_THROW(parseOptions({L"--force"}), std::invalid_argument);
    EXPECT_THROW(parseOptions({L"--samples", L"3", L"--report", L"a", L"--samples", L"4"}),
                 std::invalid_argument);
}
TEST(WindowsProbe, RequiresKnownMatchingInteractiveSession) {
    Environment e;
    EXPECT_FALSE(interactive(e));
    e.session = 7;
    e.shellSession = 7;
    e.shellWindow = reinterpret_cast<HWND>(1);
    e.stationVisible = true;
    e.inputDesktop = true;
    EXPECT_TRUE(interactive(e));
    e.inputDesktop = false;
    EXPECT_FALSE(interactive(e));
    e.inputDesktop = true;
    e.shellSession = 8;
    EXPECT_FALSE(interactive(e));
    e.shellSession = 7;
    e.stationVisible.reset();
    EXPECT_FALSE(interactive(e));
}
TEST(WindowsProbe, SampleAssociationSurvivesStorageOrderAndRedactsLocalData) {
    Report r;
    r.snapshot = "snapshot-fixture";
    r.samples.push_back(fixture("snapshot-fixture/S2", {300, 90}));
    r.samples.push_back(fixture("snapshot-fixture/S1", {-30, 40}));
    ASSERT_TRUE(r.samples[0].bitmap);
    ASSERT_TRUE(r.samples[1].bitmap);
    r.samples[1].bitmap.reset();
    r.samples[1].imageHr = E_ACCESSDENIED;
    r.samples[0].filesystemItem = false;  // virtual item does not require a path
    auto report = encodeReport(r);
    auto second = report.find("snapshot-fixture/S1");
    auto first =
        report.substr(report.find("snapshot-fixture/S2"), second - report.find("snapshot-fixture/S2"));
    EXPECT_NE(first.find("[300,90]"), std::string::npos);
    EXPECT_NE(first.find("\"image_read_success\":true"), std::string::npos);
    EXPECT_NE(first.find("\"filesystem_item\":false"), std::string::npos);
    EXPECT_NE(report.substr(second).find("[-30,40]"), std::string::npos);
    EXPECT_NE(report.substr(second).find("\"image_read_success\":false"), std::string::npos);
    EXPECT_EQ(report.find("private-person"), std::string::npos);
    EXPECT_EQ(report.find("private-name"), std::string::npos);
    EXPECT_EQ(report.find("C:"), std::string::npos);
}
TEST(WindowsProbe, FailedPositionDiscardsValueAndRetainsError) {
    Sample s = fixture("S1", {50, 60});
    setPosition(s, E_ACCESSDENIED, {777, 888});
    EXPECT_FALSE(s.position);
    Report r;
    r.samples.push_back(std::move(s));
    auto text = encodeReport(r);
    EXPECT_NE(text.find("\"position_hresult\":\"0x80070005\""), std::string::npos);
    EXPECT_NE(text.find("\"raw_position\":null"), std::string::npos);
    EXPECT_EQ(text.find("777"), std::string::npos);
    EXPECT_NE(text.find("\"sample_image_successes\":1"), std::string::npos);
}
TEST(WindowsProbe, ViewAnchorNeverBecomesScreenOrPhysicsPoint) {
    static_assert(!std::is_convertible_v<ShellItemPosition, POINT>);
    Report r;
    r.samples.push_back(fixture("S1", {-90, -2}));
    auto text = encodeReport(r);
    EXPECT_NE(text.find("[-90,-2]"), std::string::npos);
    EXPECT_NE(text.find("\"screen_position\":null"), std::string::npos);
    EXPECT_NE(text.find("\"client_position\":null"), std::string::npos);
    EXPECT_NE(text.find("\"scene_position\":null"), std::string::npos);
    EXPECT_NE(text.find("\"transforms_applied\":[]"), std::string::npos);
    EXPECT_NE(text.find("\"screen_alignment_verified\":false"), std::string::npos);
}
TEST(WindowsProbe, ConfirmationRequiresSavedValidDataAndUnchangedDisplay) {
    Report r;
    r.samples.push_back(fixture("S1", {0, 0}));
    r.samples[0].identity = r.samples[0].layout = Confirmation::Matches;
    EXPECT_EQ(encodeReport(r).find("user_reports_match"), std::string::npos);
    r.userSaved = true;
    EXPECT_NE(encodeReport(r).find("user_reports_match"), std::string::npos);
    r.displayChanged = true;
    EXPECT_EQ(encodeReport(r).find("user_reports_match"), std::string::npos);
    r.displayChanged = false;
    r.samples[0].nameHr = E_FAIL;
    setPosition(r.samples[0], E_FAIL, {});
    EXPECT_EQ(encodeReport(r).find("user_reports_match"), std::string::npos);
}
TEST(WindowsProbe, UnknownMeasurementsStayUnknown) {
    Report r;
    r.samples.emplace_back();
    auto text = encodeReport(r);
    EXPECT_NE(text.find("\"image_read_success\":null"), std::string::npos);
    EXPECT_NE(text.find("\"remote_session_metric\":null"), std::string::npos);
    for (const auto* key : {"session_id", "shell_session_id", "station_visible", "input_desktop",
                            "shell_view_dpi", "item_count", "shell_hresult"})
        EXPECT_NE(text.find(std::string("\"") + key + "\":null"), std::string::npos);
    EXPECT_EQ(text.find("user_reports_match"), std::string::npos);
}
TEST(WindowsProbe, ReportCreationNeverOverwritesExistingEvidence) {
    wchar_t directory[MAX_PATH]{}, temporary[MAX_PATH]{};
    ASSERT_NE(GetTempPathW(MAX_PATH, directory), 0u);
    ASSERT_NE(GetTempFileNameW(directory, L"dpp", 0, temporary), 0u);
    struct Cleanup {
        std::wstring path;
        ~Cleanup() { DeleteFileW(path.c_str()); }
    } cleanup{temporary};
    {
        std::ofstream file{std::filesystem::path(temporary)};
        file << "original evidence";
    }
    EXPECT_NE(writeNewReport(temporary, "replacement"), ERROR_SUCCESS);
    std::ifstream file{std::filesystem::path(temporary)};
    std::string contents((std::istreambuf_iterator<char>(file)), {});
    EXPECT_EQ(contents, "original evidence");
    file.close();
    ASSERT_TRUE(DeleteFileW(temporary));
    EXPECT_EQ(writeNewReport(temporary, "{\"fresh\":true}\n"), ERROR_SUCCESS);
}
