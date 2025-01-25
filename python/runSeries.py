#! /usr/bin/env python3

import os, sys, subprocess
import csv
import numpy as np

P_DIR = None
EXEC = "ManyToMany"
TESTS = ["oneToMany","manyToOne","manyToMany","oneToOne"]
#QUEUES = ["BoundedCRQueue","BoundedPRQueue","BoundedMTQueue","BoundedMuxQueue","LinkedPRQueue","LinkedMTQueue","LinkedCRQueue","LinkedMuxQueue","FAAArrayQueue"]
QUEUES = ["BoundedCRQueue","BoundedPRQueue"]
THREADS = [1,2,4,8,12,16,24,32,48,64,96,112,128]

OPS = 1_000_000 #number of operations
SIZE = [64,2048,16382]
RUNS = 5 #number of runs

"""
Assumes that the script is located in ProjectDir/python
"""
def getProjectDir():
    return os.path.abspath(os.path.join(os.path.dirname(__file__),".."))
    
"""
    if ManyToMany called with 0 as numOps returns 0 if the queue is valid and 1 if the queue is invalid
"""
def check_queues()->list:
    invalidQueues:list = []
    for q in QUEUES:
        # EXEC,"1",str(t),str(s),str(OPS),"0","1"
        proc = subprocess.run([f"./{EXEC}","1","1","1","0","0","0","0"],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL,cwd=f"{getProjectDir()}/build") #start from 
        #check process exitStatus
        if proc.returncode != 0:
            invalidQueues.append(q)
        else: print(f"{q} is valid")
    return invalidQueues

"""
    fresh update cmake changes and compile all executables
"""
def compile(jobs:int = 16)->bool:
    if type(jobs) != int:
        raise TypeError("jobs must be an integer")
    elif jobs < 1:
        raise ValueError("jobs must be greater than 0")
    proc = subprocess.run(["cmake",".."],cwd=f"{getProjectDir()}/build")
    if proc.returncode != 0:
        print("Error in cmake")
        return False
    proc = subprocess.run(["make","-C","build","-j","16"],cwd=f"{getProjectDir()}")
    if proc.returncode != 0:
        print("Error in make")
        return False
    return True

def runOneToOne():
    projectDir = f"{getProjectDir()}/build"
    with open("../OneToOne.csv",mode="w",newline="") as file:
        fieldnames = ["Queue","Producers","Consumers","Size","Items","Runs","Score","Score Error"]
        writer = csv.DictWriter(file,fieldnames=fieldnames)
        #write the header (column names)
        writer.writeheader()
        totalRuns = len(QUEUES) * len(SIZE) * RUNS
        step = 0
        for q in QUEUES:
            for s in SIZE:
                results:list = []
                for i in range(RUNS):
                    # " <queue_name> <producers> <consumers> <size_queue> <items> <min_wait> <max_wait>" process signature
                    proc = subprocess.run([f"./{EXEC}",q,"1","1",str(s),str(OPS),"0","1"],cwd=projectDir,stdout=subprocess.PIPE)
                    if proc.returncode != 0:
                        print(f"Error in runOneToOne for queue {q} run {i}")
                        continue
                    else:
                        step += 1
                        print(f"STEP: {step/totalRuns*100:.2f}%")
                    
                    #parse stdout output first line of output should have the result in the form of a floating point
                    results.append(proc.stdout.decode().strip())
                
                #after it's done running calculate average and stddev
                if len(results) != RUNS:
                    print(f"Error in at least one run for queue {q}")
                    continue
                results = np.array(results,dtype=float)
                avg = np.mean(results)
                std = np.std(results)
                writer.writerow({"Queue":q,"Producers":1,"Consumers":1,"Size":s,"Items":OPS,"Runs":RUNS,"Score":avg,"Score Error":std})
    return

