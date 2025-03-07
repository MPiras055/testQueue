import os
import subprocess
import shutil
import csv
import numpy as np
import logging
from typing import List, Tuple

logging.basicConfig(level=logging.INFO, format="%(asctime)s - %(levelname)s - %(message)s")

class TimeoutError(Exception):
    pass

def get_project_dir() -> str:
    return os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))

def check_queues(exec_file: str, queues: List[str]) -> List[str]:
    invalid_queues = []
    build_dir = os.path.join(get_project_dir(), "build")
    for q in queues:
        proc = subprocess.run([f"./{exec_file}", q],
                              stdout=subprocess.DEVNULL,
                              stderr=subprocess.DEVNULL,
                              cwd=build_dir)
        if proc.returncode != 0:
            invalid_queues.append(q)
    return invalid_queues

def compile_project(exec_targets: List[str], fresh: bool = False) -> bool:
    build_dir = os.path.join(get_project_dir(), "build")
    if fresh and os.path.exists(build_dir):
        shutil.rmtree(build_dir)
    os.makedirs(build_dir, exist_ok=True)
    
    # targets fresh recompilation [unsets the cached variables]
    if subprocess.run(["cmake","-U'*'", "-DCMAKE_BUILD_TYPE=Release", ".."], cwd=build_dir).returncode != 0:
        logging.error("CMake failed!")
        return False
    
    if subprocess.run(["make", "-j"] + exec_targets, cwd=build_dir).returncode != 0:
        logging.error("Make failed!")
        return False
    
    return True

def calculate_delay(delay: int, run_count: int = 10000, checks: int = 5) -> Tuple[str, int]:
    if delay < 100:
        return 0,0
    
    tolerance = delay // 10
    proc = subprocess.run(["./time", str(delay), str(tolerance), str(run_count), str(checks)],
                          cwd=os.path.join(get_project_dir(), "build"), stdout=subprocess.PIPE)
    if proc.returncode != 0:
        raise ChildProcessError("Error in delay calculation")
    
    lines = proc.stdout.decode().split("\n")
    if len(lines) < 2:
        raise ValueError("Invalid output")
    
    return int(lines[0]), int(lines[1])

def run_benchmark(*, exec_file_out: str, outfile: str, queues: List[str], threads: List[Tuple[int, int]], ops: int, runs: int, sizes: List[int], delays: List[int], timeout_base: int = 200):
    unbalanced_work = 0
    if exec_file_out == "OneToMany":
        unbalanced_work = 2 # only consumers work
    elif exec_file_out == "ManyToOne":
        unbalanced_work = 1 # only producers work

    exec_file = "ManyToMany" #underlying exec_file that gets called

    project_dir = os.path.join(get_project_dir(), "build")
    computed_delays = []
    
    for delay in delays:
        try:
            center, amplitude = calculate_delay(delay)
            timeout = max(timeout_base, ops * 2 * delay * 1e-9 * 2)
            computed_delays.append((delay, center, amplitude, timeout))
        except Exception as e:
            logging.error(f"Error in delay calculation for delay {delay}: {e}")
    
    with open(outfile, "w", newline="") as file:
        writer = csv.DictWriter(file, fieldnames=[
            "Queue", "Producers", "Consumers", "Size", "Items", "Runs", "Delay", "Score", "Score Error"
        ])
        writer.writeheader()
        
        total_runs = len(queues) * len(threads) * len(sizes) * len(delays) * runs
        step = 0
        
        for q in queues:
            for p, c in threads:
                for s in sizes:
                    for delay, center, amplitude, current_timeout in computed_delays:
                        results = []
                        for _ in range(runs):
                            try:
                                proc_list = [f"./{exec_file}", q, str(p), str(c), str(s), str(ops), str(center), str(amplitude), str(unbalanced_work)]
                                print(proc_list)
                                proc = subprocess.run(
                                    proc_list,
                                    cwd=project_dir, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                    timeout=current_timeout
                                )
                                if proc.returncode != 0:
                                    logging.error(f"Error in run for queue {q}")
                                    break
                                results.append(float(proc.stdout.decode().strip()))
                            except subprocess.TimeoutExpired:
                                logging.error(f"Timeout in run for queue {q} p:{p} c:{c}")
                                break
                        
                        if len(results) != runs:
                            logging.warning(f"Incomplete runs for queue {q}")
                            continue
                        
                        writer.writerow({
                            "Queue": (q), "Producers": p, "Consumers": c,
                            "Size": s, "Items": ops, "Runs": runs, "Delay": delay,
                            "Score": np.mean(results), "Score Error": np.std(results)
                        })
                        file.flush()
                        step += runs
                        logging.info(f"Progress: [{q} p:{p} c:{c} size:{s}] {step}/{total_runs} ({(step/total_runs)*100:.2f}%)")


