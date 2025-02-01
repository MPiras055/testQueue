#pragma once

#include <vector>
#include <thread>
#include <string>
#include <numa.h>
#include <numeric>

#define core_map_file ".numa_core_map.tmp"

class NumaDispatcher {
private:
    //Information about Numa Topology
    struct NumaCluster {
        //static methods to find cache info

        int cluster_id = -1;
        std::vector<int> core_ids = std::vector<int>(0); //physical cores
        std::vector<int> ht_core_ids = std::vector<int>(0);   //hyperthreading cores

        void sort_cache_topology(uint level);
    };

    std::vector<NumaCluster> core_map;  //current core map

    void get_cores(std::vector<int>&,std::vector<int>&);
    static void bind_thread_to_core(std::thread&,int);
    static uint get_cache_id(int,uint);

    void save_core_map(std::string);
    bool load_core_map(std::string);

public:
    NumaDispatcher(uint cache_level, bool try_load = false);
        //prova a caricare la mappa dei core
        //se la trova allora termina
        //altrimenti inizializza la topologia
    
    ~NumaDispatcher();

    //get max_node
    //creates a vector for each numa core
        //foreach node sorts cores based on cache_topology
    //creates a cache_map for each core
    void dispatch_threads(const std::vector<std::thread>&);

    //dispatches threads from each group considering their ratio
    void dispatch_threads(const std::vector<std::thread>&, std::vector<std::thread>&);
    void print_core_map();

    /**
     * public methods used to access cpu and numa node information
     */
    inline static int get_core(){
        return sched_getcpu();
    }

    inline static int get_numa_node(){
        const int cpu = sched_getcpu();
        return cpu >= 0 ? numa_node_of_cpu(cpu) : -1;
    }
    
};