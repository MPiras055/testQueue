#include <vector>
#include <chrono>
#include <iostream>
#include <numeric>

int main(int argc, char **argv){
    if(argc != 4){
        std::cerr << "Usage: " << argv[0] << " <length> <factor> <run_count>" << std::endl;
        return 1;
    }

    size_t length = std::stoul(argv[1]);
    size_t factor = std::stoul(argv[2]);
    size_t runCount  = std::stoul(argv[3]);
    std::vector<size_t> data(length,factor);
    std::vector<long double> times(runCount);
    
    for(size_t j = 0; j < runCount; j++){
        
        auto start = std::chrono::high_resolution_clock::now();
        for(int i = 0; i < length; i++){
            data[i] *= factor;
        }
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