import tkinter as tk
from tkinter import ttk
import queue
import threading
import json
import time
import random
from datetime import datetime

# ==========================================
# 1. Dummy Simulator (From previous step)
# ==========================================
class DummySmartPlugSimulator:
    def __init__(self):
        self.relay_state = 1
        self.system_locked = False

    def _get_timestamp(self):
        return datetime.now().strftime("%Y-%m-%d %H:%M:%S.%f")[:-3]

    def generate_telemetry(self):
            v_rms = round(random.uniform(118.0, 122.5), 1)
            if self.relay_state == 1:
                i_rms = round(random.uniform(0.1, 5.5), 2)
                pf = round(random.uniform(0.85, 0.99), 2)
            else:
                i_rms = 0.0
                pf = 0.0
            thd = round(random.uniform(0.01, 0.08), 2)

            return json.dumps({
                "event_type": "HEARTBEAT",
                "timestamp": self._get_timestamp(),
                "metrics": {
                    "v_rms": v_rms, 
                    "i_rms": i_rms, 
                    "pf": pf, 
                    "thd": thd,
                    "zero_cross_count": 60 # Added zero cross count
                },
                "relay_state": self.relay_state
            })

    def generate_alert(self):
        self.relay_state = 0 
        self.system_locked = True
        return json.dumps({
            "event_type": "CRITICAL_PROTECTION",
            "cause": "OVERCURRENT_SAG",
            "timestamp": self._get_timestamp(),
            "data": {"peak_value": 25.4, "unit": "Amperes"},
            "action_taken": "RELAY_OPEN",
            "system_status": "LOCKED_AWAITING_ACK"
        })

