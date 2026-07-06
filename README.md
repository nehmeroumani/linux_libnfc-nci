linux_libnfc-nci
================

branch NCI2.0_PN7160: Linux NFC stack for PN7160 NCI2.0 based NXP NFC Controller.
For previous NXP NFC Controllers support (PN7150, PN7120) refer to branch master.

This fork includes patches for **64-bit Linux** (no extra patch step required), **libgpiod** GPIO support for Raspberry Pi OS Bookworm, and related portability fixes. Upstream NCI2.0-R1.1 still documents a separate 64-bit patch; that is already applied here.

Information about NXP NFC Controller can be found on [NXP website](https://www.nxp.com/products/identification-and-security/nfc/nfc-reader-ics:NFC-READER).

Further details about the stack [here](https://www.nxp.com/doc/AN13287).

Install
-------

Tested on **Raspberry Pi OS Bookworm** with an **Elechouse PN7160** (I2C, default pins below). On Bookworm the kernel no longer exposes GPIO through sysfs; this tree uses **libgpiod** instead when built with `--enable-libgpiod`.

### 1. Enable I2C

```bash
sudo raspi-config nonint do_i2c 0
sudo reboot
```

After reboot, confirm the bus and chip:

```bash
ls /dev/i2c-1 /dev/gpiochip0
sudo apt install -y i2c-tools
i2cdetect -y 1    # PN7160 should appear at address 0x28
```

### 2. Install build dependencies

```bash
sudo apt update
sudo apt install -y git build-essential autoconf automake libtool pkg-config libgpiod-dev
```

### 3. Build and install

```bash
git clone -b NCI2.0_PN7160 https://github.com/nehmeroumani/linux_libnfc-nci.git
cd linux_libnfc-nci
./bootstrap
./configure --enable-libgpiod
make
sudo make install
sudo ldconfig
```

If `nfcDemoApp` cannot find the library at runtime, set the library path for the current shell:

```bash
export LD_LIBRARY_PATH=/usr/local/lib
```

To make this last setting permanent, run the following command:

```bash
echo "export LD_LIBRARY_PATH=/usr/local/lib" >> ~/.bashrc
```

`--enable-libgpiod` is required on Bookworm. Without it the stack falls back to the deprecated sysfs GPIO interface, which does not work reliably on current Raspberry Pi kernels.

### 4. Install configuration files

`sudo make install` installs configs to `/usr/local/etc/`. Configs in `/etc/nfc/` take precedence when present. To use `/etc/nfc/`:

```bash
sudo mkdir -p /etc/nfc
sudo cp conf/libnfc-nci.conf conf/libnfc-nxp.conf /etc/nfc/
```

For debugging, set `NXPLOG_NCIHAL_LOGLEVEL`, `NXPLOG_FWDNLD_LOGLEVEL`, and `NXPLOG_TML_LOGLEVEL` to `0x03` in `libnfc-nxp.conf`.

### 5. Wiring (Elechouse PN7160 default)

| PN7160 | Pi (BCM) | Physical pin |
|--------|----------|--------------|
| SDA    | GPIO2    | 3            |
| SCL    | GPIO3    | 5            |
| IRQ    | GPIO23   | 16           |
| VEN    | GPIO24   | 18           |
| DWL_REQ| GPIO25   | 22           |
| VCC    | 3V3      | 1 or 17      |
| GND    | GND      | 6, 9, …      |

GPIO control defaults to `gpiochip0` with pins 23/24/25 (see `NfccAltTransport.h`).
The chip and pins can be overridden in `/etc/nfc/libnfc-nxp.conf` without
recompiling:

```
NXP_GPIO_CHIP="gpiochip0"
NXP_PIN_INT=23
NXP_PIN_VEN=24
NXP_PIN_FWDNLD=25
```

> **Raspberry Pi 5:** depending on the kernel, the 40-pin header GPIOs may be
> exposed on `gpiochip4` instead of `gpiochip0`. Check with `gpioinfo` and set
> `NXP_GPIO_CHIP` accordingly.

When using the ALT_SPI transport (`NXP_TRANSPORT=0x03`), set
`NXP_NFC_DEV_NODE` to the SPI device (e.g. `"/dev/spidev0.0"`).

### 6. Run the demo

```bash
nfcDemoApp poll
```

Release version
---------------
- NCI2.0-R1.1 fix limitation about multiple T5T, fix issue with MIFARE Classic read after write, cleanup of alternate transport definition, cleanup of logs 
- NCI2.0-R1.0 is the first official release of Linux libnfc-nci stack for PN7160

Possible problems, known errors and restrictions of R1.1:
---------------------------------------------------------
- LLCP1.3 support requires OpenSSL Cryptography and SSL/TLS Toolkit (version 1.0.1j or later)
- **64-bit OS:** already patched in this repository; build normally on aarch64/arm64 and amd64
