import serial
import csv
import sys
import time

# Configuration
PORT = "/dev/ttyACM0"
BAUD = 115200
CSV_FILE = f"data/{int(time.time())}.csv"
NUM_VALUES = 6 # Number of features of the sensor (we don't care what they are, the NN will do the magic lol)

def main():
    header = ["gesture_name", "take_number", "seq_num"] + [f"val{i+1}" for i in range(NUM_VALUES)]

    ser = serial.Serial(PORT, BAUD, timeout=1)
    print(f"[INFO] Listening on {PORT} at {BAUD} baud")

    take_number = 0
    gesture_name = None
    seq_num = 0

    with open(CSV_FILE, "w", newline="") as f:
        writer = csv.writer(f)
        writer.writerow(header)

        try:
            while True:
                line = ser.readline().decode("utf-8", errors="ignore").strip()
                if not line:
                    continue

                if line.startswith("@"):
                    # Start of new gesture
                    gesture_name = line[1:].strip()
                    take_number += 1
                    seq_num = 0
                    print(f"[NEW] Gesture '{gesture_name}' Take {take_number}")

                elif line.startswith("#") and gesture_name is not None:
                    # Data line
                    parts = line.split("\t")
                    if len(parts) - 1 < NUM_VALUES:
                        print(f"[WARN] Not enough values in line: {line}")
                        continue

                    seq_num += 1
                    values = parts[1:1 + NUM_VALUES]
                    row = [gesture_name, take_number, seq_num] + values
                    writer.writerow(row)
                    f.flush()  # ensure write to disk

                    if seq_num%10==0: #Do not fill stdout with junk
                        print(f"[DATA] {gesture_name} T{take_number} Seq{seq_num}: {values}")

                else:
                    print(f"[IGNORED] {line}")

        except KeyboardInterrupt:
            print("\n[INFO] Stopping program.")
            sys.exit(0)

if __name__ == "__main__":
    main()
