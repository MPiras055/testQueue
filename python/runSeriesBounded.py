#! /usr/bin/env python3

import os
import subprocess
import shutil
import csv
import numpy as np
import logging
from typing import List, Tuple, Optional

logging.basicConfig(level=logging.INFO, format="%(asctime)s - %(levelname)s - %(message)s")

class TimeoutError(Exception):
    pass

# Configuration
"""
    We're working with delays
"""
CONFIG = {
    "EXEC": "ManyToMany",
    "NORMAL_QUEUES": [
        "BoundedMTQueue", "BoundedMuxQueue",
        "LinkedMTQueue", "LinkedCRQueue", "LinkedMuxQueue", "FAAArrayQueue"
    ],
    "WEIRD_QUEUES": [
        "BoundedItemCRQueue", "BoundedSegmentCRQueue"
    ],
    "THREADS": [2, 4, 8, 12, 16, 24, 32], #discard the case 1-1
    "THREADS_WEIRD" : [3,7,11,15,23,31,47,63], #used for unbalance load [discard the case 1-1 2-2]
    "OPS": 1_00_000,
    #"SIZE": [64, 2048, 16382],
    "SIZE": [64],
    "RUNS": 10,
    # delay (t - 10% t , t + 10% t)
    "CENTER" : [17000],
    "AMPLITUDE" : [int(17000/2)],
    "TIMEOUT": 120,  # Timeout for subprocess execution [seconds]
}

CONFIG["QUEUES"] = CONFIG["WEIRD_QUEUES"]

def get_project_dir() -> str:
    """Returns the absolute path of the project directory."""
    return os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))

def check_queues() -> List[str]:
    """Checks if queues are valid by executing ManyToMany with zero operations."""
    invalid_queues = []
    build_dir = os.path.join(get_project_dir(), "build")
    for q in CONFIG["QUEUES"]:
        proc = subprocess.run([f"./{CONFIG['EXEC']}", q],
                              stdout=subprocess.DEVNULL,
                              stderr=subprocess.DEVNULL,
                              cwd=build_dir)
        if proc.returncode != 0:
            invalid_queues.append(q)
    return invalid_queues

def compile_project(fresh: bool = False) -> bool:
    """Compiles the project, removing build directory if fresh is True."""
    build_dir = os.path.join(get_project_dir(), "build")
    if fresh and os.path.exists(build_dir):
        shutil.rmtree(build_dir)
    os.makedirs(build_dir, exist_ok=True)
    
    #don't compile with debug flags
    cmake_cmd = ["cmake", "-DCMAKE_BUILD_TYPE=Release", ".."]
    if subprocess.run(cmake_cmd, cwd=build_dir).returncode != 0:
        logging.error("CMake failed!")
        return False
    
    if subprocess.run(["make", "-j", "ManyToMany", "All2All"], cwd=build_dir).returncode != 0:
        logging.error("Make failed!")
        return False
    
    return True

def run_benchmark(
    exec_file: str,
    outfile: str,
    threads: Optional[List[Tuple[int, int]]] = None,
    queues: Optional[List[str]] = None,
    ops: Optional[int] = None,
    runs: Optional[int] = None,
    center: Optional[int] = 0,
    amplitude: Optional[int] = 0
):
    """Runs benchmark tests and writes results to CSV."""
    project_dir = os.path.join(get_project_dir(), "build")
    threads = threads or [(t, t) for t in CONFIG["THREADS"]]
    queues = queues or CONFIG["WEIRD_QUEUES"]
    ops = ops or CONFIG["OPS"]
    runs = runs or CONFIG["RUNS"]
    center = center or CONFIG["CENTER"]
    amplitude = amplitude or CONFIG["AMPLITUDE"]
    
    with open(outfile, mode="w", newline="") as file:
        writer = csv.DictWriter(file, fieldnames=[
            "Queue", "Producers", "Consumers", "Size", "Items", "Runs", "Score", "Score Error"
        ])
        writer.writeheader()
        
        total_runs = len(queues) * len(threads) * len(CONFIG["SIZE"]) * runs
        step = 0
        
        for q in queues:
            for p, c in threads:
                for s in CONFIG["SIZE"]:
                    results = []
                    for i in range(runs):
                        try:
                            proc = subprocess.run(
                                [f"./{exec_file}", q, str(p), str(c), str(s), str(ops), str(center), str(amplitude)],
                                cwd=project_dir, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                timeout=CONFIG["TIMEOUT"]
                            )
                            if proc.returncode != 0:
                                logging.error(f"Error in run {i} for queue {q}")
                                break
                            results.append(float(proc.stdout.decode().strip()))
                        except subprocess.TimeoutExpired:
                            logging.error(f"Timeout in run {i} for queue {q} p:{p} c:{c}")
                            break
                    
                    if len(results) != runs:
                        logging.warning(f"Incomplete runs for queue {q}")
                        continue
                    
                    writer.writerow({
                        "Queue": q, "Producers": p, "Consumers": c,
                        "Size": s, "Items": ops, "Runs": runs,
                        "Score": int(np.mean(results)), "Score Error": int(np.std(results))
                    })
                    file.flush()
                    step += len(results)
                    logging.info(f"Progress: [{q} p:{p} c:{c} length:{s}] {step}/{total_runs} ({(step/total_runs)*100:.2f}%)")

def main():
    if not compile_project(fresh=True):
        return
    
    invalid_queues = check_queues()
    if invalid_queues:
        logging.error(f"Invalid queues found: {invalid_queues}")
        return
    

    subprocess.run("clear")
    
    # Running different benchmarks
    benchmarks = [
        ("ManyToMany", "../csv/ManyToMany.csv", [(t, t) for t in CONFIG["THREADS"]]),
        ("ManyToMany", "../csv/ManyToOne.csv", [(t, 1) for t in CONFIG["THREADS_WEIRD"]]),
        ("ManyToMany", "../csv/OneToMany.csv", [(1, t) for t in CONFIG["THREADS_WEIRD"]]),
        ("ManyToMany", "../csv/OneToOne.csv", [(1, 1)])
    ]
    
    #1 micro of delay

    for exec_name, output_file, thread_conf in benchmarks:
        run_benchmark(exec_name, output_file, threads=thread_conf)
        
    # Running benchmarks with workload variance
    for exec_name, output_file, thread_conf in benchmarks:
        run_benchmark(exec_name, output_file.replace(".csv", "_work.csv"), threads=thread_conf, ops=operations)

if __name__ == "__main__":
    main()
