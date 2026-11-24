add_executable(desktop_probe probe.mm)
target_link_libraries(desktop_probe PRIVATE dpp_project_options "-framework AppKit" "-framework ApplicationServices" "-framework CoreServices" "-framework QuartzCore")
target_include_directories(desktop_probe PRIVATE ${PROJECT_SOURCE_DIR})
target_compile_options(desktop_probe PRIVATE -fobjc-arc)
set_target_properties(desktop_probe PROPERTIES OBJCXX_STANDARD 20)
install(TARGETS desktop_probe RUNTIME DESTINATION bin)

# Explicit synthetic document preview check, never reads user desktop files.
add_executable(document_thumbnail_smoke ${PROJECT_SOURCE_DIR}/tests/macos_document_thumbnail_smoke.mm)
target_include_directories(document_thumbnail_smoke PRIVATE ${PROJECT_SOURCE_DIR})
target_link_libraries(document_thumbnail_smoke PRIVATE "-framework AppKit" "-framework ImageIO"
  "-framework UniformTypeIdentifiers" "-framework QuickLookThumbnailing")
target_compile_options(document_thumbnail_smoke PRIVATE -fobjc-arc)
set_target_properties(document_thumbnail_smoke PROPERTIES OBJCXX_STANDARD 20 OBJCXX_STANDARD_REQUIRED YES)

# Explicit GUI fixture, intentionally NOT registered with ctest.
add_executable(mirror_native_smoke ${PROJECT_SOURCE_DIR}/tests/macos_mirror_native_smoke.mm mirror_preview.mm desktop_access.mm)
target_link_libraries(mirror_native_smoke PRIVATE "-framework AVFoundation" "-framework Metal" "-framework Security" dpp_core "-framework AppKit" "-framework ImageIO" "-framework UniformTypeIdentifiers" "-framework QuartzCore" "-framework ApplicationServices" "-framework CoreServices")
target_compile_options(mirror_native_smoke PRIVATE -fobjc-arc)
target_compile_definitions(mirror_native_smoke PRIVATE DPP_BUILD_ID="synthetic-fixture-build" DPP_BUILD_CONFIG="$<CONFIG>" DPP_MIRROR_FAULT_TESTING=1)
set_target_properties(mirror_native_smoke PROPERTIES OBJCXX_STANDARD 20 OBJCXX_STANDARD_REQUIRED YES)

add_executable(mirror_label_benchmark ${PROJECT_SOURCE_DIR}/tests/macos_mirror_label_benchmark.mm mirror_preview.mm)
target_link_libraries(mirror_label_benchmark PRIVATE "-framework AVFoundation" "-framework Metal" dpp_core "-framework AppKit" "-framework QuartzCore" "-framework CoreServices")
target_compile_options(mirror_label_benchmark PRIVATE -fobjc-arc)
target_compile_definitions(mirror_label_benchmark PRIVATE DPP_BUILD_ID="label-benchmark-fixture" DPP_BUILD_CONFIG="$<CONFIG>" DPP_MIRROR_FAULT_TESTING=1)
set_target_properties(mirror_label_benchmark PROPERTIES OBJCXX_STANDARD 20 OBJCXX_STANDARD_REQUIRED YES)

# Explicit synthetic input-handler fixture; never registered with ctest.
add_executable(sandbox_drag_smoke ${PROJECT_SOURCE_DIR}/tests/macos_sandbox_drag_smoke.mm mirror_preview.mm desktop_access.mm)
target_link_libraries(sandbox_drag_smoke PRIVATE "-framework AVFoundation" "-framework Metal" "-framework Security" dpp_core "-framework AppKit" "-framework ImageIO" "-framework UniformTypeIdentifiers" "-framework QuartzCore" "-framework ApplicationServices" "-framework CoreServices")
target_compile_options(sandbox_drag_smoke PRIVATE -fobjc-arc)
target_compile_definitions(sandbox_drag_smoke PRIVATE DPP_BUILD_ID="synthetic-drag-fixture" DPP_BUILD_CONFIG="$<CONFIG>")
set_target_properties(sandbox_drag_smoke PROPERTIES OBJCXX_STANDARD 20 OBJCXX_STANDARD_REQUIRED YES)

