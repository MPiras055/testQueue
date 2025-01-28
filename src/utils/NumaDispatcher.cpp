#include "NumaDispatcher.hpp"
#include <fstream>
#include <stdexcept>
#include <sstream>
#include <filesystem>
#include <algorithm>
#include <utility>
#include <numa.h>
#include <iostream>
#include <cmath>

void NumaDispatcher::save_core_map(std::string file){
    std::ofstream out(file);
    if(!out.is_open())
        throw std::runtime_error("Error opening file: " + file);

    for(NumaCluster &c : core_map){
        out <<"Cluster " << c.cluster_id << "\n";
        //print physical cores
        out << "PC: ";
        for(int i : c.core_ids){
            out << i << " ";
        }
        out << "\n";
        out << "LC: ";
        for(int i : c.ht_core_ids){
            out << i << " ";
        }
        out << "\n";
    }
    out.close();
}

bool NumaDispatcher::load_core_map(std::string file){
    std::vector<NumaCluster> tmp(0);
    std::ifstream in(file);
    if(!in.is_open())
        return false;

    std::string line;
    while(std::getline(in,line)){
        std::istringstream iss(line);
        std::string token;
        iss >> token;
        if(token == "Cluster"){
            NumaCluster c;
            iss >> c.cluster_id;
            std::getline(in,line);
            std::istringstream iss2(line);
            iss2 >> token;
            if(token == "PC:"){
                while(iss2 >> token){
                    c.core_ids.push_back(std::stoi(token));
                }
            }
            std::getline(in,line);
            std::istringstream iss3(line);
            iss3 >> token;
            if(token == "LC:"){
                while(iss3 >> token){
                    c.ht_core_ids.push_back(std::stoi(token));
                }
            }
            tmp.push_back(c);
        }

        
    }
    //update the class member
    core_map = tmp;
    return true;
}

void NumaDispatcher::get_cores(std::vector<int>& physical_cores, std::vector<int>& logical_cores){
    //get the max core count
    uint max_cores = std::thread::hardware_concurrency();
    std::vector<int> phys_cores(0);
    std::vector<int> ht_cores(0);
    if(max_cores == 0)
        throw std::runtime_error("Error getting hardware concurrency");
    //initialize the vector
    for(uint i = 0; i < max_cores; i++){
        std::ifstream in("/sys/devices/system/cpu/cpu" + std::to_string(i) + "/topology/thread_siblings_list");
        if(!in.is_open())
            throw std::runtime_error("Error opening file: /sys/devices/system/cpu/cpu" + std::to_string(i) + "/topology/thread_siblings_list");
        std::string line,token;
        std::getline(in,line);  //get the first line
        std::istringstream iss(line);
        std::getline(iss,token,',');    //get the first token given the delimiter
        phys_cores.push_back(std::stoi(token));
        while(std::getline(iss,token,',')){
            ht_cores.push_back(std::stoi(token));  
        } 
    }

    std::sort(phys_cores.begin(),phys_cores.end());
    std::sort(ht_cores.begin(),ht_cores.end());
    // Step 2: Remove consecutive duplicates
    auto lastPhys = std::unique(phys_cores.begin(), phys_cores.end());
    phys_cores.erase(lastPhys, phys_cores.end());
    auto lastHt = std::unique(ht_cores.begin(), ht_cores.end());
    ht_cores.erase(lastHt, ht_cores.end());
    physical_cores = phys_cores;
    logical_cores = ht_cores;
    return;
}

void NumaDispatcher::bind_thread_to_core(std::thread& thread, int core_id){
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(core_id, &cpuset);
    
    int rc = pthread_setaffinity_np(thread.native_handle(), sizeof(cpu_set_t), &cpuset);
    if (rc != 0) {
        throw std::runtime_error("Failed to set thread affinity");
    }
}

uint NumaDispatcher::get_cache_id(int core_id, uint level){
    std::ifstream in("/sys/devices/system/cpu/cpu" + std::to_string(core_id) + "/cache/index" + std::to_string(level) + "/shared_cpu_map");
    if(!in.is_open())
        throw std::runtime_error("Error opening file: /sys/devices/system/cpu/cpu" + std::to_string(core_id) + "/cache/index" + std::to_string(level) + "/shared_cpu_map");
    std::string line;
    std::getline(in,line);
    std::istringstream iss(line);
    uint val;
    iss >> val;
    return val;
}

void NumaDispatcher::NumaCluster::sort_cache_topology(uint level){
    std::vector<std::pair<int,int>> cache_map(0);
    /**
     * PHYSICAL CORES
     */
    cache_map.reserve(core_ids.size());
    for(int i : core_ids){
        cache_map.push_back(std::make_pair(i,get_cache_id(i,level)));
    }
    //stable sort to keepe the core ordering
    std::stable_sort(cache_map.begin(),cache_map.end(),[](const std::pair<int,int> &a, const std::pair<int,int> &b){
        return a.second < b.second;
    });
    for(int i = 0; i < core_ids.size(); i++){
        core_ids[i] = cache_map[i].first;
    }
    /**
     * HYPER THREAD CORES
     */
    cache_map.clear();
    cache_map.reserve(ht_core_ids.size());
    for(int i : ht_core_ids){
        cache_map.push_back(std::make_pair(i,get_cache_id(i,level)));
    }
    std::stable_sort(cache_map.begin(),cache_map.end(),[](const std::pair<int,int> &a, const std::pair<int,int> &b){
        return a.second < b.second;
    });
    for(int i = 0; i < ht_core_ids.size(); i++){
        ht_core_ids[i] = cache_map[i].first;
    }

}