def runOneToMany():
    projectDir = f"{getProjectDir()}/build"
    #threads = [1,2,4,8,12,16]
    threads = THREADS
    with open("../OneToMany.csv",mode="w",newline="") as file:
        filednames = ["Queue","Producers","Consumers","Size","Items","Runs","Score","Score Error"]
        """
            For now we just test a limited amount of threads because no wait is involved
            because single producer will be a useless bottleneck to the system
        """
        writer = csv.DictWriter(file,fieldnames=filednames)
        writer.writeheader()
        totalRuns = len(QUEUES) * len(threads) * len(SIZE) * RUNS
        step = 0
        for q in QUEUES:
            for t in threads:
                for s in SIZE:
                    results:list = []
                    for i in range(RUNS):
                        proc = subprocess.run([f"./{EXEC}",q,"1",str(t),str(s),str(OPS),"0","1"],cwd=projectDir,stdout=subprocess.PIPE)
                        if proc.returncode != 0:
                            print(f"Error in runOneToMany for queue {q} run {i}")
                            continue
                        else:
                            step += 1
                            print(f"STEP: {step/totalRuns*100:.2f}%")
                        results.append(proc.stdout.decode().strip())
                    if len(results) != RUNS:
                        print(f"Error in at least one run for queue {q}")
                        continue
                    results = np.array(results,dtype=float)
                    avg = np.mean(results)
                    std = np.std(results)
                    writer.writerow({"Queue":q,"Producers":1,"Consumers":t,"Size":s,"Items":OPS,"Runs":RUNS,"Score":avg,"Score Error":std})
    return

def runManyToOne():
    projectDir = f"{getProjectDir()}/build"
    threads = THREADS
    with open("../ManyToOne.csv",mode="w",newline="") as file:
        filednames = ["Queue","Producers","Consumers","Size","Items","Runs","Score","Score Error"]
        """
            For now we just test a limited amount of threads because no wait is involved
            because consumers will be a useless bottleneck to the system
        """
        writer = csv.DictWriter(file,fieldnames=filednames)
        writer.writeheader()
        totalRuns = len(QUEUES) * len(threads) * len(SIZE) * RUNS
        step = 0
        for q in QUEUES:
            for t in threads:
                for s in SIZE:
                    results:list = []
                    for i in range(RUNS):
                        proc = subprocess.run([f"./{EXEC}",q,str(t),"1",str(s),str(OPS),"0","1"],cwd=projectDir,stdout=subprocess.PIPE)
                        if proc.returncode != 0:
                            print(f"Error in runOneToMany for queue {q} run {i}")
                            continue
                        else:
                            step += 1
                            print(f"STEP: {step/totalRuns*100:.2f}%")
                        results.append(proc.stdout.decode().strip())
                    if len(results) != RUNS:
                        print(f"Error in at least one run for queue {q}")
                        continue
                    results = np.array(results,dtype=float)
                    avg = np.mean(results)
                    std = np.std(results)
                    writer.writerow({"Queue":q,"Producers":t,"Consumers":1,"Size":s,"Items":OPS,"Runs":RUNS,"Score":avg,"Score Error":std})
    return


"""
Pass a list of tuples with the number of producers and consumers
"""
def runManyToMany(threads: list[tuple[int,int]],outfile:str):
    projectDir = f"{getProjectDir()}/build"
    threads = THREADS[:-3]
    with open(outfile,mode="w",newline="") as file:
        filednames = ["Queue","Producers","Consumers","Size","Items","Runs","Score","Score Error"]
        writer = csv.DictWriter(file,fieldnames=filednames)
        writer.writeheader()
        totalRuns = len(QUEUES) * len(threads) * len(SIZE) * RUNS
        step = 0
        for q in QUEUES:
            for t in threads: #since each threads test double the amount of threads then we can skip the last 3
                for s in SIZE:
                    results:list = []
                    for i in range(RUNS):
                        #add perf support
                        proc = subprocess.run([f"./{EXEC}",q,str(t[0]),str(t[1]),str(s),str(OPS),"0","1"],cwd=projectDir,stdout=subprocess.PIPE)
                        if proc.returncode != 0:
                            print(f"Error in runManyToMany for queue {q} run {i}")
                            continue
                        else:
                            step += 1
                            print(f"STEP: {step/totalRuns*100:.2f}%")
                        results.append(proc.stdout.decode().strip())
                    if len(results) != RUNS:
                        print(f"Error in at least one run for queue {q}")
                        continue
                    results = np.array(results,dtype=float)
                    avg = np.mean(results)
                    std = np.std(results)
                    writer.writerow({"Queue":q,"Producers":t,"Consumers":t,"Size":s,"Items":OPS,"Runs":RUNS,"Score":avg,"Score Error":std})
    return

def main():
    P_DIR = getProjectDir()
    print(P_DIR)
    # if not compile():
    #     print("Error in compilation")
    #     return
    invalidQueues = check_queues()
    if len(invalidQueues) > 0:
        print(f"Invalid queues found: {invalidQueues}")
        return
    runManyToMany()
    print("ManyToMany done")

if __name__ == "__main__":
    main()
        
        
                



