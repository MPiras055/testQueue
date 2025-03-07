import numpy as np
import pandas as pd
import matplotlib.pyplot as plt
import os

def MetricsGraph(filename: str, output_file_prefix: str, title, runSize, show: bool = False, cwd=os.path.dirname(__file__)):
    # Load data from CSV file
    csv_file_path = os.path.join(cwd, filename)
    df = pd.read_csv(csv_file_path)

    # Clean the "Queue" column
    df["Queue"] = df["Queue"].astype(str).str.replace("/padded", "")
    df.loc[df['Queue'].str.contains("BoundedMTQueue", na=False), 'Queue'] = 'BoundedFastflowQueue'
    df.loc[df['Queue'].str.contains("MTQueue", na=False), 'Queue'] = 'LinkedFastflowQueue'

    # Handle Score == 0 by setting Time to NaN or any value you'd like
    df["Time"] = df["Score"].apply(lambda score: runSize / score if score != 0 else np.nan)

    # Find the sequential execution time (1 Producer, 1 Consumer)
    baseline_df = df[(df["Producers"] == 1) & (df["Consumers"] == 1)]
    
    if baseline_df.empty:
        print("Warning: No baseline data found (1 Producer, 1 Consumer). Skipping scalability computation.")
        return
    
    # Create a dictionary mapping (Queue, Size, Delay) -> Baseline time
    baseline_times = baseline_df.set_index(["Queue", "Size", "Delay"])["Time"].to_dict()

    # Compute Scalability
    def compute_scalability(row):
        # Check if Time is NaN (due to division by zero)
        if np.isnan(row["Time"]):
            return None  # Skip rows with NaN Time
        key = (row["Queue"], row["Size"], row["Delay"])
        if key in baseline_times:
            return baseline_times[key] / row["Time"]  # Scalability formula
        return None  # If no baseline found, return None
    
    df["Scalability"] = df.apply(compute_scalability, axis=1)

    # Filter bounded and unbounded queues
    bounded_df = df[df["Queue"].str.contains("Bounded", na=False) | df["Queue"].str.contains("All2All", na=False)]
    unbounded_df = df[~df["Queue"].str.contains("Bounded", na=False) & ~df["Queue"].str.contains("All2All", na=False)]

    def plot_grouped_data(data, group_title, output_suffix):
        grouped_data = data.groupby(['Size', 'Items', 'Runs', 'Delay'])
        
        for (size, _, _, delay), group in grouped_data:
            plt.figure(figsize=(10, 6))
            
            sum_values = sorted((group[['Producers', 'Consumers']].sum(axis=1)).unique())
            even_positions = range(len(sum_values))
            
            for queue in group["Queue"].unique():
                queue_data = group[group["Queue"] == queue]
                print(queue)
                
                x_values = queue_data["Producers"] + queue_data["Consumers"]
                x_mapped = [even_positions[sum_values.index(val)] for val in x_values]
                
                plt.errorbar(x_mapped, queue_data["Scalability"],
                            label=queue, capsize=3, fmt='o',
                            linestyle='-', markeredgewidth=2,
                            linewidth=2, markersize=8)
                
            
            plt.xticks(even_positions, labels=[str(x) for x in sum_values], rotation=45)
            plt.xlim(-0.5, len(sum_values) - 0.5)
            
            plt.ylabel("Scalability (Speedup over Sequential)", fontsize=12)
            plt.xlabel("Number of Workers (Threads)", fontsize=12)
            plt.title(f"{group_title} - Size {size} - Delay {delay}", fontsize=14)
            plt.legend(title="Queue", bbox_to_anchor=(1.05, 1), loc='upper left', fontsize=10)
            plt.grid(True, which="both", linestyle="--", linewidth=0.5)
            
            output_file = f"{output_file_prefix}_{output_suffix}_Size{size}_Delay{delay}.svg"
            plt.tight_layout()
            plt.savefig(output_file, format='svg')
            
            if show:
                plt.show()
            
            plt.close()

    print(bounded_df["Queue"].unique())
    plot_grouped_data(bounded_df, f"{title} (Bounded Queues)", "bound")
    plot_grouped_data(unbounded_df, f"{title} (Unbounded Queues)", "unbounded")

# Example usage:
path = os.path.join(os.path.dirname(__file__), "../csv")
graphs_dir = os.path.join(os.path.dirname(__file__), "../graphs")

runSize = 1_000_000  # Define the run size in operations
#MetricsGraph("../csv/OneToMany.csv", f"{graphs_dir}/OneToMany", "One To Many", runSize, show=False, cwd=path)
MetricsGraph("../csv/OTM.csv", f"{graphs_dir}/OneToMany", "One To Many", runSize, show=False, cwd=path)
MetricsGraph("../csv/ManyToOne.csv", f"{graphs_dir}/ManyToOne", "Many To One", runSize, show=False, cwd=path)
