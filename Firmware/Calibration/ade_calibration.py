import serial
import pandas as pd
import matplotlib.pyplot as plt
from datetime import datetime
import time
import math

PORT = "COM11"      # Ensure this matches your ESP32 port
BAUD = 115200

# Global session variables to carry calculations between stages
session_constants = {
    "v_const": 0.0,
    "i_const": 0.0,
    "wh_lsb": 0.0
}

def collect_data(duration_sec, stage_name):
    rows = []
    print(f"\n---> Starting data collection for '{stage_name}' ({duration_sec} seconds)...")
    
    try:
        with serial.Serial(PORT, BAUD, timeout=2) as ser:
            ser.reset_input_buffer()
            start_time = time.time()
            
            while (time.time() - start_time) < duration_sec:
                line = ser.readline().decode(errors="ignore").strip()
                
                if not line or line.startswith(("\x1b", "I ", "W ", "E ", "t_ms", "CRITICAL")):
                    continue
                
                try:
                    parts = line.split(",")
                    # Handle both 10-column standard and 12-column headroom modes
                    if len(parts) >= 10:
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
                        if len(parts) >= 12:
                            row["vpeak_raw"] = int(parts[10])
                            row["iapeak_raw"] = int(parts[11])
                        rows.append(row)
                except ValueError:
                    pass

    except serial.SerialException as e:
        print(f"Serial Error: {e}. Is the port correct and closed elsewhere?")
        return None

    if not rows:
        print("No valid data received!")
        return None

    df = pd.DataFrame(rows)
    filename = f"{stage_name.replace(' ', '_').lower()}_{datetime.now().strftime('%Y%m%d_%H%M%S')}.csv"
    df.to_csv(filename, index=False)
    print(f"Saved {len(df)} samples to {filename}")
    return df

def plot_data(df, stage_name):
    t0 = df["t_ms"].iloc[0]
    df["t_rel_s"] = (df["t_ms"] - t0) / 1000.0

    fig, axes = plt.subplots(3, 1, figsize=(10, 8), sharex=True)
    fig.suptitle(f"AICE Calibration: {stage_name}")

    axes[0].plot(df["t_rel_s"], df["vrms_raw"], label="VRMS Raw", color='blue')
    axes[0].legend(); axes[0].grid(True)

    axes[1].plot(df["t_rel_s"], df["irmsa_raw"], label="IRMSA Raw", color='orange')
    axes[1].legend(); axes[1].grid(True)

    axes[2].plot(df["t_rel_s"], df["awatt_raw"], label="AWATT Raw", color='green')
    axes[2].set_xlabel("Time (seconds)")
    axes[2].legend(); axes[2].grid(True)

    plt.tight_layout()
    plt.show(block=False)

