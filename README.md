# Linux/OpenWrt Raw ESP-Touch Sender

This is a small C command-line sender for ESP-Touch provisioning over raw
802.11 frame injection or UDP broadcast. It supports ESP-Touch v1 and
unencrypted ESP-Touch v2.

The program does not switch Wi-Fi mode or channel by itself. Prepare the
wireless interface with `iw` or an OpenWrt script, then pass the monitor
interface to this program when using raw mode.

It implements the ESP-Touch length streams used by Espressif's ESP8266/ESP32
SmartConfig receiver. It does not implement AirKiss or vendor-private
protocols. ESP-Touch v2 AES encryption is not implemented.

## Build

Native build:

```sh
make
```

OpenWrt SDK cross build:

```sh
make CC=<target-openwrt-linux-musl-gcc>
```

## Prepare Monitor Interface

Example on Linux/OpenWrt:

```sh
iw dev wlan0 interface add mon0 type monitor
ip link set mon0 up
iw dev mon0 set channel 6
```

Use the channel of the AP that the IoT device should join. If the target device
is scanning all channels, you can run the sender once per channel from a shell
script.

## Usage

```sh
sudo ./smartconfig --send raw --type v1 -i mon0 -s "YourSSID" -p "YourPassword" \
  -a "aa:bb:cc:dd:ee:ff" -I "192.168.1.2"
```

ESP-Touch v2:

```sh
sudo ./smartconfig --send raw --type v2 -i mon0 -s "YourSSID" -p "YourPassword" \
  -a "aa:bb:cc:dd:ee:ff" -I "192.168.1.2"
```

UDP broadcast mode:

```sh
sudo ./smartconfig --send udp --type v1 -i wlan0 -s "YourSSID" -p "YourPassword" \
  -a "aa:bb:cc:dd:ee:ff" -I "192.168.1.2" -b 255.255.255.255 -P 7001
```

Useful options:

```text
-t, --type TYPE         ESP-Touch type: v1 or v2, default v1
-S, --send MODE         Send mode: raw or udp, default raw
-i, --iface IFACE       Interface: monitor iface for raw, outgoing iface for udp
-s, --ssid SSID         Wi-Fi SSID to provision
-p, --password PASS     Wi-Fi password to provision
-m, --src-mac MAC       Transmitter MAC, default interface MAC
-a, --bssid MAC         Target AP BSSID, required for ESP-Touch
-I, --ip ADDR           Sender/local IPv4 address, default 192.168.1.2
-b, --broadcast ADDR    UDP broadcast address, default 255.255.255.255
-P, --port PORT         UDP destination port, default 7001
-M, --app-port-mark N   ESP-Touch v2 app port mark 0..3, default 0
-r, --repeat COUNT      Number of full transmit cycles, default 80
-d, --delay-us USEC     Delay between packets/frames, default 8000
-n, --dry-run           Print the ESP-Touch lengths without sending frames
-v, --verbose           Print transmit details
-h, --help              Show help
```

Root is required for raw mode because the sender opens an `AF_PACKET/SOCK_RAW`
socket. UDP mode usually also needs root when binding to a specific interface
with `SO_BINDTODEVICE`.

Find the AP BSSID with:

```sh
iw dev wlan0 scan | grep -A5 'SSID: YourSSID'
```

## ESP-Touch v1 Format

Each transmit cycle sends ESP-Touch v1 guide lengths followed by DatumCode
lengths.

Guide lengths:

```text
515, 514, 513, 512
```

DatumCode uses Espressif's v1 format:

```text
data = total_len + password_len + ssid_crc + bssid_crc + total_xor
     + sender_ip + password + ssid + bssid

each byte is encoded as three lengths:
  0x00 + crc_high/data_high + 40
  0x01 + sequence_index     + 40
  0x00 + crc_low/data_low   + 40
```

The raw injected packet is:

```text
radiotap header, 8 bytes
802.11 data header, 24 bytes
variable body, body length equal to the ESP-Touch length
```

The ESP device must be in SmartConfig/ESP-Touch mode and sniffing the same
2.4 GHz channel.

## UDP Mode

UDP mode sends the same ESP-Touch length stream as UDP broadcast payload
lengths. This is how phone apps commonly trigger SmartConfig through the normal
Wi-Fi network stack:

```text
app/program -> UDP broadcast -> Wi-Fi driver/AP -> 802.11 air frames
```

For an OpenWrt router, UDP mode is useful when the router/AP interface will
actually transmit those broadcast frames over the target 2.4 GHz radio. Raw
mode is more direct because it injects the 802.11 frame lengths itself.

## ESP-Touch v2 Format

ESP-Touch v2 uses a different packet-length stream and is not compatible with
v1. This sender implements the unencrypted v2 path from Espressif's Android
library:

```text
sync length          = 1048
sequence size length = 1072 + total_sequence_count - 1
sequence length      = 128 + sequence
data length          = (bit_index << 7) | (1 << 6) | six_bit_data
```

The v2 header contains SSID length, password length, BSSID CRC, app port mark,
version, IPv4 flag, and header CRC. Password and SSID bytes are then sent in
6-byte or 5-byte groups depending on whether byte encoding is required.
