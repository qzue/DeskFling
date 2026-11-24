# Third-party notices and provenance

## Box2D

- Author: Erin Catto; MIT license, full text in `third_party/box2d/LICENSE`.
- Repository: https://github.com/erincatto/box2d
- Version **v3.1.1**, commit **8c661469c9507d3ad6fbd2fea3f1aa71669c2fe3**.
- Vendored subset: `include/`, `src/`, top-level `CMakeLists.txt`, `README.md`, `LICENSE`.
- On 2026-11-11 compared local include/src recursively and top-level CMake with a fresh checkout of the tag: no differences. The subset excludes upstream samples/tests/docs, not engine code.
- Compiled as C17; our C++20 wrapper uses only its handle-based C API. Bundled license is installed with the sandbox.

## GoogleTest

- Repository: https://github.com/google/googletest
- Version **v1.15.2**, archive SHA256 **7b42b4d6ed48810c5362c265a17faebe90dc2373c885e5216439d37927f02926**.
- BSD 3-Clause license, full text copied from the verified archive into `third_party/notices/GoogleTest-LICENSE`.
- Build/test dependency only, not linked into or shipped with the native application.
- Optional offline archive: `third_party/archives/googletest-1.15.2.tar.gz`; otherwise downloaded from the fixed tag with hash verification.

## Desktop Icon Billiards (reference only)

- Repository: https://github.com/liuxiaogang-com/desktop-icon-billiards
- Audited commit **187e5e89f6ab76ede0d1fe22246477b0cfaf0541**.
- MIT, Copyright (c) 2025-2026 LXG. Full license retained in `third_party/notices/desktop-icon-billiards-LICENSE`.
- No upstream Python source/assets are included in the product or translated verbatim. Shell API selection, gameplay scope, and recovery concerns were reviewed as references. See `docs/upstream-audit.md` for per-symbol decisions.
- The audit checkout stayed in `/tmp`; it was not executed against the user's desktop.

## Native system resources

AppKit, Core Animation, Win32, Direct2D and DirectWrite are operating-system SDK dependencies. macOS sandbox images are generated in memory using system symbols; Windows uses synthetic geometry. Neither host ships the user's desktop content, screenshots, wallpaper or third-party icon packs.

Project-private `.tools/` contains CMake 3.31.6 and Ninja 1.11.1.3 installed from PyPI for this build. It is ignored and never packaged. Python is a build bootstrap convenience, not a product dependency.
