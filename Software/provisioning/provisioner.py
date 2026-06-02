"""
BLE provisioning helper for the AYCE Smart Plug.

This file can be used in two ways:
    1. Run directly from the command line.
    2. Imported by the GUI, which calls send_credentials_sync(...).

The default credentials below are intentionally left as used during the working
integration tests.
"""

from __future__ import annotations

import argparse
import asyncio
import json
from typing import Any, Dict, Optional

try:
    from bleak import BleakClient, BleakScanner
except ModuleNotFoundError:  # GUI can still open and report a clear BLE dependency error.
    BleakClient = None
    BleakScanner = None

# --- TECHNICAL CONFIGURATION (Must match firmware values) ---
SERVICE_UUID = "f0debc9a-7856-3412-7856-341278563412"
CHAR_UUID_JSON = "f1debc9a-7856-3412-7856-341278563412"
DEVICE_NAME = "SmartPlug"
TARGET_MAC = "E0:72:A1:CE:A3:8A"  # Got by nRF Connect

# Working development credentials. Keep in sync with the GUI defaults.
DEFAULT_WIFI_SSID = "AICE_HS"
DEFAULT_WIFI_PASSWORD = "Bake_This"
DEFAULT_MQTT_BROKER = "192.168.137.1"
DEFAULT_MQTT_PORT = 1883


class ProvisioningError(RuntimeError):
    """Raised when BLE provisioning cannot be completed."""


async def find_target_device(target_mac: str = TARGET_MAC, timeout_s: float = 15.0):
    """Find the ESP32 provisioning peripheral by MAC address."""
    if BleakScanner is None:
        raise ProvisioningError("Python package 'bleak' is not installed. Run: pip install -r requirements.txt")
    return await asyncio.wait_for(
        BleakScanner.find_device_by_filter(lambda d, ad: d.address.upper() == target_mac.upper()),
        timeout=timeout_s,
    )


async def send_credentials_async(
    ssid: str = DEFAULT_WIFI_SSID,
    password: str = DEFAULT_WIFI_PASSWORD,
    target_mac: str = TARGET_MAC,
    timeout_s: float = 15.0,
    broker_ip: str = DEFAULT_MQTT_BROKER,
    broker_port: int = DEFAULT_MQTT_PORT,
    include_mqtt_fields: bool = False,
) -> Dict[str, Any]:
    """Send provisioning JSON through BLE.

    The firmware version used in the current tests expects at least:
        {"ssid": "...", "password": "..."}

    `include_mqtt_fields` is left disabled by default to preserve the working
    firmware contract. Enable it only if the firmware provisioning parser is
    updated to read broker_ip / broker_port from the same JSON payload.
    """
    if not ssid:
        raise ProvisioningError("WiFi SSID cannot be empty")

    try:
        device = await find_target_device(target_mac=target_mac, timeout_s=timeout_s)
    except asyncio.TimeoutError as exc:
        raise ProvisioningError(
            f"Scan timeout: device with MAC {target_mac} not found within {timeout_s:.0f} seconds"
        ) from exc

    if not device:
        raise ProvisioningError(f"Device with MAC {target_mac} not found in BLE scan results")

    payload_dict: Dict[str, Any] = {
        "ssid": ssid,
        "password": password,
    }
    if include_mqtt_fields:
        payload_dict.update({"broker_ip": broker_ip, "broker_port": int(broker_port)})

    payload = json.dumps(payload_dict, separators=(",", ":")).encode("utf-8")

    try:
        if BleakClient is None:
            raise ProvisioningError("Python package 'bleak' is not installed. Run: pip install -r requirements.txt")
        async with BleakClient(device) as client:
            services = client.services
            available_service_uuids = {service.uuid.lower() for service in services}
            if SERVICE_UUID.lower() not in available_service_uuids:
                raise ProvisioningError(
                    f"Service UUID {SERVICE_UUID} not found. Available services: {available_service_uuids}"
                )
            await client.write_gatt_char(CHAR_UUID_JSON, payload)
    except ProvisioningError:
        raise
    except Exception as exc:  # bleak raises platform-specific exceptions.
        raise ProvisioningError(f"BLE connection/write error: {exc}") from exc

    return {
        "device_address": device.address,
        "ssid": ssid,
        "broker_ip": broker_ip,
        "broker_port": int(broker_port),
        "payload_bytes": len(payload),
        "include_mqtt_fields": include_mqtt_fields,
    }


def send_credentials_sync(
    ssid: str = DEFAULT_WIFI_SSID,
    password: str = DEFAULT_WIFI_PASSWORD,
    target_mac: str = TARGET_MAC,
    timeout_s: float = 15.0,
    broker_ip: str = DEFAULT_MQTT_BROKER,
    broker_port: int = DEFAULT_MQTT_PORT,
    include_mqtt_fields: bool = False,
) -> Dict[str, Any]:
    """Synchronous wrapper used by Tkinter worker threads."""
    return asyncio.run(
        send_credentials_async(
            ssid=ssid,
            password=password,
            target_mac=target_mac,
            timeout_s=timeout_s,
            broker_ip=broker_ip,
            broker_port=broker_port,
            include_mqtt_fields=include_mqtt_fields,
        )
    )


async def main_async(args: argparse.Namespace) -> int:
    print(f"--- Searching for device with MAC {args.mac}... ---")
    try:
        result = await send_credentials_async(
            ssid=args.ssid,
            password=args.password,
            target_mac=args.mac,
            timeout_s=args.timeout,
            broker_ip=args.broker,
            broker_port=args.port,
            include_mqtt_fields=args.include_mqtt_fields,
        )
    except ProvisioningError as exc:
        print(f"Provisioning failed: {exc}")
        print("Is the ESP32 powered on and advertising?")
        return 2

    print(f"Found: {result['device_address']}")
    print(f"Credentials sent successfully: ssid={result['ssid']} broker={result['broker_ip']}:{result['broker_port']}")
    print("ESP32 should now restart WiFi and connect to the MQTT broker.")
    return 0


def build_arg_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Provision AYCE Smart Plug WiFi credentials over BLE")
    parser.add_argument("--ssid", default=DEFAULT_WIFI_SSID, help="WiFi SSID sent to the ESP32")
    parser.add_argument("--password", default=DEFAULT_WIFI_PASSWORD, help="WiFi password sent to the ESP32")
    parser.add_argument("--broker", default=DEFAULT_MQTT_BROKER, help="MQTT broker IP stored/used by the PC tools")
    parser.add_argument("--port", type=int, default=DEFAULT_MQTT_PORT, help="MQTT broker port")
    parser.add_argument("--mac", default=TARGET_MAC, help="ESP32 BLE MAC address")
    parser.add_argument("--timeout", type=float, default=15.0, help="BLE scan timeout in seconds")
    parser.add_argument(
        "--include-mqtt-fields",
        action="store_true",
        help="Also include broker_ip/broker_port in the BLE JSON payload; leave off for current firmware",
    )
    return parser


def main() -> int:
    parser = build_arg_parser()
    args = parser.parse_args()
    return asyncio.run(main_async(args))


if __name__ == "__main__":
    raise SystemExit(main())
