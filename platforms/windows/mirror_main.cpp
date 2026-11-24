#include <iostream>

#include "mirror_preview.h"

int wmain(int argc, wchar_t** argv) {
    if (argc == 2 && std::wstring(argv[1]) == L"--help") {
        std::cout << "desktop_mirror_preview --report <NEW.json>\n"
                     "Read-only desktop mirror DEVELOPMENT PREVIEW: original icons remain.\n"
                     "Starts with markers. On the validated profile, explicitly confirm markers for copies,\n"
                     "then confirm static copies for shared gravity playback.\n"
                     "Interaction requires a separate Enter interaction click after playback confirmation.\n"
                     "Default: no mouse capture. Focus loss exits interaction. Icon size: measured system "
                     "image-list pixels.\n"
                     "Normal logged-in desktop required. Stop panel + 90-second automatic overlay stop.\n"
                     "Panel remains for saving results after the overlay automatically stops.\n"
                     "Report + initialization sidecar omit names, images and private paths; no overwrites.\n";
        return 0;
    }
    if (argc != 3 || std::wstring(argv[1]) != L"--report" || !*argv[2]) return 2;
    try {
        return dpp::winmirror::runMarkers(argv[2]);
    } catch (...) {
        std::cerr << "preview_failed_before_report; no desktop changes performed\n";
        return 1;
    }
}
