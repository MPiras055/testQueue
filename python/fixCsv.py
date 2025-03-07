import csv

def fix_csv(csv_file_path: str) -> None:
    with open(csv_file_path, mode="r") as file:
        reader = csv.reader(file)
        rows = list(reader)

    with open("../csv/OTM.csv", mode="w",newline="") as outfile:
        # Write the header
        outfile.write(",".join(rows[0])+"\n")

        # process each line
        for i in range(1,len(rows)):
            row = rows[i]
            #split the row in individual values
            values = row
           
            values[5] = str(1)
            values[7] = str(float(int(float(values[7]))))
            values[8] = str(float(0))

            print(values)

            # write the modified row
            outfile.write(",".join(values)+"\n")

fix_csv("../csv/OneToMany_broken.csv")