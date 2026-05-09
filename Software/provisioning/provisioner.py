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
TARGET_MAC = "D8:3B:DA:8A:2B:A6" # Got by Nrf connect 

async def main():
    print(f"--- Searching for device with MAC {TARGET_MAC}... ---")
    
    # 1. Scan for BLE devices (with 15-second timeout)
    try:
        device = await asyncio.wait_for(
            BleakScanner.find_device_by_filter(
                lambda d, ad: d.address == TARGET_MAC
            ),
            timeout=15.0
        )
    except asyncio.TimeoutError:
        print(f"Scan timeout: Device with MAC {TARGET_MAC} not found within 15 seconds.")
        print("   Is the ESP32 powered on and advertising?")
        return

    if not device:
        print(f"Device with MAC {TARGET_MAC} not found in scan results")
        return

    print(f"Found: {device.address}. Connecting...")

    # 2. Connect and send provisioning data
    try:
        async with BleakClient(device) as client:
            print("Connected!")

            # Verify that the expected provisioning service is present
            services = client.services
            available_service_uuids = {service.uuid.lower() for service in services}
            if SERVICE_UUID.lower() not in available_service_uuids:
                print(f"   Service UUID {SERVICE_UUID} not found on device.")
                print(f"   Available services: {available_service_uuids}")
                return

            # Hotspot credentials (replace with real values)
            wifi_credentials = {
                "ssid": "AICE_HS", # Replace with hotspot SSID
                "password": "Bake_This" # Replace with hotspot password 
            }
            
            # Convert dictionary to JSON string and then to bytes
            payload = json.dumps(wifi_credentials).encode('utf-8')

            print(f" Sending credentials: {wifi_credentials}")
            await client.write_gatt_char(CHAR_UUID_JSON, payload)
            
            print(" Credentials sent successfully. ESP32 should now restart Wi-Fi.")
            
    except asyncio.TimeoutError:
        print(f"Connection timeout. Device may have disconnected.")
    except Exception as e:
        print(f"Connection error: {e}")

if __name__ == "__main__":
    asyncio.run(main())