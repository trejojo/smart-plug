import asyncio
import json
from bleak import BleakScanner, BleakClient

# --- TECHNICAL CONFIGURATION (Must match firmware values) ---
# Service UUID expected by the ESP32 firmware
SERVICE_UUID = "f0debc9a-7856-3412-7856-341278563412"
# Characteristic UUID where the ESP32 expects the JSON payload
# Derived from CREDS_JSON_CHAR_UUID bytes (NimBLE uses little-endian byte order)
CHAR_UUID_JSON = "f1debc9a-7856-3412-7856-341278563412"
DEVICE_NAME = "SmartPlug"

async def main():
    print(f"--- Searching for {DEVICE_NAME}... ---")
    
    # 1. Scan for BLE devices
    device = await BleakScanner.find_device_by_filter(
        lambda d, ad: d.name == DEVICE_NAME
    )

    if not device:
        print("❌ SmartPlug not found. Is it powered on and in BLE mode?")
        return

    print(f"✅ Found: {device.address}. Connecting...")

    # 2. Connect and send provisioning data
    try:
        async with BleakClient(device) as client:
            print("🔗 Connected!")

            # Verify that the expected provisioning service is present
            services = await client.get_services()
            available_service_uuids = {service.uuid.lower() for service in services}
            if SERVICE_UUID.lower() not in available_service_uuids:
                print("💥 Expected provisioning service UUID not found on this device.")
                return

            # Hotspot credentials (replace with real values)
            wifi_credentials = {
                "ssid": "Your_Hotspot_Name",
                "password": "Your_Password_123"
            }
            
            # Convert dictionary to JSON string and then to bytes
            payload = json.dumps(wifi_credentials).encode('utf-8')

            print(f"📤 Sending credentials: {wifi_credentials}")
            await client.write_gatt_char(CHAR_UUID_JSON, payload)
            
            print("🚀 Credentials sent successfully. ESP32 should now restart Wi-Fi.")
            
    except Exception as e:
        print(f"💥 Connection error: {e}")

if __name__ == "__main__":
    asyncio.run(main())