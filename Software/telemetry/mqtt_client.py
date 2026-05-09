"""
SmartPlug MQTT Telemetry Client
Receives and logs telemetry data from the ESP32 SmartPlug device.
"""

import paho.mqtt.client as mqtt
import json
from datetime import datetime

# MQTT Configuration
MQTT_BROKER = "127.0.0.1"  # Local PC (MQTT broker must run on this machine)
MQTT_PORT = 1883
MQTT_KEEPALIVE = 60

# Subscribe to these topic patterns
TOPICS = [
    "smartplug/status",
    "smartplug/relay",
    "smartplug/led",
    "smartplug/energy",
    "smartplug/+",  # Wildcard to catch all smartplug topics
]


def on_connect(client, userdata, flags, rc):
    """Callback when client connects to the broker."""
    if rc == 0:
        print("✅ Connected to MQTT broker")
        # Subscribe to all topics
        for topic in TOPICS:
            client.subscribe(topic)
            print(f"   Subscribed to: {topic}")
    else:
        print(f"❌ Connection failed with code {rc}")


def on_disconnect(client, userdata, rc):
    """Callback when client disconnects from the broker."""
    if rc != 0:
        print(f"⚠️  Unexpected disconnection (code: {rc})")
    else:
        print("Disconnected from MQTT broker")


def on_message(client, userdata, msg):
    """Callback when a message is received."""
    timestamp = datetime.now().strftime("%Y-%m-%d %H:%M:%S")
    topic = msg.topic
    payload = msg.payload.decode('utf-8', errors='ignore')
    
    print(f"\n[{timestamp}] 📨 Message from {topic}:")
    print(f"   Payload: {payload}")
    
    # Try to parse as JSON for pretty printing
    try:
        data = json.loads(payload)
        print(f"   Parsed JSON:")
        for key, value in data.items():
            print(f"      {key}: {value}")
    except (json.JSONDecodeError, ValueError):
        # Not JSON, just a string value
        pass


def on_subscribe(client, userdata, mid, granted_qos):
    """Callback when subscription is acknowledged."""
    print(f"   Subscription acknowledged (QoS: {granted_qos})")


def main():
    print("=" * 60)
    print("SmartPlug MQTT Telemetry Listener")
    print("=" * 60)
    print(f"Connecting to MQTT broker at {MQTT_BROKER}:{MQTT_PORT}...\n")
    
    # Create MQTT client
    client = mqtt.Client(client_id="SmartPlug_PC_Listener", clean_session=True)
    
    # Set callbacks
    client.on_connect = on_connect
    client.on_disconnect = on_disconnect
    client.on_message = on_message
    client.on_subscribe = on_subscribe
    
    try:
        # Connect to broker
        client.connect(MQTT_BROKER, MQTT_PORT, MQTT_KEEPALIVE)
        
        # Start the loop
        client.loop_forever()
        
    except ConnectionRefusedError:
        print(f"❌ Could not connect to {MQTT_BROKER}:{MQTT_PORT}")
        print("   Make sure the MQTT broker (Mosquitto) is running!")
        print("   Windows: Run 'mosquitto' from Command Prompt or Services")
        print("   Or start it with: mosquitto -c mosquitto.conf")
        
    except KeyboardInterrupt:
        print("\n\n⏹️  Listener stopped by user")
        client.disconnect()
        client.loop_stop()


if __name__ == "__main__":
    main()
