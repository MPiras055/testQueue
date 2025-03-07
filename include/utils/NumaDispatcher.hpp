#pragma once

#include <vector>
#include <thread>
#include <string>
#include <numa.h>
#include <numeric>

class Dispatcher {
private:
    std::vector<int> logical_core_map;

    bool load_topology(std::string& file);

public:

    /**
     * @brief Class constructor
     * 
     * @par unit cache_level: specify the cache level to optimize access to
     * @par topology_file: specify the file to load the core map from
     */
    Dispatcher(uint target_cache_level, std::string &topology_file);
        
    ~Dispatcher();

    static void bind_thread_to_core(std::thread& thread, int core_id);

    void dispatch_threads(const std::vector<std::thread>&);

    //dispatches threads from each group considering their ratio
    void dispatch_threads(std::vector<std::thread>&, std::vector<std::thread>&);
    
};