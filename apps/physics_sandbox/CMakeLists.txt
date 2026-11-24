if(APPLE)
    add_executable(physics_sandbox MACOSX_BUNDLE ${PROJECT_SOURCE_DIR}/platforms/macos/sandbox.mm
        ${PROJECT_SOURCE_DIR}/platforms/macos/desktop_access.mm
        ${PROJECT_SOURCE_DIR}/platforms/macos/mirror_preview.mm)
    target_link_libraries(physics_sandbox PRIVATE "-framework AVFoundation" "-framework Metal" dpp_core "-framework AppKit" "-framework ImageIO" "-framework UniformTypeIdentifiers" "-framework QuartzCore" "-framework ApplicationServices" "-framework CoreServices" "-framework Security")
    # Content identity for diagnostics; real product signing remains a separate release concern.
    file(GLOB_RECURSE _dpp_identity_sources CONFIGURE_DEPENDS
        "${PROJECT_SOURCE_DIR}/core/*.h" "${PROJECT_SOURCE_DIR}/core/*.cpp"
        "${PROJECT_SOURCE_DIR}/platforms/macos/*.h" "${PROJECT_SOURCE_DIR}/platforms/macos/*.mm"
        "${PROJECT_SOURCE_DIR}/platforms/macos/*.in")
    list(APPEND _dpp_identity_sources "${CMAKE_CURRENT_LIST_FILE}")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${_dpp_identity_sources})
    set(_dpp_identity "Box2D-v3.1.1")
    foreach(_input IN LISTS _dpp_identity_sources)
        file(SHA256 "${_input}" _hash)
        string(APPEND _dpp_identity "${_hash}")
    endforeach()
    string(SHA256 _dpp_build_id "${_dpp_identity}")
    target_compile_definitions(physics_sandbox PRIVATE DPP_BUILD_ID="${_dpp_build_id}")
    # Local development: explicit ad-hoc identity, no Developer ID or notarization claim.
    # Retain existing non-hardened, non-App-Sandbox mode; no entitlements are needed in this mode.
    add_custom_command(TARGET physics_sandbox POST_BUILD
        COMMAND /usr/bin/codesign --force --sign - --identifier dev.dpp.sandbox
            "$<TARGET_BUNDLE_DIR:physics_sandbox>" VERBATIM)
    target_compile_options(physics_sandbox PRIVATE -fobjc-arc)
    target_compile_definitions(physics_sandbox PRIVATE DPP_BUILD_CONFIG="$<CONFIG>")
    set_target_properties(physics_sandbox PROPERTIES
        OBJCXX_STANDARD 20 OBJCXX_STANDARD_REQUIRED YES
        MACOSX_BUNDLE_BUNDLE_NAME "Desktop Physics Sandbox"
        MACOSX_BUNDLE_GUI_IDENTIFIER "dev.dpp.sandbox"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "0.1.0"
        MACOSX_BUNDLE_INFO_PLIST "${PROJECT_SOURCE_DIR}/platforms/macos/Info.plist.in")
    set_target_properties(physics_sandbox PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/platforms/macos")
    install(TARGETS physics_sandbox BUNDLE DESTINATION .)
elseif(WIN32)
    add_executable(physics_sandbox WIN32 ${PROJECT_SOURCE_DIR}/platforms/windows/sandbox.cpp)
    target_link_libraries(physics_sandbox PRIVATE dpp_core d2d1 dwrite user32 gdi32)
    set_target_properties(physics_sandbox PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/platforms/windows")
    install(TARGETS physics_sandbox RUNTIME DESTINATION bin)
endif()