# Opt-in compatibility experiment; never started by a build or unit test.
add_executable(desktop_protected_probe protected_session.mm)
target_link_libraries(desktop_protected_probe PRIVATE dpp_core "-framework AppKit" "-framework CoreGraphics" "-framework CoreServices")
target_compile_options(desktop_protected_probe PRIVATE -fobjc-arc)
set_target_properties(desktop_protected_probe PROPERTIES OBJCXX_STANDARD 20 OBJCXX_STANDARD_REQUIRED YES)
# Purpose string for the separate, manually invoked authorization request.
target_link_options(desktop_protected_probe PRIVATE "LINKER:-sectcreate,__TEXT,__info_plist,${CMAKE_CURRENT_SOURCE_DIR}/protected_probe_info.plist")
set_property(TARGET desktop_protected_probe APPEND PROPERTY LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/protected_probe_info.plist")
add_custom_command(TARGET desktop_protected_probe POST_BUILD
  COMMAND /usr/bin/codesign --force --sign - --identifier org.desktopphysicsplayground.protected-probe "$<TARGET_FILE:desktop_protected_probe>"
  VERBATIM)

# User-requested one-off post-authorization comparison. Production/legacy entry gates stay intact.
add_executable(desktop_windowmanager_recheck protected_session.mm)
target_link_libraries(desktop_windowmanager_recheck PRIVATE dpp_core "-framework AppKit" "-framework CoreGraphics" "-framework CoreServices")
target_compile_options(desktop_windowmanager_recheck PRIVATE -fobjc-arc)
target_compile_definitions(desktop_windowmanager_recheck PRIVATE DPP_WINDOWMANAGER_AUTHORIZED_RECHECK=1)
set_target_properties(desktop_windowmanager_recheck PROPERTIES OBJCXX_STANDARD 20 OBJCXX_STANDARD_REQUIRED YES)
target_link_options(desktop_windowmanager_recheck PRIVATE "LINKER:-sectcreate,__TEXT,__info_plist,${CMAKE_CURRENT_SOURCE_DIR}/protected_probe_info.plist")
set_property(TARGET desktop_windowmanager_recheck APPEND PROPERTY LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/protected_probe_info.plist")
add_custom_command(TARGET desktop_windowmanager_recheck POST_BUILD
  COMMAND /usr/bin/codesign --force --sign - --identifier org.desktopphysicsplayground.protected-probe "$<TARGET_FILE:desktop_windowmanager_recheck>"
  VERBATIM)

# Separate manual full desktop entry; shares the proved Finder recovery executor.
add_executable(desktop_protected_mirror protected_session.mm protected_mirror.mm mirror_preview.mm)
target_link_libraries(desktop_protected_mirror PRIVATE "-framework AVFoundation" "-framework Metal" dpp_core "-framework AppKit" "-framework CoreServices"
  "-framework CoreGraphics" "-framework QuartzCore" "-framework ImageIO" "-framework UniformTypeIdentifiers" "-framework QuickLookThumbnailing")
target_compile_options(desktop_protected_mirror PRIVATE -fobjc-arc)
target_compile_definitions(desktop_protected_mirror PRIVATE DPP_PROTECTED_MIRROR=1 DPP_BUILD_CONFIG="$<CONFIG>")
file(GLOB_RECURSE _protected_sources CONFIGURE_DEPENDS
  "${PROJECT_SOURCE_DIR}/platforms/display_migration.h" "${PROJECT_SOURCE_DIR}/platforms/mirror_hit.h" "${PROJECT_SOURCE_DIR}/platforms/filename_geometry.h" "${PROJECT_SOURCE_DIR}/core/*.h" "${PROJECT_SOURCE_DIR}/core/*.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/*.h" "${CMAKE_CURRENT_SOURCE_DIR}/*.mm" "${CMAKE_CURRENT_SOURCE_DIR}/*.inc")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${_protected_sources})
set(_protected_identity "Box2D-v3.1.1;protected-finder-mirror-v1")
foreach(_input IN LISTS _protected_sources)
  file(SHA256 "${_input}" _hash)
  string(APPEND _protected_identity "${_hash}")
endforeach()
string(SHA256 _protected_build_id "${_protected_identity}")
target_compile_definitions(desktop_protected_mirror PRIVATE DPP_BUILD_ID="${_protected_build_id}")
set_target_properties(desktop_protected_mirror PROPERTIES OBJCXX_STANDARD 20 OBJCXX_STANDARD_REQUIRED YES)
target_link_options(desktop_protected_mirror PRIVATE "LINKER:-sectcreate,__TEXT,__info_plist,${CMAKE_CURRENT_SOURCE_DIR}/protected_probe_info.plist")
set_property(TARGET desktop_protected_mirror APPEND PROPERTY LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/protected_probe_info.plist")
add_custom_command(TARGET desktop_protected_mirror POST_BUILD
  COMMAND /usr/bin/codesign --force --sign - --identifier org.desktopphysicsplayground.protected-probe "$<TARGET_FILE:desktop_protected_mirror>"
  VERBATIM)
