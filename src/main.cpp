#include "hydra/hardware_detector.hpp"

#include <iostream>

int main(int argc, char* argv[]) {
    (void)argc;
    (void)argv;

    std::cout << "Starting HydraSeat Engine v0.1.0..." << std::endl;

    hydra::HardwareDetector detector;
    detector.printReport();

    return 0;
}
