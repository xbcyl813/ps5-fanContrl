# ps5-fanContrl

**English** | [简体中文](README_zh.md)

A configurable PS5 fan speed controller that manages boost thresholds (target temperatures) and polling intervals via INI configuration files.

---

## 🌟 Features

* **Safe Autoservo Control**  
  Preserves Sony's factory fan calibration curves and hardware fail-safe mechanisms by safely modifying only the target temperature byte (`offset 5`) of `/dev/icc_fan` via a strict **Read-Modify-Write-Verify** (`GET -> Modify -> SET -> Readback`) workflow.

* **Anti-Reset Target Guard**  
  Automatically detects when the PS5 system (`SceShellCore`) switches servo patterns (e.g., launching a game, suspending to the home screen, or waking from rest mode) and seamlessly reapplies your custom target temperature without spamming notifications.

* **Hot-Reloadable INI Config**  
  Supports live updates to target temperature, polling interval, and OSD notification toggle via USB drive or internal storage without restarting the payload.

* **Hardware Fail-Safe & Dynamic Sensors**  
  Dynamically enumerates all available SoC temperature sensors across different PS5 board revisions and automatically exits to restore stock fan control if consecutive hardware I/O errors occur.

---

## ⚙️ Configuration

Create a file named `fan_control.ini` and place it in one of the following locations (checked in order of priority):

1. **Internal Storage (Highest Priority):** `/data/fan_control.ini`
2. **USB Drive (Fallback):** `/mnt/usb0/fan_control.ini` ~ `/mnt/usb7/fan_control.ini`

### Example `fan_control.ini`

```ini
# Target temperature threshold in Celsius (Range: 30 - 90, Default: 80)
target_temperature=80

# Polling & guard refresh interval in seconds (Range: 1 - 3600, Default: 15)
interval=15

# Show PS5 system notification when target temperature is applied/re-applied (1 = Enable, 0 = Disable)
show_status=1
```

> **Note:** If no configuration file is found, the daemon runs with default settings (`target_temperature=70`, `interval=5`, `show_status=0`). If a configuration file exists but contains invalid or out-of-range values, the daemon will exit safely to prevent unintended hardware behavior.

---

## 🛠️ Building from Source

This project is built using the [ps5-payload-sdk](https://github.com).

```bash
# Set your SDK path if not installed in the default location
export PS5_PAYLOAD_SDK=/opt/ps5-payload-sdk

# Build the ELF payload
make clean && make
```

The compiled payload `ps5-fancontrl.elf` can be sent to your PS5 via your preferred payload loader (e.g., `elfldr`, `etaHEN`, or `nc`).

---

## ⚠️ Disclaimer

This software interacts directly with low-level PS5 hardware thermal control interfaces (`/dev/icc_fan`). While strict safety checks and readback verifications are enforced, **use this tool at your own risk**. The author is not responsible for any hardware damage or overheating caused by improper configuration.

---

## 📄 License

This project is licensed under the **GPL-3.0 License** - see the [LICENSE](LICENSE) file for details.
