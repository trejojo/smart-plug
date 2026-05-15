import serial
import pandas as pd
import matplotlib.pyplot as plt
from datetime import datetime
import time

PORT = "COM5"      # Change to your ESP32 port
BAUD = 115200
COLUMNS = ["t_ms", "vrms_raw", "irmsa_raw", "irmsb_raw", "awatt_raw", "avar_raw", "ava_raw", "aenergy_raw", "pf_raw", "period_raw"]

def collect_data(duration_sec, stage_name):
    rows = []
    print(f"\n---> Starting data collection for '{stage_name}' ({duration_sec} seconds)...")
    
    try:
        with serial.Serial(PORT, BAUD, timeout=2) as ser:
            # Flush existing buffer
            ser.reset_input_buffer()
            start_time = time.time()
            
            while (time.time() - start_time) < duration_sec:
                line = ser.readline().decode(errors="ignore").strip()
                
                # Filter out standard ESP logging or empty lines
                if not line or line.startswith(("\x1b", "I ", "W ", "E ", "t_ms")):
                    continue
                
                try:
                    parts = line.split(",")
                    if len(parts) == 10:
                        row = {
                            "t_ms": int(parts[0]),
                            "vrms_raw": int(parts[1]),
                            "irmsa_raw": int(parts[2]),
                            "irmsb_raw": int(parts[3]),
                            "awatt_raw": int(parts[4]),
                            "avar_raw": int(parts[5]),
                            "ava_raw": int(parts[6]),
                            "aenergy_raw": int(parts[7]),
                            "pf_raw": int(parts[8]),
                            "period_raw": int(parts[9]),
                        }
                        rows.append(row)
                except ValueError:
                    pass # Malformed line, skip

    except serial.SerialException as e:
        print(f"Serial Error: {e}. Is the port correct and not open in another program?")
        return None

    if not rows:
        print("No valid data received!")
        return None

    df = pd.DataFrame(rows)
    # Save to CSV
    filename = f"{stage_name.replace(' ', '_').lower()}_{datetime.now().strftime('%Y%m%d_%H%M%S')}.csv"
    df.to_csv(filename, index=False)
    print(f"Saved {len(df)} samples to {filename}")
    return df

def plot_data(df, stage_name):
    # Normalize time to start at 0
    t0 = df["t_ms"].iloc[0]
    df["t_rel_s"] = (df["t_ms"] - t0) / 1000.0

    fig, axes = plt.subplots(3, 1, figsize=(10, 8), sharex=True)
    fig.suptitle(f"AICE Calibration: {stage_name}")

    axes[0].plot(df["t_rel_s"], df["vrms_raw"], label="VRMS Raw", color='blue')
    axes[0].legend()
    axes[0].grid(True)

    axes[1].plot(df["t_rel_s"], df["irmsa_raw"], label="IRMSA Raw", color='orange')
    axes[1].legend()
    axes[1].grid(True)

    axes[2].plot(df["t_rel_s"], df["awatt_raw"], label="AWATT Raw", color='green')
    axes[2].set_xlabel("Time (seconds)")
    axes[2].legend()
    axes[2].grid(True)

    plt.tight_layout()
    plt.show(block=False) # Non-blocking so menu continues

def calculate_constant(df, column, reference_val):
    avg_raw = df[column].mean()
    if avg_raw == 0:
        return 0
    constant = reference_val / avg_raw
    print(f"Average {column}: {avg_raw:.2f}")
    print(f"Calculated Constant (Ref / Raw): {constant:.8f}")
    return constant

def main():
    print("=== AICE SmartPlug Calibration Tool ===")
    
    while True:
        print("\nSelect Calibration Stage:")
        print("1. No load, relay open (Offset Check)")
        print("2. Known voltage, no current (VRMS Cal)")
        print("3. Known resistive load (IRMSA, AWATT, PF Cal)")
        print("4. Inductive/capacitive load (VAR, PF Validate)")
        print("5. Overload/sag test")
        print("6. Long run (Wh Validate)")
        print("0. Exit")
        
        choice = input("Enter choice (0-6): ")
        
        if choice == '0':
            break
            
        elif choice == '1':
            df = collect_data(10, "No Load Offset")
            if df is not None:
                plot_data(df, "No Load Offset")
                print("Observe the plotted offsets. If IRMS/WATT hover above 0 significantly, firmware offset registers may be needed.")
                
        elif choice == '2':
            ref_v = float(input("Enter actual multimeter voltage (e.g. 120.5): "))
            df = collect_data(10, "Voltage Calibration")
            if df is not None:
                plot_data(df, "Voltage Calibration")
                calculate_constant(df, "vrms_raw", ref_v)
                
        elif choice == '3':
            ref_i = float(input("Enter actual multimeter current (Amps): "))
            ref_w = float(input("Enter actual reference power (Watts): "))
            df = collect_data(15, "Resistive Load")
            if df is not None:
                plot_data(df, "Resistive Load")
                calculate_constant(df, "irmsa_raw", ref_i)
                calculate_constant(df, "awatt_raw", ref_w)
                
        elif choice == '4':
            df = collect_data(15, "Reactive Load")
            if df is not None:
                plot_data(df, "Reactive Load")
                print("Review the plot. Check if AVAR represents the expected reactive power.")
                
        elif choice == '5':
            print("Toggle the relay/load physically or via the ESP button to trigger sag/overload.")
            df = collect_data(20, "Overload Sag")
            if df is not None:
                plot_data(df, "Overload Sag")
                print(f"Max IRMS observed: {df['irmsa_raw'].max()}")
                print(f"Min VRMS observed: {df['vrms_raw'].min()}")
                
        elif choice == '6':
            duration = int(input("Enter duration in seconds (e.g. 3600 for 1 hr): "))
            df = collect_data(duration, "Long Run")
            if df is not None:
                df["energy_accumulated"] = df["aenergy_raw"].cumsum()
                plt.figure()
                plt.plot((df["t_ms"] - df["t_ms"].iloc[0])/1000, df["energy_accumulated"])
                plt.title("Accumulated Raw Energy (aenergy_a_delta sum)")
                plt.xlabel("Time (s)")
                plt.grid(True)
                plt.show(block=False)

if __name__ == "__main__":
    main()