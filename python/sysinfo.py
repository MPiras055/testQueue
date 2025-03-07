import os,sys

CPU_PATH = '/sys/devices/system/cpu'
NODE_PATH = '/sys/devices/system/node'

def get_node_of_cpu(core_id: int): # logical ids
    "Returns the numa node of the given core_id"
    for file in os.listdir(os.path.join(CPU_PATH,f"cpu{core_id}")):
        if not file.startswith("node"): continue
        node_idx = file[4:]
        if not node_idx.isdigit():continue
        return int(node_idx)
    return None


def get_cache_info(logic_id: int): #logic_id
    "Returns the list of the cache_ids for a given core" 
    cache_ids = []
    cache_idxs = []
    CACHE_PATH = os.path.join(CPU_PATH,f"cpu{logic_id}","cache")
    for dirs in os.listdir(CACHE_PATH):
        if not dirs.startswith("index"): continue
        if not dirs[5:].isdigit(): continue
        cache_idxs.append(os.path.join(CACHE_PATH,dirs))
    cache_idxs.sort(key=lambda x: int(x[-1]))

    #open the directories and read the id
    for cache in cache_idxs:
        with open(os.path.join(cache,"id"),'r') as f:
            cache_ids.append(int(f.read().strip()))
    return cache_ids
    
    
def get_cpu_info():
    """
        Returns a list of lists, for every tuple the first element is the physical core id and the other elements the PU id

        The length of the list is the number of physical cores
    """

    cpu_str_list = list(filter(lambda x: x.startswith("cpu") and x[3:].isdigit(),os.listdir(CPU_PATH)))
    cpu_list = []

    for cpu in cpu_str_list:
        cpu_nbr = int(cpu[3:])
        topology_dir = os.path.join(CPU_PATH,cpu,"topology")

        #exclude the pus that are not physical cores [redundant]
        siblings = open(os.path.join(topology_dir,"thread_siblings_list"),'r').read().strip().split(',')
        siblings = list(map(int,siblings))

        cpu_info = {
            'logical_id'    : cpu_nbr,
            'core_id'       : None,
            'siblings'      : siblings,
            'numa_node'     : None,
            'cache_ids'     : None, #list of cache ids for every level
            'package_id'    : None
        }

        #read the core_id

        cpu_info['core_id'] = int(open(os.path.join(topology_dir,"core_id"),'r').read().strip())
        cpu_info['package_id'] = int(open(os.path.join(topology_dir,"physical_package_id"),'r').read().strip())
        cpu_info['cache_ids'] = get_cache_info(cpu_info['logical_id'])
        cpu_info['numa_node'] = get_node_of_cpu(cpu_info['logical_id'])
        cpu_list.append(cpu_info)
    
    cpu_list.sort(key=lambda x: x['logical_id'])
    return cpu_list


def node_distance_matrix():
    "Returns the distance matrix between the numa nodes"
    list_dir = os.listdir(NODE_PATH)
    nodes = sorted(filter(lambda x: x.startswith("node") and x[4:].isdigit(),list_dir),key=lambda x: int(x[4:]))

    distance_matrix = []

    for node in nodes:
        distances = open(os.path.join(NODE_PATH,node,"distance"),'r').read().strip().split()
        distance_matrix.append(list(map(lambda x:int(x),distances)))

    return distance_matrix

    
def print_cpu_info(cpu_info: list[dict]):
    for cpu in cpu_info:
        print("Logical ID: ",   cpu['logical_id'])
        print("\tCore ID:\t",   cpu['core_id'])
        print("\tSiblings:\t",  cpu['siblings'])
        print("\tNuma Node:\t", cpu['numa_node'])
        print("\tCache IDs:\t", cpu['cache_ids'])
        print("\tPackage ID:\t",cpu['package_id'])
        print()


def numa_pinning(cache_level: int = 3, only_id = True) : #target shared L3 caches
    cpus = get_cpu_info()
    dist_v = node_distance_matrix()[0] #we the node 0 as reference
    
    dist_v = sorted([(dist_v[i],i) for i in range(0,len(dist_v))],key=lambda x:x[0]) # tiple of (d,node) sorted by d
    dist_v = list(map(lambda x: x[1],dist_v)) #keep only the node

    cpus.sort(key=lambda x:(x['numa_node'],x['cache_ids'][cache_level])) #sort based on nodes and shared cache

    numa_matrix = [[] for _ in range(len(dist_v))] # each row is a numa node

    for pu in cpus:
        numa_matrix[pu['numa_node']].append(pu)

    for row in numa_matrix:
        row.sort(key=lambda x: (x['cache_ids'])[cache_level])
        
    # filter into first set and second set
    first = []
    second = []

    for i in dist_v:
        for pu in numa_matrix[i]:
            if pu['logical_id'] == pu['siblings'][0]:
                first.append(pu)
            else:
                second.append(pu)

    if only_id:
        first = list(map(lambda x: x['logical_id'],first))
        second = list(map(lambda x: x['logical_id'],second))
    
    return first + second


def save_pinning(path: str, target_cache_level: int) -> None:
    pinning = numa_pinning(only_id=True,cache_level=target_cache_level)

    try:
        with open(path,'w') as f:
            for i in pinning:
                f.write(str(i) + '\n')
    except IOError:
        print("Error while writing to file")
    


if __name__ == "__main__":
    cache_target = 3

    pars = sys.argv[1:]
    if len(pars) == 0:
        print_cpu_info(get_cpu_info())
        exit(0)
    
    elif len(pars) == 2:
        if pars[1].isdigit(): cache_target = int(pars[1])
    
    save_pinning(pars[0],cache_target)