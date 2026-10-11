# AI-VOX-Buddy — Hermes Voice Satellite for AI-VOX3

Native ESP-IDF firmware project for the NULLLAB AI-VOX3 (ESP32-S3R8,
16 MB flash, 8 MB octal PSRAM). It contains a hardware self-test and an
opt-in Hermes/MQTT audio-satellite mode. The satellite has been built but
**not flashed or tested on-device**.

## Hardware reference

Authoritative source: [manufacturer repository](https://github.com/nulllaborg/esp32-s3-audio-dev-board)
and its V1.1 schematic.

| Function | GPIO |
|---|---|
| ES8311 I2S DOUT / WS / DIN / BCLK / MCLK | 7 / 8 / 9 / 10 / 11 |
| ES8311 I2C SCL / SDA | 12 / 13 |
| ST7789 DC / CS / BL / SCLK / MOSI | 14 / 15 / 16 / 17 / 21 |
| microSD CMD / CLK / D0 | 38 / 39 / 40 |
| WS2812B | 41 |
| BOOT / B / A buttons | 0 / 45 / 46 |
| Battery ADC / charge detect | 18 / 47 |

The manufacturer's SD/audio demo incorrectly defines `PA_ENABLE` as GPIO53.
ESP32-S3 has only GPIO0..48, and the schematic shows no software PA-enable.
This firmware uses `GPIO_NUM_NC` plus a compile-time guard.

## Toolchain and build

Tested with ESP-IDF v5.5.5. The local installation is reproducible with:

```sh
git clone --branch v5.5.5 --depth 1 --recursive \
  https://github.com/espressif/esp-idf.git .tools/esp-idf-v5.5.5
IDF_TOOLS_PATH="$PWD/.tools/espressif" .tools/esp-idf-v5.5.5/install.sh esp32s3
./scripts/build.sh
```

The optional LCD defaults to disabled. Configure options with
`source scripts/env.sh && idf.py menuconfig`; besides the LCD, the
"AI-VOX3 hardware self-test" menu sets speaker volume (default 55) and
microphone gain (default 24 dB). `scripts/env.sh` can be sourced from an
interactive shell; it does not change shell options.

`partitions.csv` keeps ESP-IDF's default NVS/PHY offsets but enlarges the
factory app to 4 MB: a fully configured Hermes image is about 1,010 KB and
left only 1 % of the former 1-MB app partition free. Flashing writes the new
partition table automatically; NVS data is preserved. Generated `sdkconfig*`
files created before this change still select the old table, so either
delete them (they are regenerated from the defaults) or select
"Custom partition table CSV" in menuconfig.

The WAV parsing and resampling code has no ESP-IDF dependency and is tested on
the host with `./scripts/test-host.sh` (gcc/clang with ASan/UBSan).
Its separate build profile can be checked without changing the normal build:
`source scripts/env.sh && idf.py -B build-lcd -DSDKCONFIG=sdkconfig.lcd -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.lcd.defaults" build`.

## Hermes satellite mode

The optional mode publishes 100-ms, PCM16/16-kHz/mono WAV chunks from the
ES8311 microphone to `hermes/audioServer/<siteId>/audioFrame`. It subscribes
to `hermes/audioServer/<siteId>/playBytes/+`, plays PCM16 WAV (mono or stereo,
8..48 kHz, e.g. Piper's 22.05 kHz output) through ES8311/I2S after converting it
to 16-kHz mono with linear interpolation, and publishes `hermes/audioServer/<siteId>/playFinished`
with the request ID after playback. Microphone publishing pauses during
speaker playback. Wake-word recognition, ASR, NLU, and dialogue management
are expected to run elsewhere on the MQTT network; this firmware does not
implement them locally.

Build and configure the separate mode:

```sh
./scripts/build-hermes.sh
source scripts/env.sh
idf.py -B build-hermes -DSDKCONFIG=sdkconfig.hermes \
  -DSDKCONFIG_DEFAULTS='sdkconfig.defaults;sdkconfig.hermes.defaults' menuconfig
# Set Wi-Fi SSID/password, MQTT URI/credentials and Hermes site ID there.
./scripts/build-hermes.sh
```

The generated `sdkconfig.hermes` is Git-ignored because credentials become
part of both that file and the firmware image. Do not share either. The
default build has Hermes disabled and needs no credentials. Playback accepts
only 16-bit PCM WAV, with a 512-KB message limit; compressed or float formats
and longer TTS messages need conversion/chunking upstream. Request IDs must be
1..64 printable ASCII characters without space, quote, backslash, `/`, `+` or
`#`. Wi-Fi power saving is disabled in this mode to keep audio latency low.
The satellite runs in its own 8-KB task. To flash it, use
`AIVOX3_BUILD_DIR=build-hermes AIVOX3_HERMES_FLASH_APPROVED=yes` in addition to
the normal flash variables below, and `AIVOX3_BUILD_DIR=build-hermes
./scripts/monitor.sh` to decode its logs with the matching ELF. Use an MQTT broker on a trusted local network; TLS certificate
provisioning is not implemented yet. This is an initial Hermes audio endpoint,
not a validated production satellite.

## One-time microSD formatting

The normal self-test never formats a card. For an explicitly authorized card
format, `./scripts/build-format.sh` creates a separate image. It enables
`format_if_mount_failed` only in that image; ESP-IDF then partitions the card
and creates a FAT volume if no valid FAT volume can be mounted. This erases
existing data on the inserted microSD. After verifying the mount, flash the
normal `build/` image again so future cards cannot be formatted automatically.
The guarded flash script accepts `AIVOX3_BUILD_DIR=build-format` only with the
additional `AIVOX3_SD_FORMAT_APPROVED=yes` flag.

## USB, backup, flash and diagnosis

Connect with a data-capable USB-C cable and press PWR briefly. Run
`./scripts/detect-device.sh`. On Debian, serial access commonly needs membership
in the device's group, usually `dialout` (`sudo usermod -aG dialout "$USER"`,
then log out/in). Verify the actual group using `ls -l /dev/ttyACM*`.
The ESP32-S3 enumerates as `303a:1001` in its USB bootloader and `303a:0009`
after this firmware starts; the detection script accepts both. The device
node's permissions can change across that transition.

The previous partial backup was stopped and deleted at the user's request.
No firmware backup currently exists. To create a new backup later, if desired:

```sh
./scripts/backup-flash.sh
```

The backup reads 256-KB blocks and keeps completed blocks under `backup/`.
After an interrupted run, resume with
`AIVOX3_BACKUP_CHUNK_DIR=/absolute/path/to/backup/chunks-MAC-timestamp ./scripts/backup-flash.sh`.
The backup may contain saved Wi-Fi credentials and other personal data; it is
excluded from Git.

The flash script accepts exactly one serial port, verifies ESP32-S3 identity,
and requires explicit approval. It normally verifies a full backup. If the
user explicitly chooses to flash without one, an additional
`AIVOX3_NO_BACKUP_APPROVED=yes` flag is required instead of
`AIVOX3_BACKUP_FILE`; the original firmware may then be unrecoverable. The
script flashes the default self-test build, not the Hermes profile:

```sh
AIVOX3_EXPECTED_MAC=94:a9:90:d5:0a:0c \
AIVOX3_BACKUP_FILE="$PWD/backup/ai-vox3-full-YYYYMMDD-HHMMSS.bin" \
AIVOX3_FLASH_APPROVED=yes ./scripts/flash.sh
./scripts/monitor.sh
```

ESP32-S3 identity cannot prove the PCB model. Also inspect the physical AI-VOX3
label before approval. The serial self-test reports chip/flash/PSRAM identity,
cycles RGB, probes ES8311, runs microphone-to-speaker I2S loopback, probes
microSD and continuously reports button states at 115200 baud.
The RGB and LCD tests can only confirm their driver calls in software; their
actual light and picture must be checked by eye. The LCD should show red,
green and blue horizontal bands from top to bottom (RGB565 is byte-swapped
for the ST7789's big-endian SPI format). A speaker must be connected
to hear the microphone loopback.

## Device validation (2026-10-11)

The default self-test image was flashed to ESP32-S3 MAC
`94:a9:90:d5:0a:0c` without a backup, as explicitly requested. esptool
verified the written bootloader, partition table and application hashes.
Serial results: 16-MB flash and 8-MB PSRAM detected; buttons, RGB driver,
ES8311/I2S initialization and two-second I2S audio transfer passed. Without a
card, SDMMC initialization timed out. After inserting a card and rebooting,
SDMMC responded, but FatFs returned `FR_NO_FILESYSTEM` (13): no valid FAT
volume was found. This firmware does not enable exFAT support and never
formats the card automatically. LCD was disabled.
Visual RGB output, button presses and audible microphone/speaker output still
need human confirmation. The Hermes build was not flashed.

After explicit user approval, the one-time format profile created a FAT volume
on the inserted `AFGCC` SDHC card (15,193 MB reported capacity). It was then
replaced with the normal self-test image. The final serial run reported
`[PASS] microSD: ESP_OK` and `[PASS] audio loopback: ESP_OK`; the normal image
has `AIVOX3_FORMAT_SD_ONCE` disabled, so it will not format other cards.
