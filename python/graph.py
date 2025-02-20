# import pandas as pd
# import matplotlib.pyplot as plt
# import math
# from matplotlib.ticker import FuncFormatter
# import os

# def MetricsGraph(filename: str, output_file_prefix: str, title, y_format, show: bool = False, cwd=os.path.dirname(__file__)):
#     # Load data from CSV file
#     csv_file_path = os.path.join(cwd, filename)
#     df = pd.read_csv(csv_file_path)

#     # Clean the "Queue" column
#     df["Queue"] = df["Queue"].astype(str).str.replace("/padded", "")
#     df.loc[df['Queue'].str.contains("BoundedMTQueue", na=None), 'Queue'] = 'BoundedFastflowQueue'
#     df.loc[df['Queue'].str.contains("MTQueue", na=None), 'Queue'] = 'LinkedFastflowQueue'

#     # Scale scores and error to millions
#     df["Score"] = df["Score"] / 1_000_000
#     df["Score Error"] = df["Score Error"] / 1_000_000

#     # Filter bounded and unbounded queues
#     bounded_df = df[df["Queue"].str.contains("Bounded", na=False) | df["Queue"].str.contains("All2All", na=False)]
#     unbounded_df = df[~df["Queue"].str.contains("Bounded", na=False) & ~df["Queue"].str.contains("All2All", na=False)]

#     def plot_grouped_data(data, group_title, output_suffix):
#     # Group by Size, Items, Runs, and Delay
#         grouped_data = data.groupby(['Size', 'Items', 'Runs', 'Delay'])
    
#         for (size, _, _, delay), group in grouped_data:
#             plt.figure(figsize=(10, 6))

#             # Calculate the sum values for the x-axis
#             sum_values = sorted((group[['Producers', 'Consumers']].sum(axis=1)).unique())
#             even_positions = range(len(sum_values))

#             # Plot each Queue with error bars
#             for queue in group["Queue"].unique():
#                 queue_data = group[group["Queue"] == queue]
#                 x_values = queue_data["Producers"] + queue_data["Consumers"]
#                 x_mapped = [even_positions[sum_values.index(val)] for val in x_values]
#                 plt.errorbar(x_mapped, queue_data["Score"],
#                             yerr=queue_data["Score Error"],
#                             label=queue, capsize=3, fmt='-o', markeredgewidth=2, linewidth=2)

#             # Set x-ticks to the evenly spaced positions with the actual values as labels
#             plt.xticks(even_positions, labels=[str(x) for x in sum_values], rotation=45)

#             # Set x-axis limits to match the evenly spaced positions
#             plt.xlim(-0.5, len(sum_values) - 0.5)

#             # Dynamically set yticks if not provided
#             max_y = group["Score"].max()
#             max_error = group["Score Error"].max()

#             # Compute y-ticks dynamically, e.g., in steps of 1 million
#             yticks = [1] + list(range(2, math.ceil(max_y) + 3, 2))  # Step of 2 million, adjust as needed

#             if(len(yticks) < 5):
#                 yticks = list(range(1, math.ceil(max_y) + 3, 1))

#             # Set custom y-ticks
#             plt.yticks(yticks, labels=[f"{y}.0 M" for y in yticks])

#             # Set y-limits to ensure proper visibility
#             plt.ylim(0, max(max_y + max_error,math.ceil(max_y)) + 0.5)  # Add the max error to max_y and give some padding

#             print(f"{output_file_prefix}_{output_suffix}_Size{size}_Delay{delay}.png")
#             print(group["Queue"].unique())
#             print()

#             # Labels and Title
#             plt.xlabel("Threads", fontsize=12)
#             plt.ylabel(y_format, fontsize=12)
#             plt.title(f"{group_title} - Delay {delay}", fontsize=14)
#             plt.legend(title="Queue", bbox_to_anchor=(1.05, 1), loc='upper left', fontsize=10)
#             plt.grid(True)

#             # Save the plot to file
#             output_file = f"{output_file_prefix}_{output_suffix}_Size{size}_Delay{delay}.png"
#             plt.tight_layout()
#             plt.savefig(output_file)

#             # Show the plot if requested
#             if show:
#                 plt.show()

#             # Close the plot to avoid overlap in subsequent plots
#             plt.close()


#     # Plot data for bounded and unbounded queues
#     plot_grouped_data(bounded_df, f"{title} (Bounded Queues)", "bound")
#     plot_grouped_data(unbounded_df, f"{title} (Unbounded Queues)", "unbounded")

