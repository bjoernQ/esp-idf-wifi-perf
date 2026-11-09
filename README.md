# Wi-Fi bench

ESP-IDF client for the same TCP benchmark as
`esp-hal/examples/qa/src/bin/embassy-wifi-bench.rs`.

It joins a Wi-Fi network, then loops three 15 second tests against
`extras/bench-server`:

| Test | Port | What is counted |
| --- | --- | --- |
| Download | 4321 | Bytes received |
| Upload | 4322 | Bytes sent |
| Upload and download | 4323 | Bytes received while sending at the same time |

Results use the same formula as the embassy test: `(bytes + 512) / 1024 / 15` and are printed as `download: N kB/s`, `upload: N kB/s`, and `upload+download: N kB/s`. The next round starts after 10 seconds.

The client follows `embassy-wifi-bench`: 1024-byte reads and writes, 16 KiB TCP send and receive buffers, a 10 second socket timeout, and a 15 second limit around each test. Upload and download run at the same time on port 4323. Wi-Fi power save is off.

`sdkconfig.defaults` and `sdkconfig.defaults.<chip>` still carry the iperf example's Wi-Fi buffer, AMPDU, IRAM, compiler, CPU, tick, watchdog, and PHY settings. The TCP window lines from that example are not used, so every chip keeps the Rust example's 16 KiB socket buffers.

## Server

On the host, from an `esp-hal` checkout:

```text
cd extras/bench-server
cargo run --release
```

The server listens on `0.0.0.0:4321`, `4322`, and `4323`. Use the host's address on the Wi-Fi LAN as `BENCH_HOST_IP`.

## Configure and flash

```text
idf.py menuconfig
```

Set **Wi-Fi Bench Configuration**: SSID, password, and the server address.

```text
idf.py set-target esp32s3
idf.py build flash monitor
```

Replace `esp32s3` with the chip you are using. Per-target defaults exist for the same chips as the iperf example.
