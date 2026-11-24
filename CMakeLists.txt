cmake_minimum_required(VERSION 3.25)

# Set the default before project() initializes this cache entry from the host OS.
if(CMAKE_HOST_APPLE AND NOT DEFINED CMAKE_OSX_DEPLOYMENT_TARGET)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "13.0" CACHE STRING "Minimum macOS")
endif()

project(DesktopPhysicsPlayground
    VERSION 1.0.0
    DESCRIPTION "DeskFling - Desktop Physics Playground"
    LANGUAGES C CXX)

if(APPLE)
    enable_language(OBJCXX)
endif()

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------
option(DPP_BUILD_TESTS "Build GoogleTest unit/session tests" ON)
option(DPP_BUILD_APPS "Build native sandbox apps and platform probes" ON)
option(DPP_BUILD_BENCHMARKS "Build the headless physics benchmark" ON)
option(DPP_WARNINGS_AS_ERRORS "Treat project warnings as errors (third-party code excluded)" OFF)
option(DPP_SANITIZE "Enable AddressSanitizer + UBSan on project targets (Clang/GCC, Debug use)" OFF)
set(DPP_GTEST_LOCAL_ARCHIVE "${CMAKE_CURRENT_SOURCE_DIR}/third_party/archives/googletest-1.15.2.tar.gz"
    CACHE FILEPATH "Optional offline mirror of the pinned GoogleTest archive (hash-verified)")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ---------------------------------------------------------------------------
# Third-party: Box2D v3.1.1 (vendored, unmodified, commit 8c661469c9507d3ad6fbd2fea3f1aa71669c2fe3)
# Box2D is C17; its own CMakeLists sets C_STANDARD 17 on the box2d target.
# ---------------------------------------------------------------------------
set(BUILD_SHARED_LIBS OFF)
add_subdirectory(third_party/box2d EXCLUDE_FROM_ALL)

# ---------------------------------------------------------------------------
# Common warning / sanitizer settings for project-owned targets only
# ---------------------------------------------------------------------------
add_library(dpp_project_options INTERFACE)
if(MSVC)
    target_compile_options(dpp_project_options INTERFACE /W4 /permissive- /utf-8 /EHsc)
    target_compile_definitions(dpp_project_options INTERFACE NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
    if(DPP_WARNINGS_AS_ERRORS)
        target_compile_options(dpp_project_options INTERFACE /WX)
    endif()
else()
    target_compile_options(dpp_project_options INTERFACE
        -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wno-sign-conversion)
    if(DPP_WARNINGS_AS_ERRORS)
        target_compile_options(dpp_project_options INTERFACE -Werror)
    endif()
    if(DPP_SANITIZE)
        target_compile_options(dpp_project_options INTERFACE -fsanitize=address,undefined -fno-omit-frame-pointer)
        target_link_options(dpp_project_options INTERFACE -fsanitize=address,undefined)
        target_compile_options(box2d PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
    endif()
endif()
# NOTE: no -ffast-math / /fp:fast. Numerical behaviour must stay predictable.

add_subdirectory(core)

if(DPP_BUILD_APPS)
    add_subdirectory(apps/physics_sandbox)
    if(WIN32)
        add_subdirectory(platforms/windows)
    elseif(APPLE)
        add_subdirectory(platforms/macos)
    endif()
endif()

if(DPP_BUILD_BENCHMARKS)
    add_subdirectory(benchmarks)
endif()

if(DPP_BUILD_TESTS)
    include(FetchContent)
    set(_dpp_gtest_urls "https://github.com/google/googletest/archive/refs/tags/v1.15.2.tar.gz")
    if(EXISTS "${DPP_GTEST_LOCAL_ARCHIVE}")
        set(_dpp_gtest_urls "${DPP_GTEST_LOCAL_ARCHIVE}")
    endif()
    FetchContent_Declare(googletest
        URL ${_dpp_gtest_urls}
        URL_HASH SHA256=7b42b4d6ed48810c5362c265a17faebe90dc2373c885e5216439d37927f02926
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)
    enable_testing()
    add_subdirectory(tests)
endif()

install(FILES LICENSE README.md THIRD_PARTY_NOTICES.md DESTINATION .)
install(FILES third_party/box2d/LICENSE DESTINATION licenses RENAME Box2D-LICENSE)
install(DIRECTORY docs/ DESTINATION docs
    PATTERN "evidence" EXCLUDE
    PATTERN "assets" EXCLUDE
    PATTERN ".DS_Store" EXCLUDE
    PATTERN "*-history-*" EXCLUDE)
install(DIRECTORY third_party/notices/ DESTINATION licenses)
set(CPACK_PACKAGE_NAME "DeskFling")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_GENERATOR "ZIP")
include(CPack)