# # Example usage:
# path = os.path.join(os.path.dirname(__file__), "../csv")  # path of csv files
# graphs_dir = os.path.join(os.path.dirname(__file__), "../graphs")  # path of graphs

# # Example usage to generate graphs
# MetricsGraph("../csv/ManyAll_low.csv", f"{graphs_dir}/ManyToMany", "Many To Many", "Throughput (Million ops/sec)", show=False, cwd=path)
import pandas as pd
import matplotlib.pyplot as plt
import math
from matplotlib.ticker import FuncFormatter
import os

# Global formatting map for queues
queue_formatting = {
    'LinkedFastflowQueue': {
        'legend_name': 'lFastflow',
        'color': '#2C3E50',  # Dark blue-gray for better contrast
        'linestyle': '-',
        'marker': 'o',  # Simple circle marker
        'linewidth': 2,
        'markersize': 8,  # Larger marker for visibility
        'capsize': 3
    },
    'LinkedCRQueue': {
        'legend_name': 'lCRQ',
        'color': '#16A085',  # Teal green (muted)
        'linestyle': '-',  # Dashed line
        'marker': 's',  # Square marker
        'linewidth': 2,
        'markersize': 8,
        'capsize': 3
    },
    'LinkedPRQueue': {
        'legend_name': 'lPRQ',
        'color': '#E74C3C',  # Muted red
        'linestyle': '-',  # Dash-dot line
        'marker': '^',  # Triangle marker
        'linewidth': 2,
        'markersize': 8,
        'capsize': 3
    },
    'LinkedMuxQueue': {
        'legend_name': 'lMutex',
        'color': '#8E44AD',  # Dark purple
        'linestyle': '-',  # Dotted line
        'marker': 'D',  # Diamond marker
        'linewidth': 2,
        'markersize': 8,
        'capsize': 3
    },
    'FAAArrayQueue': {
        'legend_name': 'FAAarray',
        'color': '#F39C12',  # Golden orange
        'linestyle': '-',  # Solid line
        'marker': 'x',  # Cross marker
        'linewidth': 2,
        'markersize': 8,
        'capsize': 3
    },
    'BoundedFastflowQueue': {
        'legend_name': 'bFastflow',
        'color': '#34495E',  # Darker blue-gray
        'linestyle': '-',  # Solid line
        'marker': 'o',  # Circle marker
        'linewidth': 2,
        'markersize': 8,
        'capsize': 3
    },
    'BoundedMuxQueue': {
        'legend_name': 'bMutexQ',
        'color': '#1ABC9C',  # Muted cyan green
        'linestyle': '-',  # Dashed line
        'marker': 's',  # Square marker
        'linewidth': 2,
        'markersize': 8,
        'capsize': 3
    },
    'BoundedItemCRQueue': {
        'legend_name': 'biCRQ',
        'color': '#C0392B',  # Brick red
        'linestyle': '-',  # Dash-dot line
        'marker': '^',  # Triangle marker
        'linewidth': 2,
        'markersize': 8,
        'capsize': 3
    },
    'BoundedSegmentCRQueue': {
        'legend_name': 'bsCRQ',
        'color': '#9B59B6',  # Lavender purple
        'linestyle': '-',  # Dotted line
        'marker': 'D',  # Diamond marker
        'linewidth': 2,
        'markersize': 8,
        'capsize': 3
    },
    'BoundedItemPRQueue': {
        'legend_name': 'biPRQ',
        'color': '#F39C12',  # Golden yellow (distinct from Item CR Queue)
        'linestyle': '-',  # Solid line
        'marker': 'x',  # Cross marker
        'linewidth': 2,
        'markersize': 8,
        'capsize': 3
    },
    'BoundedSegmentPRQueue': {
        'legend_name': 'bsPRQ',
        'color': '#7F8C8D',  # Muted gray
        'linestyle': '-',  # Dashed line
        'marker': 'P',  # Plus marker
        'linewidth': 2,
        'markersize': 8,
        'capsize': 3
    },
    'All2All': {
        'legend_name': 'All2All',
        'color': '#1F77B4',  # Standard blue
        'linestyle': '-',  # Dash-dot line
        'marker': '*',  # Star marker
        'linewidth': 2,
        'markersize': 8,
        'capsize': 3
    }
}


