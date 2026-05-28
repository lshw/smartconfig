# Linux/OpenWrt UDP ESP-Touch Sender

This is a small C command-line sender for ESP-Touch provisioning over UDP
broadcast. It supports ESP-Touch v1 and unencrypted ESP-Touch v2.

It matches the common Java/Android client model: the program sends UDP packets
with protocol-specific payload lengths, and the Wi-Fi driver/AP turns those
packets into 802.11 air frames that the ESP device can sniff while in
SmartConfig mode.

It does not implement AirKiss or vendor-private protocols. ESP-Touch v2 AES
encryption is not implemented.

## Build

Native build:

```sh
make
```

OpenWrt SDK cross build:

```sh
make CC=<target-openwrt-linux-musl-gcc>
```

## Usage

ESP-Touch v1, Android-app style:

```sh
sudo ./smartconfig --type v1 -s "YourSSID" -p "YourPassword"
```

ESP-Touch v2:

```sh
sudo ./smartconfig --type v2 -s "YourSSID" -p "YourPassword"
```

If `-i`, `-I`, and `-b` are all omitted, the tool sends on every non-loopback
IPv4 interface that has a broadcast address. For ESP-Touch v1, each interface
gets its own packet stream because the sender IP is encoded into the data.

If any of `-i`, `-I`, or `-b` is provided, the tool uses single-interface mode
and fills in the remaining values automatically when possible.

If `-a` is omitted, it uses `00:00:00:00:00:00`, matching the Android app in
`UsbTerminal`.

By default the tool also listens for Espressif's SmartConfig ACK on UDP port
18266. A successful device reply is printed as:

```text
ACK success: mac=aa:bb:cc:dd:ee:ff ip=192.168.1.123
```

Use `--no-ack` to keep the old send-only behavior.

Useful options:

```text
-t, --type TYPE         ESP-Touch type: v1 or v2, default v1
-i, --iface IFACE       Outgoing Wi-Fi interface, default all broadcast-capable IPv4 interfaces
-s, --ssid SSID         Wi-Fi SSID to provision
-p, --password PASS     Wi-Fi password to provision
-a, --bssid MAC         Target AP BSSID, default 00:00:00:00:00:00
-I, --ip ADDR           Sender/local IPv4 address, default auto
-b, --broadcast ADDR    UDP broadcast address, default auto
-P, --port PORT         UDP destination port, default 7001
-A, --ack-port PORT     UDP ACK listen port, default 18266
-W, --ack-wait-ms MS    Total ACK wait time, default 60000
-N, --no-ack            Do not listen for ESP-Touch success ACK
-M, --app-port-mark N   ESP-Touch v2 app port mark 0..3, default 0
-r, --repeat COUNT      Number of full transmit cycles, default 80
-d, --delay-us USEC     Delay between UDP packets, default 8000
-n, --dry-run           Print the ESP-Touch lengths without sending
-v, --verbose           Print transmit details
-h, --help              Show help
```

Root is usually required because the sender binds the socket to a specific
interface with `SO_BINDTODEVICE`.

Find the AP BSSID with:

```sh
iw dev wlan0 scan | grep -A5 'SSID: YourSSID'
```

On OpenWrt, use the AP interface that actually transmits on the target 2.4 GHz
radio. It may be named `wlan0`, `phy0-ap0`, or similar:

```sh
iw dev
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
