import matplotlib.pyplot as plt

# Simulated data (Threads vs Execution Time)
threads = [1, 2, 4, 8, 16, 32, 64]
execution_times = [1.00, 0.60, 0.35, 0.25, 0.30, 0.45, 0.80]

# Calculate speedup
speedup = [execution_times[0] / time for time in execution_times]

# Plot Execution Time vs Threads
plt.figure(figsize=(10, 5))

# Subplot for Execution Time vs Threads
plt.subplot(1, 2, 1)
plt.plot(threads, execution_times, marker='o', color='b', label='Execution Time')
plt.xscale('log')  # Log scale for x-axis
plt.yscale('log')  # Log scale for y-axis
plt.title('Execution Time vs Number of Threads (Bad Scalability)')
plt.xlabel('Number of Threads')
plt.ylabel('Execution Time (s)')
plt.grid(True)

# Subplot for Speedup vs Threads
plt.subplot(1, 2, 2)
plt.plot(threads, speedup, marker='o', color='r', label='Speedup')
plt.xscale('log')  # Log scale for x-axis
plt.yscale('linear')  # Speedup vs Threads
plt.title('Speedup vs Number of Threads (Bad Scalability)')
plt.xlabel('Number of Threads')
plt.ylabel('Speedup')
plt.grid(True)

# Adjust layout and show the plots
plt.tight_layout()
plt.show()
