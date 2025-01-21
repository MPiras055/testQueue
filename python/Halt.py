

#! /usr/bin/env python3
import subprocess
import sys
import numpy as np

START_DIR = './build/'

EXEC = 10000

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

def halt(program: list):
    program[0] =  START_DIR + program[0][2:] 
    values = []
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
                print("HERE")
                print(line)
            if line_count > 0 : return

    #print('Program did not halt')

halt(sys.argv[1:])