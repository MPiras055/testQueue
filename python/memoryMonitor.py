import time,sys,csv,os,tempfile,subprocess

# use /proc/pid/statm <total program size> <resident set size> <shared pages> <text (code) pages> <library (data) pages> <data+stack pages> <dirty pages>

BATCH_SIZE = 1000

def getPageSize():
    proc = subprocess.run(["getconf","PAGESIZE"],stdout=subprocess.PIPE)
    return int(proc.stdout)

"""
Return the process memory in terms of pages
@param pid: process id
@return tuple(total program size [VM], resident set size [RSS])
"""
def pidMemInfo(pid: int) -> list[int,int]:
    try:
        with open(f"/proc/{pid}/statm", "r") as file:
            # read only the first 2 lines
            line = file.readline().split()
            return list([(int(line[0])), (int(line[1]))]) # shift right to get the mbytes
    except IOError:
        sys.stderr.write(f"[MEMORY_READ] Error opening file for PID {pid}")
    return [-1,-1]

"""
The calling process should be something like python3 memoryMonitor.py [granularity] [outfile] [command]

We perform batching to reduce the number of writes to the file
"""

def main(granularity:float,outfile:str,command:list[str]) -> None : 
    if granularity <= 0:
        raise ValueError("Granularity must be a positive float")

    header = ["VM","RSS","Step"]
    step = 0 #append a step 
    retval = None
    memory: list[list[int,int,int]] = []
    _break = False
    with open(outfile,"w",newline='') as csvOutput:
        writer = csv.writer(csvOutput)
        writer.writerow(header)
        process = subprocess.Popen(command)
        pid = process.pid
        while not _break:
            for i in range(BATCH_SIZE):
                retval = process.poll()
                if retval is not None:
                    _break = True
                    break
                memory.append(pidMemInfo(pid)+[step])
                step += 1
                time.sleep(granularity)
            # write to csfile
            writer.writerows(memory)
            memory = []
    if retval != 0:
        raise Exception(f"Process Return Code: {retval}")

"""
We use it to compute the memory in terms of bytes
@param outfile: the csv file to process
@param unit: the number of bits to shift right (i.e.. 10 = kB, 20 = MB, 30 = GB)
"""
def process_csv(outfile:str,unit: int = 20) -> None:
    if unit < 0:
        raise ValueError("Unit must be a positive integer")
    PAGESIZE = getPageSize()
    batch = []

    with tempfile.NamedTemporaryFile(mode='w',delete=True,dir=os.getcwd()) as temp:
        with open(outfile,'r') as file:
            reader = csv.reader(file)
            writer = csv.writer(temp)
            try:
                header = next(reader)
            except StopIteration:
                raise Exception("Empty file")
            else: writer.writerow(header)
            _break = False
            while not _break:
                for i in range(BATCH_SIZE):
                    try:
                        row = next(reader)
                    except StopIteration:
                        _break = True
                        break
                    for i in range(2):
                        row[i] = round(int(row[i])*PAGESIZE /2**unit,2)
                    batch.append(row)
                writer.writerows(batch)
                batch = []
        temp.flush()
        os.replace(temp.name,outfile)

if __name__ == "__main__":
    if len(sys.argv) < 4:
        sys.stderr.write("Usage: python3 memoryMonitor.py granularity outfile command...\n")
        sys.exit(1)
    try:
        main(float(sys.argv[1]),sys.argv[2],sys.argv[3:])
        process_csv(sys.argv[2])
    except Exception as e :
        sys.stderr.write(f"{e}")
        sys.exit(1)

    
                

                
                
                
            
        