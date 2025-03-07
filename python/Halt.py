

#! /usr/bin/env python3
import subprocess
import sys,os
import numpy as np

#START_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)),'../build/')
START_DIR = "../build/"

EXEC = 1000

def halt_avg(program: list):
    program[0] =  START_DIR + program[0][2:] 
    values = []
    for i in range(EXEC):
        proc = subprocess.Popen(program,stdout = subprocess.PIPE, stderr = subprocess.PIPE)
        # Now, clear the last printed line
        sys.stdout.write("\r" + " " * 80)  # Move to the beginning of the line and overwrite with spaces
        sys.stdout.write("\r")  # Move back to the start of the line
        sys.stdout.flush()
        print('Step:', round(i / EXEC * 100,2), '%')

        proc.wait()

        # Read output line by line
        for line in proc.stdout:
            # Strip any whitespace and split the line into values
            for value in line.strip().split():
                values.append(value)
    values = [float(value) for value in values]
    avg = np.mean(values)
    stddev = np.std(values)

    print('MEAN:', avg)
    print('STDDEV:', stddev)

    #print('Program did not halt')

#to test



def halt(program: list,template = False):
    queues_to_test = ["BoundedSegmentCRQueue"]
    program[0] =  START_DIR + program[0][2:] 
    
    procs = []

    if template:
        for q in queues_to_test:
            process = program.copy()
            process[1] = q
            procs.append(process)
    else: procs.append(program)

    for program in procs:
        for i in range(EXEC):
            proc = subprocess.Popen(program,stderr=subprocess.PIPE)
            print('Step:', round(i / EXEC * 100,2), '%')
            proc.wait()
            #if stderr is not empty then return immediately
            if proc.stderr:
                #print stderr 
                line_count = 0
                for line in proc.stderr:
                    line_count += 1
                    print(line.decode('utf-8'))
                    sys.exit(1)

THREADS = [1,2,4,8,12,16,24,32,48,64,96,112,128]

THREADS_MM = THREADS[:-3]

RUNS = 50

LENGTHS = [4,8,16,64,128,256,512,1024,2048,4096]


def exec_fail(proc: list[str], runs: int):
    print(proc)
    for i in range(runs):
        process = subprocess.Popen(proc,stderr=subprocess.PIPE)
        if(process.wait() != 0):
            print("Error in process")
            sys.exit(1)

        if proc.stderr:
            #print stderr 
            line_count = 0
            for line in proc.stderr:
                line_count += 1
                print(line.decode('utf-8'))
                sys.exit(1)
        
        print('Step:', round(i / runs * 100,2), '%',f" {proc}")

def halt_prq():
    queues_to_test = ["LinkedPRQueue"]

    program = [START_DIR + 'ManyToMany',queues_to_test[0],0,0,0,str(10**6),"0","0"]

    # --- Test Many To Many #

    for q in queues_to_test:
        for t in THREADS_MM:
            program[2] = str(t)
            program[3] = str(t)
            for l in LENGTHS:
                program[4] = str(l)
                exec_fail(program,RUNS)

    # --- Test Many To One --- #
    for q in queues_to_test:
        for t in THREADS_MM:
            program[2] = str(t)
            program[3] = str(1)
            for l in LENGTHS:
                program[4] = str(l)
                exec_fail(program,RUNS)
    
    # --- Test One To Many --- #
    for q in queues_to_test:
        for t in THREADS_MM:
            program[2] = str(1)
            program[3] = str(t)
            for l in LENGTHS:
                program[4] = str(l)
                exec_fail(program,RUNS)

    open('Success','w').close()
    #print('Program did not halt')

halt(sys.argv[1:])