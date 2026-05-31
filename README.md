# esp32-tts-server

Local HTTP text-to-speech server for ESP32-S3. Accepts a URL-encoded text string and returns a WAV file synthesised by PicoTTS. Designed as a companion to [squeezelite-esp32](https://github.com/Baileyflix/ESP32AudioPlayer) to provide station-name announcements without cloud latency.

## Endpoint

```
GET /tts?text=<url-encoded string>
```

Returns a 16 kHz mono 16-bit PCM WAV file. Typical latency: ~1.9 s (1.3 s synthesis + 0.5 s download over local WiFi).

## Hardware

- ESP32-S3 with PSRAM (tested on ESP32-S3-WROOM-1 8 MB PSRAM variant)
- WiFi only — no I²S or audio output hardware required

## Prerequisites

- ESP-IDF **v5.4.1** — install from https://docs.espressif.com/projects/esp-idf/en/v5.4.1/esp32s3/get-started/
- Git (for submodule)

> **Note:** This project requires IDF v5 and will not build with the IDF v4.x Docker image used by squeezelite-esp32.

## Build

```bash
git clone https://github.com/Baileyflix/esp32-tts-server.git
cd esp32-tts-server
git submodule update --init          # fetches DiUS/esp-picotts into components/picotts
source ~/esp/esp-idf/export.sh       # adjust path to your IDF install
idf.py build
```

## Configure WiFi

WiFi credentials are baked into `sdkconfig.defaults` and `sdkconfig`. Edit both before building:

```
CONFIG_TTS_WIFI_SSID="your-ssid"
CONFIG_TTS_WIFI_PASS="your-password"
```

Or use `idf.py menuconfig` → *TTS Server Configuration*.

## Flash

First-time flash (bootloader + partition table + app):

```bash
cd build
curl -s -X POST http://<workbench-ip>:8080/api/flash \
  -F slot=SLOT1 -F chip=esp32s3 -F baud=460800 \
  -F flash_args=@flash_args \
  -F bootloader.bin=@bootloader/bootloader.bin \
  -F partition-table.bin=@partition_table/partition-table.bin \
  -F esp32-tts-server.bin=@esp32-tts-server.bin
```

Or with esptool directly:

```bash
python -m esptool --chip esp32s3 -b 460800 \
  write_flash "@flash_args"
```

## Squeezelite integration

Set the `tts_url` NVS key on the squeezelite device to:

```
http://<tts-server-ip>/tts?text=%s
```

The `%s` placeholder is replaced with the URL-encoded station name at announcement time.

## Acknowledgements

TTS engine: [DiUS/esp-picotts](https://github.com/DiUS/esp-picotts) — PicoTTS port for ESP-IDF.