def MetricsGraph(filename: str, output_file_prefix: str, title, y_format, show: bool = False, cwd=os.path.dirname(__file__)):
    # Load data from CSV file
    csv_file_path = os.path.join(cwd, filename)
    df = pd.read_csv(csv_file_path)

    # Clean the "Queue" column
    df["Queue"] = df["Queue"].astype(str).str.replace("/padded", "")
    df.loc[df['Queue'].str.contains("BoundedMTQueue", na=None), 'Queue'] = 'BoundedFastflowQueue'
    df.loc[df['Queue'].str.contains("MTQueue", na=None), 'Queue'] = 'LinkedFastflowQueue'

    # Scale scores and error to millions
    df["Score"] = df["Score"] / 1_000_000
    df["Score Error"] = df["Score Error"] / 1_000_000

    # Filter bounded and unbounded queues
    bounded_df = df[df["Queue"].str.contains("Bounded", na=False) | df["Queue"].str.contains("All2All", na=False)]
    unbounded_df = df[~df["Queue"].str.contains("Bounded", na=False) & ~df["Queue"].str.contains("All2All", na=False)]

    def plot_grouped_data(data, group_title, output_suffix):
        # Group by Size, Items, Runs, and Delay
        grouped_data = data.groupby(['Size', 'Items', 'Runs', 'Delay'])
        
        for (size, _, _, delay), group in grouped_data:
            plt.figure(figsize=(10, 6))

            # Calculate the sum values for the x-axis
            sum_values = sorted((group[['Producers', 'Consumers']].sum(axis=1)).unique())
            even_positions = range(len(sum_values))

            # Plot each Queue with error bars, using the global formatting map
            for queue in group["Queue"].unique():
                queue_data = group[group["Queue"] == queue]
                
                # Fetch the formatting settings from the global map
                format_settings = queue_formatting.get(queue, {})
                legend_name = format_settings.get('legend_name', queue)  # Default to queue name if not found
                color = format_settings.get('color', 'black')
                linestyle = format_settings.get('linestyle', '-')
                marker = format_settings.get('marker', 'o')
                linewidth = format_settings.get('linewidth', 2)
                markersize = format_settings.get('markersize', 6)
                capsize = format_settings.get('capsize', 3)

                x_values = queue_data["Producers"] + queue_data["Consumers"]
                x_mapped = [even_positions[sum_values.index(val)] for val in x_values]
                plt.errorbar(x_mapped, queue_data["Score"],
                            yerr=queue_data["Score Error"],
                            label=legend_name, capsize=capsize, fmt=marker, 
                            color=color, linestyle=linestyle, markeredgewidth=2, 
                            linewidth=linewidth, markersize=markersize)

            # Set x-ticks to the evenly spaced positions with the actual values as labels
            plt.xticks(even_positions, labels=[str(x) for x in sum_values], rotation=45)

            # Set x-axis limits to match the evenly spaced positions
            plt.xlim(-0.5, len(sum_values) - 0.5)

            # Dynamically set yticks if not provided
            max_y = group["Score"].max()
            max_error = group["Score Error"].max()

            yticks = [1] + list(range(2, math.ceil(max_y) + 3, 2))  # Step of 2 million, adjust as needed
            if(len(yticks) < 5):
                yticks = list(range(1, math.ceil(max_y) + 3, 1))

            plt.yticks(yticks, labels=[f"{y}.0 M" for y in yticks])

            # Set y-limits to ensure proper visibility
            plt.ylim(0, max(max_y + max_error, math.ceil(max_y)) + 0.5)

            # Labels and Title
            plt.xlabel("Threads", fontsize=12)
            plt.ylabel(y_format, fontsize=12)
            plt.title(f"{group_title} - Size {size} - Delay {delay}", fontsize=14)
            plt.legend(title="Queue", bbox_to_anchor=(1.05, 1), loc='upper left', fontsize=10)
            plt.grid(True)

            # Save the plot to file
            output_file = f"{output_file_prefix}_{output_suffix}_Size{size}_Delay{delay}.png"
            plt.tight_layout()
            plt.savefig(output_file)

            # Show the plot if requested
            if show:
                plt.show()

            # Close the plot to avoid overlap in subsequent plots
            plt.close()

    # Plot data for bounded and unbounded queues
    plot_grouped_data(bounded_df, f"{title} (Bounded Queues)", "bound")
    plot_grouped_data(unbounded_df, f"{title} (Unbounded Queues)", "unbounded")

# Example usage:
path = os.path.join(os.path.dirname(__file__), "../csv")  # path of csv files
graphs_dir = os.path.join(os.path.dirname(__file__), "../graphs")  # path of graphs

# Example usage to generate graphs
MetricsGraph("../csv/ManyAll_low.csv", f"{graphs_dir}/ManyToMany", "Many To Many", "Throughput (Million ops/sec)", show=False, cwd=path)
