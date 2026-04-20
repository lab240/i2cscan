# i2cscan

Lightweight I2C bus scanner for embedded Linux. Detects all devices on the bus and identifies known chips by reading their ID registers.

Works on any Linux system with I2C support — Armbian, OpenWrt, Debian, Buildroot, Yocto — from a single statically linked binary. No runtime dependencies.

## Features

- Scans one or all I2C buses
- Detects devices using the same SMBus probing strategy as `i2cdetect`
- Identifies known chips by reading hardware ID registers (BME280, BMP280, MPU6050, LIS3DH, etc.)
- Falls back to address-based identification for devices without readable chip IDs (DS3231, SSD1306, EEPROM, etc.)
- Per-bus timeout protection — a hung bus won't block the entire scan
- Single static binary, no dependencies on `i2c-tools`

## Building

### Native (on the target board)

```
gcc -static -O2 -o i2cscan src/i2cscan.c
```

### Cross-compile for aarch64

```
aarch64-linux-gnu-gcc -static -O2 -o i2cscan src/i2cscan.c
```

Requires kernel headers with `linux/i2c-dev.h` and `linux/i2c.h` at build time. At runtime, only `/dev/i2c-*` device nodes are needed.

## Prerequisites

The I2C device nodes must exist on the target system.

**Armbian / Debian:**

```
sudo apt install i2c-tools
sudo modprobe i2c-dev
```

**OpenWrt:**

```
apk add kmod-i2c-core kmod-i2c-dev
```

Verify with `ls /dev/i2c-*`.

## Usage

```
i2cscan --bus <n>       # scan a specific bus
i2cscan --bus-all       # scan all available buses
i2cscan --help          # show help
```

## Example output

```
I2C scan
========

/dev/i2c-1:
  0x50  EEPROM (24Cxx)
  0x68  DS3231 (RTC) / MPU6050 / PCF8523
  0x76  BME280

/dev/i2c-3:
  (no devices found)
```

## How it works

Detection uses SMBus ioctls, matching the behavior of `i2cdetect -y`:

| Address range | Probe method | Why |
|---|---|---|
| 0x30–0x37 | SMBus quick write | Safe for devices that interpret reads as commands |
| 0x50–0x5F | SMBus quick write | Safe for EEPROMs (avoids accidental reads) |
| All others | SMBus read byte | General-purpose detection |

After a device is found, `i2cscan` tries to read known chip ID registers (e.g. `0xD0` for Bosch sensors, `0x75` for InvenSense IMUs, `0x0F` for ST sensors). If no register match is found, it falls back to a table of well-known fixed addresses.

## Supported chips

### Identified by register

| Chip | Register | ID | Typical address |
|---|---|---|---|
| BME280 | 0xD0 | 0x60 | 0x76 / 0x77 |
| BMP280 | 0xD0 | 0x58 | 0x76 / 0x77 |
| BMP180 | 0xD0 | 0x55 | 0x77 |
| BME680 | 0xD0 | 0x61 | 0x76 / 0x77 |
| MPU6050 | 0x75 | 0x68 | 0x68 / 0x69 |
| MPU6500 | 0x75 | 0x70 | 0x68 / 0x69 |
| MPU9250 | 0x75 | 0x71 | 0x68 / 0x69 |
| ICM-20689 | 0x75 | 0x98 | 0x68 / 0x69 |
| LIS3DH | 0x0F | 0x33 | 0x18 / 0x19 |
| LSM6DS3 | 0x0F | 0x6A | 0x6A / 0x6B |
| LSM6DSO | 0x0F | 0x69 | 0x6A / 0x6B |
| LIS3MDL | 0x0F | 0xBB | 0x1C / 0x1E |
| HMC5883L | 0x0A | 0x48 | 0x1E |

### Identified by address

| Address | Likely device |
|---|---|
| 0x23 | BH1750 (light sensor, ADDR=LOW) |
| 0x27 | PCF8574 (LCD/GPIO expander) |
| 0x38 | AHT20 (temperature/humidity) |
| 0x3C–0x3D | SSD1306 (OLED display) |
| 0x44–0x45 | SHT3x (temperature/humidity) |
| 0x48–0x4B | ADS1115 / TMP102 / PCF8591 |
| 0x50 | 24Cxx EEPROM |
| 0x51 | PCF8563 (RTC) / EEPROM |
| 0x5C | BH1750 (light sensor, ADDR=HIGH) |
| 0x68 | DS1307 / DS3231 (RTC) / MPU6050 / PCF8523 |
| 0x76–0x77 | BME280 / BMP280 / MS5611 |

## Adding new chips

Edit the tables in `i2cscan.c`:

- `known_chips[]` — for devices with a readable ID register (preferred)
- `known_addrs[]` — for fixed-address devices without ID registers

## License

MIT