def run_All2All(*, exec_file: str, outfile: str, threads: List[Tuple[int, int]], ops: int, runs: int, sizes: List[int], delays: List[int], timeout_base: int = 200):
    unbalanced_work = 0
    #infer prod_cons by thread configuration
    """
        if exec_file_out == "OneToMany":
            unbalanced_work = 2 # only consumers work
        elif exec_file_out == "ManyToOne":
            unbalanced_work = 1 # only producers work
    """

    # OneToMany
    if threads[0][0] == threads[1][0]:
        unbalanced_work = 2
    # ManyToOne
    if threads[0][1] == threads[1][1]:
        unbalanced_work = 1

    project_dir = os.path.join(get_project_dir(), "build")
    computed_delays = []
    
    for delay in delays:
        try:
            center, amplitude = calculate_delay(delay)
            timeout = max(timeout_base, ops * 2 * delay * 1e-9 * 2)
            computed_delays.append((delay, center, amplitude, timeout))
        except Exception as e:
            logging.error(f"Error in delay calculation for delay {delay}: {e}")
    
    with open(outfile, "w", newline="") as file:
        writer = csv.DictWriter(file, fieldnames=[
            "Queue", "Producers", "Consumers", "Size", "Items", "Runs", "Delay", "Score", "Score Error"
        ])
        writer.writeheader()
        
        total_runs = len(threads) * len(sizes) * len(delays) * runs
        step = 0
        
        
        for p, c in threads:
            for s in sizes:
                for delay, center, amplitude, current_timeout in computed_delays:
                    results = []
                    for _ in range(runs):
                        try:
                            proc_list = [f"./{exec_file}", str(p), str(c), str(s), str(ops), str(center), str(amplitude), str(unbalanced_work)]
                            print("Spawning process")
                            proc = subprocess.run(
                                proc_list,
                                cwd=project_dir, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                timeout=current_timeout
                            )
                            if proc.returncode != 0:
                                logging.error(f"Error in run for {exec_file}")
                                break
                            results.append(float(proc.stdout.decode().strip()))
                        except subprocess.TimeoutExpired:
                            logging.error(f"Timeout in run for queue {exec_file} p:{p} c:{c}")
                            break
                    
                    if len(results) != runs:
                        logging.warning(f"Incomplete runs for queue {exec_file}")
                        continue
                    
                    writer.writerow({
                        "Queue": (exec_file), "Producers": p, "Consumers": c,
                        "Size": s, "Items": ops, "Runs": runs, "Delay": delay,
                        "Score": np.mean(results), "Score Error": np.std(results) if len(results) > 1 else 0
                    })
                    file.flush()
                    step += runs
                    logging.info(f"Progress: [{exec_file} p:{p} c:{c} size:{s}] {step}/{total_runs} ({(step/total_runs)*100:.2f}%)")


def main():
    NORMAL_QUEUES = [
        "BoundedMTQueue", "BoundedMuxQueue", "LinkedMTQueue", "LinkedCRQueue", "LinkedPRQueue", "LinkedMuxQueue", "FAAArrayQueue"
    ]
    MY_QUEUES = [
        "BoundedItemCRQueue","BoundedSegmentCRQueue","BoundedItemPRQueue","BoundedSegmentPRQueue"
    ]

    QUEUES = NORMAL_QUEUES + MY_QUEUES

    THREADS_BALANCED = [1, 2, 4, 8, 12, 16, 24, 32]
    THREADS_UNBALANCED = [1, 3, 7, 11, 15, 23, 31, 47, 63]
    
    if not compile_project(["ManyToMany", "All2All", "time"], fresh=True):
        return
    
    invalid_queues = False; check_queues("ManyToMany", NORMAL_QUEUES + MY_QUEUES)
    if invalid_queues:
        logging.error(f"Invalid queues found: {invalid_queues}")
        return
    
    subprocess.run("clear")
    
    benchmarks_low_delay = [
        ("All2All", "../csv/All2All_OTM.csv", [(1,t) for t in THREADS_UNBALANCED]),
        ("All2All", "../csv/All2All_MTO.csv", [(t,1) for t in THREADS_UNBALANCED])
    ]
    
    # for exec_name, output_file, thread_conf in benchmarks_low_delay:
    #     #low delay
    #     run_All2All(exec_file=exec_name, outfile=output_file, threads=thread_conf, ops=1_000_000, runs=1, sizes=[2048,16384], delays=[0,200,800,1200,2400,10000])
    #     # run_benchmark(exec_file=exec_name, outfile=output_file, queues= QUEUES, threads=thread_conf, ops=1_000_000, runs=10, sizes=[2048,16384], delays=[0,200,800,1200,2400,10000])
    #     logging.info(f"Benchmark completed! results written in {output_file}")

    benchmarks_low_delay = [
        ("OneToMany","../csv/OneToManyBPRQ",[(1,t) for t in THREADS_UNBALANCED]),
        ("ManyToOne","../csv/ManyToOneBPRQ",[(t,1) for t in THREADS_UNBALANCED])
    ]

    benchmark_low_delay_2 = [("ManyToMany","../csv/ManyToManyBPRQ",[(t,t) for t in THREADS_BALANCED])]

    # benchmarks_low_delay = [
    #     ("OneToMany","../csv/OneToMany",[(1,t) for t in THREADS_UNBALANCED])
    # ]

    for exec_name,output_file,thread_conf in benchmarks_low_delay:
        print("Running")
        run_benchmark(exec_file_out=exec_name, outfile=output_file, queues=["BoundedPRQueue"], threads=thread_conf, ops=1_000_000, runs=1, sizes=[64,2048,16384], delays=[0,200,800,1200,2400,10000])

    for exec_name,output_file,thread_conf in benchmark_low_delay_2:
        run_benchmark(exec_file_out=exec_name, outfile=output_file, queues=["BoundedPRQueue"], threads=thread_conf, ops=1_000_000, runs=10, sizes=[64,2048,16384], delays=[0,200,800,1200,2400,10000])

    # benchmark_low_delay = [("ManyToMany","../csv/ManyToMany",[(t,t) for t in THREADS_BALANCED])]

    # for exec_name,output_file,thread_conf in benchmarks_low_delay:
    #     run_benchmark(exec_file_out=exec_name, outfile=output_file, queues= QUEUES, threads=thread_conf, ops=1_000_000, runs=1, sizes=[64,2048,16384], delays=[0,200,800,1200,2400,10000])

    logging.info("All benchmarks completed!")

if __name__ == "__main__":
    main()