NumaDispatcher::NumaDispatcher(uint cache_level, bool try_load){
    if(try_load){
        if(load_core_map(core_map_file))
            return;
    }

    std::vector<int> physical_cores;
    std::vector<int> logical_cores;
    get_cores(physical_cores,logical_cores); //this initializes the physical_cores and logical_cores vectors 
    int max_node = numa_max_node();
    core_map.resize(max_node + 1); //account for node 0
    //initialize the clusters id
    for( int i = 0; i <= max_node; i++){
        core_map[i].cluster_id = i;
    }
    //map all physical cores
    for(int i : physical_cores)
        std::cout << i << "\n"; 


    for(int i = 0; i < physical_cores.size(); i++){
        int node_of_i = numa_node_of_cpu(physical_cores[i]);
        if(i >= 0){
            core_map[node_of_i].core_ids.push_back(physical_cores[i]);
        }
        else throw std::runtime_error("Error getting node of cpu: " + std::to_string(physical_cores[i]));
    }
    //map all logical cores

    for(int i = 0; i < logical_cores.size(); i++){
        int node_of_i = numa_node_of_cpu(logical_cores[i]);
        if(i >= 0)
            core_map[node_of_i].ht_core_ids.push_back(logical_cores[i]);
        else throw std::runtime_error("Error getting node of cpu: " + std::to_string(logical_cores[i]));
    }

    //sort based on cache_topology
    for(NumaCluster &c : core_map){
        c.sort_cache_topology(cache_level);
    }

    save_core_map(core_map_file);
}

//doesn't require cleanup
NumaDispatcher::~NumaDispatcher(){
    return;
}

void NumaDispatcher::dispatch_threads(const std::vector<std::thread>& threads){
    //circle over clusters physical cores and bind threads
    int thread_idx = 0;
    int __length = threads.size();
    bool first_pass = true;
    while(thread_idx < __length){
        for(NumaCluster &c : core_map){
            for(int i : (first_pass? c.core_ids : c.ht_core_ids)){
                if(thread_idx >= __length)
                    return;
                bind_thread_to_core(const_cast<std::thread&>(threads[thread_idx]),i);
                thread_idx++;
            }
        }
        first_pass = !first_pass;   //switch to hyperthreading cores then back to physical ones (if necessary)
    }
};

void NumaDispatcher::dispatch_threads(const std::vector<std::thread>& group1, std::vector<std::thread>& group2) {
    int g1_size = group1.size();
    int g2_size = group2.size();
    
    // If either group is empty, handle the non-empty group normally
    if (g1_size == 0 || g2_size == 0) {
        if (g1_size > 0) {
            dispatch_threads(group1);
        } else if (g2_size > 0) {
            dispatch_threads(group2);
        }
        return;
    }

    // Calculate ratio between groups to determine distribution
    float ratio = static_cast<float>(g1_size) / g2_size;
    int g1_batch = std::ceil(ratio);
    int g2_batch = 1;
    
    // If ratio is less than 1, invert it
    if (ratio < 1.0f) {
        ratio = 1.0f / ratio;
        g1_batch = 1;
        g2_batch = std::ceil(ratio);
    }

    int g1_idx = 0;
    int g2_idx = 0;
    bool first_pass = true;

    while (g1_idx < g1_size || g2_idx < g2_size) {
        // First pass: use physical cores
        // Second pass: use hyperthreading cores
        for (NumaCluster &cluster : core_map) {
            const std::vector<int>& cores = first_pass ? cluster.core_ids : cluster.ht_core_ids;
            
            for (int core_id : cores) {
                // Dispatch group1 threads according to ratio
                for (int i = 0; i < g1_batch && g1_idx < g1_size; i++) {
                    bind_thread_to_core(const_cast<std::thread&>(group1[g1_idx]), core_id);
                    g1_idx++;
                }
                
                // Dispatch group2 threads according to ratio
                for (int i = 0; i < g2_batch && g2_idx < g2_size; i++) {
                    bind_thread_to_core(group2[g2_idx], core_id);
                    g2_idx++;
                }
            }
        }
        
        // If we haven't placed all threads after using physical cores,
        // switch to hyperthreading cores
        if (first_pass && (g1_idx < g1_size || g2_idx < g2_size)) {
            first_pass = false;
        }
        // If we've used both physical and HT cores and still have threads,
        // start over with physical cores (will cause context switching)
        else if (!first_pass && (g1_idx < g1_size || g2_idx < g2_size)) {
            first_pass = true;
        }
    }
}

void NumaDispatcher::print_core_map(){
    std::stringstream out;

    for(NumaCluster &c : core_map){
        out <<"Cluster " << c.cluster_id << "\n";
        //print physical cores
        out << "PC: ";
        for(int i : c.core_ids){
            out << i << " ";
        }
        out << "\n";
        out << "LC: ";
        for(int i : c.ht_core_ids){
            out << i << " ";
        }
        out << "\n";
    }
    std::cout << out.str() << std::endl;
}
