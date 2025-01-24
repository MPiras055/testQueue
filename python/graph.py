import pandas as pd
import matplotlib.pyplot as plt
from matplotlib.ticker import FuncFormatter
import os,glob

XTICKS_POSITION = [1,2,4,8,16,32,64,128,256,512,1024]

def MetricsGraph(filename: str, output_file_prefix: str, title, y_format, show: bool = False,cwd=os.path.dirname(__file__)):
    import pandas as pd
    import matplotlib.pyplot as plt
    from matplotlib.ticker import FuncFormatter
    import os

    # Load data from CSV file
    csv_file_path = os.path.join(cwd,filename)
    df = pd.read_csv(csv_file_path)

    # Clean the "Queue" column
    df["Queue"] = df["Queue"].astype(str).str.replace("/padded", "")
    df.loc[df['Queue'].str.contains("BoundedMTQueue", na=None), 'Queue'] = 'BoundedFastflowQueue'
    df.loc[df['Queue'].str.contains("MTQueue", na=None), 'Queue'] = 'LinkedFastflowQueue'

    # Scale scores and error to millions
    df["Score"] = df["Score"] / 1_000_000
    df["Score Error"] = df["Score Error"] / 1_000_000

    # Filter bounded and unbounded queues
    bounded_df = df[df["Queue"].str.contains("Bounded", na=False)]
    unbounded_df = df[~df["Queue"].str.contains("Bounded", na=False)]

    # Helper function to plot data
    def plot_grouped_data(data, group_title, output_suffix):
        grouped_data = data.groupby(['Size', 'Items', 'Runs'])
        for (size, items, runs), group in grouped_data:
            plt.figure(figsize=(10, 6))

            # Calculate the sum values for the x-axis
            sum_values = sorted((group[['Producers', 'Consumers']].sum(axis=1)).unique())
            even_positions = range(len(sum_values))

            # Plot each Queue with error bars
            for queue in group["Queue"].unique():
                queue_data = group[group["Queue"] == queue]
                x_values = queue_data["Producers"] + queue_data["Consumers"]
                x_mapped = [even_positions[sum_values.index(val)] for val in x_values]
                plt.errorbar(x_mapped, queue_data["Score"],
                             yerr=queue_data["Score Error"],
                             label=queue, capsize=3, fmt='-o', markeredgewidth=2, linewidth=2)

            # Set x-ticks to the evenly spaced positions with the actual values as labels
            plt.xticks(even_positions, labels=[str(x) for x in sum_values], rotation=45)

            # Set x-axis limits to match the evenly spaced positions
            plt.xlim(-0.5, len(sum_values) - 0.5)

            # Custom y-axis formatter
            def y_formatter(x, pos):
                return f"{x}M"

            # Set the custom y-axis formatter
            plt.gca().get_yaxis().set_major_formatter(FuncFormatter(y_formatter))

            # Dynamically set yticks if not provided
            max_y = group["Score"].max()
            rounded_max_y = (int((max_y + 9) // 10) * 10)
            yticks = [1] + list(range(2, rounded_max_y, 2))

            # Set custom y-ticks
            plt.yticks(yticks)

            # Labels and Title
            plt.xlabel("Threads", fontsize=12)
            plt.ylabel(y_format, fontsize=12)
            plt.title(f"{group_title}", fontsize=14)
            plt.legend(title="Queue Type", bbox_to_anchor=(1.05, 1), loc='upper left', fontsize=10)
            plt.grid(True)

            # Save the plot to file
            output_file = f"{output_file_prefix}_{output_suffix}_Size{size}.png"
            plt.tight_layout()
            plt.savefig(output_file)

            # Show the plot if requested
            if show:
                plt.show()

            # Close the plot to avoid overlap in subsequent plots
            plt.close()

    # Plot data for bounded and unbounded queues
    plot_grouped_data(bounded_df, f"{title} (Bounded Queues)", "bound")
    plot_grouped_data(unbounded_df, f"{title} (Unbounded Queues)", "unboound")


path = os.path.join(os.path.dirname(__file__),"../csv") #path of csv files
graphs_dir = os.path.join(os.path.dirname(__file__),"../graphs") #path of graphs

# print all graphs
MetricsGraph("ManyToMany.csv",f"{graphs_dir}/ManyToMany","Many To Many","Throughput (Million ops/sec)", show=False,cwd=path)
MetricsGraph("ManyToOne.csv",f"{graphs_dir}/ManyToOne","Many To One","Throughput (Million ops/sec)", show=False,cwd=path)
MetricsGraph("OneToMany.csv",f"{graphs_dir}/OneToMany","One To Many","Throughput (Million ops/sec)", show=False,cwd=path)
MetricsGraph("OneToOne.csv",f"{graphs_dir}/OneToOne","One To One","Throughput (Million ops/sec)", show=False,cwd=path)

