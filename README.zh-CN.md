# Linux/OpenWrt UDP ESP-Touch 发送器

[English](./README.md)

这是一个用于 ESP-Touch 配网的 C 语言命令行工具，通过 UDP broadcast 发送
ESP-Touch 长度编码数据。它支持 ESP-Touch v1 和未加密的 ESP-Touch v2。

它的工作方式接近常见 Android/Java 配网客户端：程序发送特定长度的 UDP 包，
Wi-Fi 驱动或 AP 会把这些包转换为 ESP 设备在 SmartConfig 模式下可以监听到
的 802.11 空口帧。

本工具不实现 AirKiss 或其它厂商私有协议，也不实现 ESP-Touch v2 AES 加密。

## 演示

<video src="./test.mp4" controls width="720"></video>

[下载演示视频](./test.mp4)

## 编译

本机编译：

```sh
make
```

OpenWrt SDK 交叉编译：

```sh
make CC=<target-openwrt-linux-musl-gcc>
```

## 使用

ESP-Touch v1，类似 Android App 的用法：

```sh
sudo ./smartconfig --type v1 -s "你的SSID" -p "你的密码"
```

ESP-Touch v2：

```sh
sudo ./smartconfig --type v2 -s "你的SSID" -p "你的密码"
```

如果没有指定 `-i`、`-I`、`-b`，程序会在所有非 loopback、带 IPv4 broadcast
地址的网卡上发送。对于 ESP-Touch v1，每个网卡都会使用自己的本机 IP 生成一份
长度流，因为发送端 IP 会被编码进配网数据里。

如果指定了 `-i`、`-I`、`-b` 中任意一个参数，程序会进入单网卡模式，并尽量
自动补齐缺失的本机 IP 或广播地址。

如果没有指定 `-a`，BSSID 默认使用 `00:00:00:00:00:00`，这和 `UsbTerminal`
里的 Android App 行为一致。

默认情况下，程序会监听 Espressif SmartConfig ACK，端口为 UDP 18266。设备回包
成功时会输出：

```text
ACK success: mac=aa:bb:cc:dd:ee:ff ip=192.168.1.123
```

如果只想发送、不等待设备确认，可以使用 `--no-ack`。

常用参数：

```text
-t, --type TYPE         ESP-Touch 类型：v1 或 v2，默认 v1
-i, --iface IFACE       发送网卡，默认所有支持 IPv4 broadcast 的网卡
-s, --ssid SSID         要配置的 Wi-Fi SSID
-p, --password PASS     要配置的 Wi-Fi 密码
-a, --bssid MAC         目标 AP BSSID，默认 00:00:00:00:00:00
-I, --ip ADDR           发送端/本机 IPv4 地址，默认自动
-b, --broadcast ADDR    UDP 广播地址，默认自动
-P, --port PORT         UDP 目标端口，默认 7001
-A, --ack-port PORT     UDP ACK 监听端口，默认 18266
-W, --ack-wait-ms MS    等待 ACK 的总时间，默认 60000
-N, --no-ack            不监听 ESP-Touch 成功 ACK
-M, --app-port-mark N   ESP-Touch v2 app port mark，范围 0..3，默认 0
-r, --repeat COUNT      完整发送周期次数，默认 80
-d, --delay-us USEC     UDP 包之间的延时，默认 8000
-n, --dry-run           只打印 ESP-Touch 长度流，不实际发送
-v, --verbose           打印发送细节
-h, --help              显示帮助
```

单网卡模式下通常需要 root 权限，因为程序会通过 `SO_BINDTODEVICE` 绑定到指定
网卡。默认多网卡模式不主动绑定网卡，而是发送到各接口对应的定向广播地址，让
内核按路由选择出口。

查找 AP BSSID：

```sh
iw dev wlan0 scan | grep -A5 'SSID: 你的SSID'
```

在 OpenWrt 上，应使用实际会通过目标 2.4 GHz 射频发出广播帧的 AP 或桥接接口。
接口名称可能是 `br-lan`、`wlan0`、`phy0-ap0` 等：

```sh
iw dev
```

## ESP-Touch v1 格式

每个发送周期先发送 ESP-Touch v1 guide lengths，再发送 DatumCode lengths。

Guide lengths：

```text
515, 514, 513, 512
```

DatumCode 使用 Espressif v1 格式：

```text
data = total_len + password_len + ssid_crc + bssid_crc + total_xor
     + sender_ip + password + ssid + bssid

每个字节编码为三个长度：
  0x00 + crc_high/data_high + 40
  0x01 + sequence_index     + 40
  0x00 + crc_low/data_low   + 40
```

## ESP-Touch v2 格式

ESP-Touch v2 使用不同的包长度流，和 v1 不兼容。本工具实现的是 Espressif
Android 库中的未加密 v2 路径：

```text
sync length          = 1048
sequence size length = 1072 + total_sequence_count - 1
sequence length      = 128 + sequence
data length          = (bit_index << 7) | (1 << 6) | six_bit_data
```

v2 头部包含 SSID 长度、密码长度、BSSID CRC、app port mark、版本、IPv4 标志和
头部 CRC。密码和 SSID 会根据是否需要字节编码，按 6 字节或 5 字节一组发送。

## 授权

本项目使用 GPL-3.0-only 授权。详见 [LICENSE](./LICENSE)。