def main():
    print("=== AICE SmartPlug Calibration Tool (Register Method) ===")
    
    while True:
        print("\n--- Calibration Menu ---")
        print("1. Verify Raw Signals & Headroom")
        print("2. Calibrate VRMS (K_V)")
        print("3. Calibrate IRMSA (K_I)")
        print("4. Calculate Wh/LSB Constant (PF=1 Load)")
        print("5. Calibrate AWGAIN (Requires Wh/LSB)")
        print("6. Phase Calibration PHCALA (Requires Inductive PF=0.5 Load)")
        print("7. Stream 7kHz Waveform (Requires Firmware toggle to 1)")
        print("0. Exit")
        
        choice = input("Select Stage (0-7): ")
        
        if choice == '0':
            break
            
        elif choice == '1':
            df = collect_data(5, "Raw Verification")
            if df is not None and "vpeak_raw" in df.columns:
                max_v = df["vpeak_raw"].max()
                max_i = df["iapeak_raw"].max()
                print(f"Max VPEAK: {max_v} ({(max_v/5326510)*100:.1f}% of ADC limit)")
                print(f"Max IAPEAK: {max_i} ({(max_i/5326510)*100:.1f}% of ADC limit)")
                plot_data(df, "Raw Signals")
                
        elif choice == '2':
            ref_v = float(input("Enter Multimeter Voltage (VRMS): "))
            df = collect_data(10, "Voltage Cal")
            if df is not None:
                session_constants["v_const"] = ref_v / df["vrms_raw"].mean()
                print(f"VRMS Constant (K_V): {session_constants['v_const']:.8f} V/LSB")
                
        elif choice == '3':
            ref_i = float(input("Enter Clamp Meter Current (ARMS): "))
            df = collect_data(10, "Current Cal")
            if df is not None:
                session_constants["i_const"] = ref_i / df["irmsa_raw"].mean()
                print(f"IRMSA Constant (K_I): {session_constants['i_const']:.8f} A/LSB")

        elif choice == '4':
            ref_w = float(input("Enter True Active Power (Watts) of Resistive Load: "))
            duration = 15
            df = collect_data(duration, "Wh LSB Cal")
            if df is not None:
                # Sum the energy deltas over the timeframe
                total_raw_energy = df["aenergy_raw"].sum()
                real_wh_accumulated = (ref_w * duration) / 3600.0
                
                if total_raw_energy > 0:
                    session_constants["wh_lsb"] = real_wh_accumulated / total_raw_energy
                    print(f"Total Raw Energy Sum: {total_raw_energy}")
                    print(f"Real Wh Accumulated: {real_wh_accumulated:.6f} Wh")
                    print(f"--> Wh/LSB Constant: {session_constants['wh_lsb']:.8e}")
                else:
                    print("Error: 0 raw energy accumulated. Is the load drawing power?")

        elif choice == '5':
            if session_constants["wh_lsb"] == 0.0:
                print("Please calculate Wh/LSB (Option 4) first!")
                continue
            
            ref_w = float(input("Enter True Active Power (Watts) of Resistive Load: "))
            duration = 10
            df = collect_data(duration, "AWGAIN Cal")
            if df is not None:
                actual_raw = df["aenergy_raw"].sum()
                expected_raw = ((ref_w * duration) / 3600.0) / session_constants["wh_lsb"]
                
                if actual_raw > 0:
                    awgain = 0x400000 * (expected_raw / actual_raw)
                    print(f"Expected Raw: {expected_raw:.1f} | Actual Raw: {actual_raw}")
                    print(f"--> Write to AWGAIN Register: 0x{int(awgain):06X}")
                else:
                    print("Error: No raw energy detected.")

        elif choice == '6':
            print("Connect an INDUCTIVE load (PF around 0.5 Lagging).")
            ref_pf = float(input("Enter exact True Power Factor (e.g., 0.52): "))
            ref_w = float(input("Enter True Active Power (Watts): "))
            duration = 15
            df = collect_data(duration, "Phase Cal")
            if df is not None:
                actual_raw = df["aenergy_raw"].sum()
                expected_raw = ((ref_w * duration) / 3600.0) / session_constants["wh_lsb"]
                
                if actual_raw > 0 and expected_raw > 0:
                    # AN-1118 Phase Calibration Formula
                    phi_rad = math.acos(ref_pf)
                    phi_deg = math.degrees(phi_rad)
                    
                    ratio = (actual_raw * ref_pf) / expected_raw
                    # Constrain domain for acos to prevent math domain errors from noise
                    ratio = max(min(ratio, 1.0), -1.0) 
                    
                    error_angle_deg = math.degrees(math.acos(ratio)) - phi_deg
                    
                    phcal = -1 * (error_angle_deg / (360.0 * 60.0)) * 893850
                    
                    # Convert to 10-bit sign-magnitude format
                    phcal_int = int(phcal)
                    if phcal_int < 0:
                        phcal_hex = 0x200 | abs(phcal_int) # Set sign bit
                    else:
                        phcal_hex = phcal_int
                        
                    print(f"Phase Error Calculated: {error_angle_deg:.4f} degrees")
                    print(f"--> Write to PHCALA Register: 0x{phcal_hex:03X}")

        elif choice == '7':
            # Waveform streaming block remains unchanged...
            print("\n---> Streaming live 7kHz waveforms. Press Ctrl+C to stop...")
            wave_rows = []
            try:
                with serial.Serial(PORT, BAUD, timeout=1) as ser:
                    ser.reset_input_buffer()
                    while True:
                        line = ser.readline().decode(errors="ignore").strip()
                        if not line or line.startswith("WAVE"): continue
                        try:
                            parts = line.split(",")
                            if len(parts) == 2:
                                wave_rows.append({"V_wave": int(parts[0]), "I_wave": int(parts[1])})
                                if len(wave_rows) >= 1000: break
                        except ValueError: pass
            except KeyboardInterrupt: pass

            if wave_rows:
                w_df = pd.DataFrame(wave_rows)
                fig, axes = plt.subplots(2, 1, figsize=(10, 6))
                axes[0].plot(w_df["V_wave"], color='blue', label='Voltage Sine Wave')
                axes[0].grid(True); axes[0].legend()
                axes[1].plot(w_df["I_wave"], color='red', label='Current Sine Wave')
                axes[1].grid(True); axes[1].legend()
                plt.show()

if __name__ == "__main__":
    main()