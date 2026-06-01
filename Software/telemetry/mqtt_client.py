"""
SmartPlug MQTT Telemetry & Command Client
Receives telemetry data and sends dynamic limits to the ESP32 SmartPlug.
"""

import paho.mqtt.client as mqtt
import json
from datetime import datetime
import time
import sys

# MQTT Configuration
MQTT_BROKER = "192.168.137.1"  # Local PC
MQTT_PORT = 1883
MQTT_KEEPALIVE = 60

# Subscribe to these topic patterns
TOPICS = [
    "smartplug/#", 
    "aice/#"       # Added to catch aice/status ACKs and other aice topics
]

# Command topic to publish to
COMMAND_TOPIC = "aice/cmd"

# Flag to track if we're connected
is_connected = False


def on_connect(client, userdata, flags, rc):
    """Callback when client connects to the broker."""
    global is_connected
    timestamp = datetime.now().strftime("%Y-%m-%d %H:%M:%S")
    
    if rc == 0:
        is_connected = True
        print(f"\n[{timestamp}] Connected to MQTT broker at {MQTT_BROKER}:{MQTT_PORT}")
        print(f"[{timestamp}] Subscribing to topics...")
        for topic in TOPICS:
            result = client.subscribe(topic)
            if result[0] == mqtt.MQTT_ERR_SUCCESS:
                print(f"[{timestamp}] Γ£ô Subscribed to: {topic}")
            else:
                print(f"[{timestamp}] Γ£ù Failed to subscribe to {topic}: {result}")
        print("\n" + "="*70)
        print(" COMMAND MODE READY")
        print(" Type 'set <vrms> <iarms>' to update safety limits.")
        print(" Example: set 135.0 6.5")
        print(" Type 'quit' to exit.")
        print("="*70 + "\n")
    else:
        is_connected = False
        print(f"[{timestamp}] Connection failed with code {rc}")


def on_disconnect(client, userdata, rc):
    """Callback when client disconnects from the broker."""
    global is_connected
    is_connected = False
    timestamp = datetime.now().strftime("%Y-%m-%d %H:%M:%S")
    if rc == 0:
        print(f"[{timestamp}] Disconnected from MQTT broker (clean)")
    else:
        print(f"[{timestamp}] Unexpected disconnection (code: {rc})")


def on_message(client, userdata, msg):
    """Callback when a message is received."""
    timestamp = datetime.now().strftime("%Y-%m-%d %H:%M:%S")
    topic = msg.topic
    
    try:
        payload = msg.payload.decode('utf-8')
    except Exception as e:
        payload = f"[Binary data: {len(msg.payload)} bytes] Error: {e}"
    
    # Print a single, compact line for the received payload
    print(f"\n[{timestamp}] ≡ƒôÑ RECEIVED on {topic}:\n       {payload}")
    
    # Reprint the input prompt so the user knows they can still type
    print(" > ", end="", flush=True)


def main():
    print("=" * 70)
    print("SmartPlug MQTT Telemetry & Command Console")
    print("=" * 70)
    
    client = mqtt.Client(
        client_id="SmartPlug_PC_Listener",
        clean_session=True,
        userdata=None
    )
    
    client.on_connect = on_connect
    client.on_disconnect = on_disconnect
    client.on_message = on_message
    
    try:
        print(f" Connecting to {MQTT_BROKER}:{MQTT_PORT}...")
        client.connect(MQTT_BROKER, MQTT_PORT, MQTT_KEEPALIVE)
        
        # Start the listener in a BACKGROUND thread
        client.loop_start()
        
        # Wait a moment for connection to establish before showing prompt
        time.sleep(1)
        
        # Main thread handles user input
        while True:
            try:
                cmd = input(" > ")
                cmd = cmd.strip()
                
                if not cmd:
                    continue
                    
                if cmd.lower() == 'quit' or cmd.lower() == 'exit':
                    break
                    
                if cmd.lower().startswith("set "):
                    parts = cmd.split()
                    if len(parts) == 3:
                        try:
                            v_limit = float(parts[1])
                            i_limit = float(parts[2])
                            
                            # Construct JSON payload
                            payload_dict = {
                                "max_vrms": v_limit,
                                "max_iarms": i_limit
                            }
                            payload_str = json.dumps(payload_dict)
                            
                            # Publish to the ESP32
                            client.publish(COMMAND_TOPIC, payload_str, qos=1)
                            print(f"\n ≡ƒôñ PUBLISHED to {COMMAND_TOPIC}:\n       {payload_str}")
                        except ValueError:
                            print(" Error: Limits must be valid numbers (e.g., 'set 135.0 6.5')")
                    else:
                        print(" Error: Incorrect format. Use 'set <vrms> <iarms>'")
                else:
                    print(" Unknown command. Type 'set <vrms> <iarms>' or 'quit'")
                    
            except EOFError:
                break
                
    except ConnectionRefusedError as e:
        print(f"\n Connection refused. Make sure Mosquitto is running on {MQTT_BROKER}.")
    except KeyboardInterrupt:
        print("\n\n Stopped by user")
    finally:
        if is_connected:
            client.disconnect()
        # Stop the background thread
        client.loop_stop()
        print(" Exited safely.")


if __name__ == "__main__":
    main()
