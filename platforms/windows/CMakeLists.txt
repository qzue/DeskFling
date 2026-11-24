add_executable(desktop_probe probe.cpp probe_samples.cpp)
target_link_libraries(desktop_probe PRIVATE dpp_project_options ole32 oleaut32 shell32 user32 gdi32 uuid shcore advapi32 msimg32)
install(TARGETS desktop_probe RUNTIME DESTINATION bin)

add_library(dpp_windows_protected STATIC protected_session.cpp)
target_link_libraries(dpp_windows_protected PUBLIC dpp_core ole32 oleaut32 shell32 user32
    gdi32 uuid shcore advapi32 msimg32)
add_library(dpp_windows_mirror STATIC mirror_preview.cpp billiards_lan.cpp)
target_link_libraries(dpp_windows_mirror PUBLIC dpp_windows_protected dpp_core d3d11 d3dcompiler dxgi d2d1 dcomp dwrite
    comctl32 ole32 oleaut32 shell32 user32 gdi32 uuid shcore advapi32 msimg32 windowscodecs winmm ws2_32)
add_executable(desktop_mirror_preview mirror_main.cpp probe_samples.cpp)
target_link_libraries(desktop_mirror_preview PRIVATE dpp_windows_mirror)
install(TARGETS desktop_mirror_preview RUNTIME DESTINATION bin)

# Independent opt-in visibility audit/experiment and out-of-process recovery.
add_executable(desktop_protected_session protected_main.cpp probe_samples.cpp)
target_link_libraries(desktop_protected_session PRIVATE dpp_windows_protected)
install(TARGETS desktop_protected_session RUNTIME DESTINATION bin)

add_executable(desktop_protected_mirror WIN32 protected_mirror_main.cpp probe_samples.cpp app_resources.rc)
target_link_libraries(desktop_protected_mirror PRIVATE dpp_windows_mirror)
if(MSVC)
    target_link_options(desktop_protected_mirror PRIVATE "/ENTRY:wmainCRTStartup")
endif()
install(TARGETS desktop_protected_mirror RUNTIME DESTINATION bin)

# Manual, explicitly authorized native fault scenarios. Never registered with CTest.
if(DPP_BUILD_TESTS)
    add_executable(desktop_recovery_test_runner protected_main.cpp protected_session.cpp probe_samples.cpp)
    target_compile_definitions(desktop_recovery_test_runner PRIVATE DPP_RECOVERY_TEST_HOOKS)
    target_link_libraries(desktop_recovery_test_runner PRIVATE dpp_core ole32 oleaut32 shell32 user32
        gdi32 uuid shcore advapi32 msimg32)
endif()

# Self-window message dispatch only; no real desktop or protected backend starts.
if(DPP_BUILD_TESTS)
    add_executable(windows_direct_native_tests ${PROJECT_SOURCE_DIR}/tests/windows_direct_native_tests.cpp billiards_lan.cpp probe_samples.cpp)
    target_compile_definitions(windows_direct_native_tests PRIVATE DPP_DIRECT_INPUT_TESTS=1)
    target_link_libraries(windows_direct_native_tests PRIVATE GTest::gtest_main dpp_windows_protected dpp_core
        d3d11 d3dcompiler dxgi d2d1 dcomp dwrite comctl32 ole32 oleaut32 shell32 user32 gdi32 uuid shcore advapi32 msimg32 windowscodecs winmm ws2_32)
endif()
