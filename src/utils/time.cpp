#include <vector>
#include <chrono>
#include <iostream>
#include <algorithm>
#include <numeric>
#include "AdditionalWork.hpp"

#define CONVERSION_FACTOR 648 // 100 ns ~= 648 ticks

int main(int argc, char **argv) {
    if (argc != 5) {
        std::cerr << "Usage: " << argv[0] << " <desired-nsecs> <tolerance> <run_count> <checks>" << std::endl;
        return 1;
    }

    const uint64_t desired_center = std::stoull(argv[1]);
    const uint64_t tolerance = std::stoull(argv[2]);
    const uint64_t runCount = std::stoull(argv[3]);
    const int64_t max_checks = std::stoull(argv[4]);

    if(desired_center < 100){
        std::cerr << "Warning: Desired center is too low,[ < 100 ]" << std::endl;
        return 1;
    }
    uint64_t current_center = desired_center/100ull * CONVERSION_FACTOR;
    uint64_t current_amplitude = current_center / 2;
    int64_t current_checks = 0;

    while (true) {
        std::vector<uint64_t> measurements(runCount);

        // Measure and store measurements
        for (uint64_t i = 0; i < runCount; i++) {
            auto start = std::chrono::high_resolution_clock::now();
            random_work(current_center, current_amplitude);
            auto end = std::chrono::high_resolution_clock::now();
            measurements[i] = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
        }

        // Calculate mean
        uint64_t mean = std::accumulate(measurements.begin(), measurements.end(), 0ull) / runCount;

        // Adjust parameters
        if (mean < (desired_center - tolerance)) {
            current_center += current_center / 2;
            current_amplitude += current_amplitude / 2;
            current_checks = 0;
        } else if (mean > (desired_center + tolerance)) {
            current_center -= current_center / 2;
            current_amplitude -= current_amplitude / 2;
            current_checks = 0;
        } else if (++current_checks >= max_checks) {
            break;
        }
    }

    std::cout << current_center << "\n" << current_amplitude << std::endl;
    return 0;
}