# ==========================================
# 2. Tkinter GUI Application
# ==========================================
class SmartPlugGUI:
    def __init__(self, root):
        self.root = root
        self.root.title("SmartPlug Telemetry Dashboard")
        self.root.geometry("800x500")
        
        # Queue for thread-safe communication
        self.data_queue = queue.Queue()
        self.simulator = DummySmartPlugSimulator()

        self._build_ui()
        
        # Start the background dummy data thread
        self.running = True
        self.sim_thread = threading.Thread(target=self._dummy_data_loop, daemon=True)
        self.sim_thread.start()

        # Start the Tkinter polling loop (checks the queue every 100ms)
        self.root.after(100, self._process_queue)

    def _build_ui(self):
            """Constructs the 4 main panels."""
            # --- Top Left: Telemetry Dashboard ---
            frame_telemetry = ttk.LabelFrame(self.root, text="Live Telemetry")
            frame_telemetry.grid(row=0, column=0, padx=10, pady=10, sticky="nsew")
            
            self.lbl_v = ttk.Label(frame_telemetry, text="Voltage: -- V", font=("Arial", 14))
            self.lbl_v.pack(anchor="w", padx=5, pady=2)
            
            self.lbl_i = ttk.Label(frame_telemetry, text="Current: -- A", font=("Arial", 14))
            self.lbl_i.pack(anchor="w", padx=5, pady=2)
            
            self.lbl_pf = ttk.Label(frame_telemetry, text="Power Factor: --", font=("Arial", 12))
            self.lbl_pf.pack(anchor="w", padx=5, pady=2)
            
            self.lbl_thd = ttk.Label(frame_telemetry, text="THD: --", font=("Arial", 12))
            self.lbl_thd.pack(anchor="w", padx=5, pady=2)
            
            self.lbl_zcc = ttk.Label(frame_telemetry, text="Zero Crossings: --", font=("Arial", 12))
            self.lbl_zcc.pack(anchor="w", padx=5, pady=2)
            
            self.lbl_relay = ttk.Label(frame_telemetry, text="Relay: --", font=("Arial", 14, "bold"))
            self.lbl_relay.pack(anchor="w", padx=5, pady=15)

            # --- Bottom Left: Command Controls ---
            frame_controls = ttk.LabelFrame(self.root, text="Controls")
            frame_controls.grid(row=1, column=0, padx=10, pady=10, sticky="nsew")

            ttk.Button(frame_controls, text="Relay ON", command=lambda: self._send_command("RELAY_ON")).pack(fill="x", padx=5, pady=2)
            ttk.Button(frame_controls, text="Relay OFF", command=lambda: self._send_command("RELAY_OFF")).pack(fill="x", padx=5, pady=2)
            ttk.Button(frame_controls, text="Reset Alert", command=lambda: self._send_command("RESET_ALERT")).pack(fill="x", padx=5, pady=15)

            # --- Right Side: Alerts and History Logs ---
            frame_logs = ttk.LabelFrame(self.root, text="System Events & Alerts")
            frame_logs.grid(row=0, column=1, rowspan=2, padx=10, pady=10, sticky="nsew")
            
            self.log_text = tk.Text(frame_logs, width=50, height=25, state="disabled", bg="#1e1e1e", fg="#d4d4d4")
            self.log_text.pack(padx=5, pady=5, fill="both", expand=True)

            # Configure grid weights to handle window resizing
            self.root.grid_columnconfigure(1, weight=1)
            self.root.grid_rowconfigure(0, weight=1)

    # ==========================================
    # Logic & Threading
    # ==========================================
    def _send_command(self, command_type):
            """Constructs and simulates sending an MQTT command."""
            
            # 1. Build the exact payload required by the contract
            payload = {
                "command_type": command_type,
                "timestamp": datetime.now().strftime("%Y-%m-%d %H:%M:%S.%f")[:-3],
                "source": "GUI"
            }
            
            payload_json = json.dumps(payload, indent=2)
            
            # 2. Log the simulated publish event to the GUI text box
            self._log_message(f"PUBLISHED to smartplug/cmd:\n{payload_json}", "command")
            
            # 3. Update the internal simulator state (for local GUI testing)
            # In the real application, you would remove this simulator logic and 
            # instead wait for the ESP32 to publish a new telemetry heartbeat.
            if command_type == "RELAY_ON" and not self.simulator.system_locked:
                self.simulator.relay_state = 1
            elif command_type == "RELAY_OFF":
                self.simulator.relay_state = 0
            elif command_type == "RESET_ALERT":
                self.simulator.system_locked = False
                
            # ==========================================
            # HARDWARE INTEGRATION NOTE:
            # When you connect the real MQTT client, you will add this line here:
            # mqtt_client.publish("smartplug/cmd", payload_json)
            # ==========================================

    def _dummy_data_loop(self):
        """Background thread generating fake MQTT messages."""
        while self.running:
            # Generate normal telemetry
            telemetry_json = self.simulator.generate_telemetry()
            self.data_queue.put(("smartplug/telemetry", telemetry_json))
            
            # 5% chance to simulate a random critical alert
            if random.random() < 0.05 and not self.simulator.system_locked:
                alert_json = self.simulator.generate_alert()
                self.data_queue.put(("smartplug/alerts", alert_json))
                
            time.sleep(1) # Simulate receiving data every 1 second

    def _process_queue(self):
        """Tkinter runs this on the main thread to check for new data without freezing."""
        try:
            while True:
                # Get data from queue (non-blocking)
                topic, payload_str = self.data_queue.get_nowait()
                data = json.loads(payload_str)
                
                # Route data based on Event Type or Topic
                if data.get("event_type") == "HEARTBEAT":
                    self._update_telemetry_ui(data)
                elif data.get("event_type") == "CRITICAL_PROTECTION":
                    self._handle_alert_ui(data)
                    
        except queue.Empty:
            pass # Queue is empty, nothing to do right now
        finally:
            # Reschedule this function to run again in 100ms
            self.root.after(100, self._process_queue)

    def _update_telemetry_ui(self, data):
            """Updates labels with live metrics."""
            metrics = data["metrics"]
            
            # Update existing labels
            self.lbl_v.config(text=f"Voltage: {metrics.get('v_rms', '--')} V")
            self.lbl_i.config(text=f"Current: {metrics.get('i_rms', '--')} A")
            
            # Update new labels (using .get() prevents crashes if a key is temporarily missing)
            self.lbl_pf.config(text=f"Power Factor: {metrics.get('pf', '--')}")
            self.lbl_thd.config(text=f"THD: {metrics.get('thd', '--')}")
            self.lbl_zcc.config(text=f"Zero Crossings: {metrics.get('zero_cross_count', '--')}")
            
            # Update relay state
            state = "ON (Green)" if data["relay_state"] == 1 else "OFF (Red)"
            color = "green" if data["relay_state"] == 1 else "red"
            self.lbl_relay.config(text=f"Relay: {state}", foreground=color)

    def _handle_alert_ui(self, data):
        """Handles high priority alerts."""
        msg = f"⚠️ ALERT: {data['cause']}! Action: {data['action_taken']}\n"
        self._log_message(msg, "alert")

    def _log_message(self, message, msg_type="info"):
        """Utility to write to the read-only text box."""
        self.log_text.config(state="normal")
        self.log_text.insert("end", f"[{datetime.now().strftime('%H:%M:%S')}] {message}\n")
        self.log_text.see("end") # Auto-scroll
        self.log_text.config(state="disabled")

    def on_closing(self):
        """Cleanly shut down the background thread when closing the window."""
        self.running = False
        self.root.destroy()

if __name__ == "__main__":
    root = tk.Tk()
    app = SmartPlugGUI(root)
    root.protocol("WM_DELETE_WINDOW", app.on_closing)
    root.mainloop()