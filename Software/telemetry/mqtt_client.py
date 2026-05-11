"""
SmartPlug MQTT Telemetry Client
Receives and logs telemetry data from the ESP32 SmartPlug device.
"""

import paho.mqtt.client as mqtt
import json
from datetime import datetime
import time

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
    "smartplug/#",  # Wildcard to catch ALL smartplug topics
]

# Flag to track if we're connected
is_connected = False


def on_connect(client, userdata, flags, rc):
    """Callback when client connects to the broker."""
    global is_connected
    timestamp = datetime.now().strftime("%Y-%m-%d %H:%M:%S")
    
    if rc == 0:
        is_connected = True
        print(f"[{timestamp}] ✅ Connected to MQTT broker at {MQTT_BROKER}:{MQTT_PORT}")
        print(f"[{timestamp}] 📡 Subscribing to topics...\n")
        # Subscribe to all topics
        for topic in TOPICS:
            result = client.subscribe(topic)
            if result[0] == mqtt.MQTT_ERR_SUCCESS:
                print(f"[{timestamp}] ✓ Subscribed to: {topic}")
            else:
                print(f"[{timestamp}] ✗ Failed to subscribe to {topic}: {result}")
        print()
    else:
        is_connected = False
        error_codes = {
            1: "Incorrect protocol version",
            2: "Invalid client identifier",
            3: "Server unavailable",
            4: "Bad username or password",
            5: "Not authorized"
        }
        error_msg = error_codes.get(rc, f"Unknown error code {rc}")
        print(f"[{timestamp}] ❌ Connection failed: {error_msg}")


def on_disconnect(client, userdata, rc):
    """Callback when client disconnects from the broker."""
    global is_connected
    is_connected = False
    timestamp = datetime.now().strftime("%Y-%m-%d %H:%M:%S")
    if rc == 0:
        print(f"[{timestamp}] ⏹️  Disconnected from MQTT broker (clean)")
    else:
        print(f"[{timestamp}] ⚠️  Unexpected disconnection (code: {rc})")


def on_message(client, userdata, msg):
    """Callback when a message is received."""
    timestamp = datetime.now().strftime("%Y-%m-%d %H:%M:%S")
    topic = msg.topic
    qos = msg.qos
    retain = msg.retain
    
    # Decode payload
    try:
        payload = msg.payload.decode('utf-8')
    except Exception as e:
        payload = f"[Binary data: {len(msg.payload)} bytes] Error: {e}"
    
    print(f"[{timestamp}] 📨 Message received")
    print(f"    Topic: {topic}")
    print(f"    QoS: {qos}, Retained: {retain}")
    print(f"    Payload: {payload}")
    
    # Try to parse as JSON for pretty printing
    try:
        data = json.loads(payload)
        print(f"    Parsed JSON:")
        for key, value in data.items():
            print(f"       • {key}: {value}")
    except (json.JSONDecodeError, ValueError, TypeError):
        # Not JSON, just a string value
        pass
    print()


def on_subscribe(client, userdata, mid, granted_qos, properties=None):
    """Callback when subscription is acknowledged."""
    timestamp = datetime.now().strftime("%Y-%m-%d %H:%M:%S")
    print(f"[{timestamp}] ✓ Subscription acknowledged (QoS levels: {granted_qos})")


def on_log(client, userdata, level, buf):
    """Callback for MQTT library logging."""
    if level == mqtt.MQTT_LOG_DEBUG:
        print(f"[MQTT-DEBUG] {buf}")
    elif level == mqtt.MQTT_LOG_INFO:
        print(f"[MQTT-INFO] {buf}")
    elif level == mqtt.MQTT_LOG_NOTICE:
        print(f"[MQTT-NOTICE] {buf}")
    elif level == mqtt.MQTT_LOG_WARNING:
        print(f"[MQTT-WARNING] {buf}")
    elif level == mqtt.MQTT_LOG_ERR:
        print(f"[MQTT-ERROR] {buf}")


def main():
    print("=" * 70)
    print("SmartPlug MQTT Telemetry Listener")
    print("=" * 70)
    print(f"Broker: {MQTT_BROKER}:{MQTT_PORT}")
    print(f"Client ID: SmartPlug_PC_Listener")
    print(f"Keep-alive: {MQTT_KEEPALIVE}s")
    print(f"Topics subscribed to: {len(TOPICS)}")
    for i, topic in enumerate(TOPICS, 1):
        print(f"  {i}. {topic}")
    print("=" * 70 + "\n")
    
    # Create MQTT client with clean session
    client = mqtt.Client(
        client_id="SmartPlug_PC_Listener",
        clean_session=True,
        userdata=None
    )
    
    # Set all callbacks
    client.on_connect = on_connect
    client.on_disconnect = on_disconnect
    client.on_message = on_message
    client.on_subscribe = on_subscribe
    client.on_log = on_log
    
    # Optional: Enable debug mode
    # client.enable_logger(logging.getLogger())
    
    try:
        print(f"🔌 Connecting to {MQTT_BROKER}:{MQTT_PORT}...")
        client.connect(MQTT_BROKER, MQTT_PORT, MQTT_KEEPALIVE)
        
        # Start the blocking loop
        print("📡 Starting listener loop. Press Ctrl+C to stop.\n")
        client.loop_forever()
        
    except ConnectionRefusedError as e:
        print(f"\n❌ Connection refused. Could not connect to {MQTT_BROKER}:{MQTT_PORT}")
        print("   Make sure the MQTT broker (Mosquitto) is running:")
        print("   • Check Windows Services for 'Mosquitto Broker'")
        print("   • Or start manually: mosquitto -c mosquitto.conf")
        print(f"   • Verify port 1883 is listening: netstat -an | findstr 1883")
        print(f"   Error details: {e}")
        
    except OSError as e:
        print(f"\n❌ Connection error: {e}")
        print(f"   Check that:")
        print(f"   • Mosquitto broker is running on {MQTT_BROKER}:{MQTT_PORT}")
        print(f"   • Firewall is not blocking port 1883")
        print(f"   • Configuration file has: listener 1883 and allow_anonymous true")
        
    except KeyboardInterrupt:
        print("\n\n⏹️  Listener stopped by user")
        if is_connected:
            client.disconnect()
        client.loop_stop()


if __name__ == "__main__":
    main()
