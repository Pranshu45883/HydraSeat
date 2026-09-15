#include "hydra/xinput_probe.hpp"

#include <iostream>
#include <string>
#include <vector>

#if defined(_WIN32)
int wmain(int argc, wchar_t* argv[]) {
    std::vector<std::wstring> args;
    args.reserve(argc > 1 ? static_cast<std::size_t>(argc - 1) : 0u);
    for (int i = 1; i < argc; ++i) args.emplace_back(argv[i]);

    const auto options = hydra::controller::probe::parseProbeArgs(args);
    if (!options) return 2;
    return hydra::controller::probe::runProbe(*options, std::cout);
}
#else
int main() {
    return 2;
}
#endif
