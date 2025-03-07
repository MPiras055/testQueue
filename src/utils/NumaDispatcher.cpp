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
#include <sched.h>


bool Dispatcher::load_topology(std::string& file){
    std::ifstream in(file);
    if(!in.is_open()) return false;

    std::string line;
    int val;
    while(std::getline(in,line)){
        if(std::sscanf(line.c_str(),"%d",&val) == 1){
            logical_core_map.push_back(val);
        } else return false;

    }
    return true;
}

void Dispatcher::bind_thread_to_core(std::thread& thread, int core_id){
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(core_id, &cpuset);
    
    int rc = pthread_setaffinity_np(thread.native_handle(), sizeof(cpu_set_t), &cpuset);
    if (rc != 0) throw std::runtime_error("Failed to set thread affinity");
    
}

Dispatcher::Dispatcher(uint cache_level, std::string &topology_file){
    //check if the file exists
    if(std::filesystem::exists(topology_file)){
        if(!load_topology(topology_file))
            throw std::runtime_error("Error loading core map from file: " + topology_file);
    } else throw std::runtime_error("File not found: " + topology_file);
}

//doesn't require cleanup
Dispatcher::~Dispatcher(){};

void Dispatcher::dispatch_threads(const std::vector<std::thread>& threads){
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

void Dispatcher::dispatch_threads(std::vector<std::thread>& group1, std::vector<std::thread>& group2) {
    int g1_size = group1.size();
    int g2_size = group2.size();
    
    // If either group is empty, dispatch the other group only
    if(g1_size == 0 || g2_size == 0){
        dispatch_threads(g1_size == 0? group2 : group1);
        return;
    }

    if(g1_size > g2_size){
        std::swap(group1,group2);
        std::swap(g1_size,g2_size);
    }

    // Calculate the thread dispatch ratio
    const int gcd = std::gcd(g1_size, g2_size);
    const int g1_batch = g1_size / gcd;
    const int g2_batch = g2_size / gcd;

    int g1_idx = 0,g2_idx = 0;
    int g1_batch_idx = 0,g2_batch_idx = 0;

    bool which_batch = false;
    bool use_physical_cores = true;

    while (g1_idx < g1_size || g2_idx < g2_size) {
        for(int iCluster = 0; iCluster < core_map.size() ; iCluster++){
            NumaCluster &c = core_map[iCluster];
            std::vector<int> cores = use_physical_cores ? c.core_ids : c.ht_core_ids;
            size_t cores_size = cores.size();
            size_t iCore = 0;
            while(iCore < cores_size && (g1_idx < g1_size || g2_idx < g2_size)){    //no ping pong the while exits if both groups are done
                //schedule batch from first group
                if(which_batch){
                    if(g2_batch_idx++ >= g2_batch || g2_idx >= g2_size){ //if batch is done or index out of range
                        which_batch = !which_batch; //flip group
                        g2_batch_idx = 0;
                    } else {
                        bind_thread_to_core(const_cast<std::thread&>(group2[g2_idx++]),cores[iCore++]); //increment core cout only when scheduling
                    }
                } else {
                    if(g1_batch_idx++ >= g1_batch || g1_idx >= g1_size){ //flip to other group
                        which_batch = !which_batch; //flip group
                        g1_batch_idx = 0;   //reset other batch index
                    } else {
                        bind_thread_to_core(const_cast<std::thread&>(group1[g1_idx++]),cores[iCore++]);
                    }
                }

            }

            if(g1_idx >= g1_size && g2_idx >= g2_size)  //if both finished then return
                return;
        }

        use_physical_cores = !use_physical_cores; //flip to hyperthreading cores || bac kto physical if necessary
    }
}

