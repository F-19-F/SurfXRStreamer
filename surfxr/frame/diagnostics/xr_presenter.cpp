#include "presenter_private.hpp"
using namespace surfxr::diagnostics;
int main(int argc, char** argv) {
    if (argc == 1 || (argc == 2 && std::string(argv[1]) == "--help")) {
        std::cout << "Usage: frame_xr_presenter --input FILE.rgba --width W --height H --seconds 1..30\n"
                     "Explicitly starts an OpenXR session; displays one local tightly packed RGBA8 sRGB image.\n"
                     "Offline presentation diagnostic. No arguments or --help never create a session.\n";
        return 0;
    }
    try {
        const auto options = parse(argc, argv);
        auto rgba = read_rgba(options); // Validate the complete file before touching the runtime.
        std::signal(SIGINT, signal_handler);
        std::signal(SIGTERM, signal_handler);
        Presenter presenter(options);
        presenter.initialize(rgba);
        presenter.run();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "frame_xr_presenter: " << error.what() << '\n';
        return 1;
    }
}
