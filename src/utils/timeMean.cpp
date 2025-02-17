#include <vector>
#include <chrono>
#include <iostream>
#include "AdditionalWork.hpp"

int main(int argc, char **argv){
    if(argc != 3){
        std::cerr << "Usage: " << argv[0] << " <mean> <run_count>" << std::endl;
        return 1;
    }

    size_t meanTime = std::stoul(argv[1]);
    size_t runCount  = std::stoul(argv[2]);
    std::vector<long double> times(runCount);
    
    for(size_t j = 0; j < runCount; j++){
        auto start = std::chrono::high_resolution_clock::now();
        random_work((double) meanTime);
        auto stop = std::chrono::high_resolution_clock::now();
        times[j] = std::chrono::duration_cast<std::chrono::nanoseconds>(stop - start).count();
    }

    long double mean = 0;
    for(int i = 0; i < runCount; i++){
        mean += times[i];
    }

    std::cout << "Mean: " << mean/runCount << std::endl;

    return 0;
}