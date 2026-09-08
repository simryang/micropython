The ESP32 W5500 Dev Kit pairs an ESP32-S3-WROOM-1 module (16 MiB flash, 8 MiB
octal PSRAM) with a WIZnet W5500 Ethernet controller on the FSPI bus.  This
firmware drives the W5500 in hardware-socket (TOE) mode: the chip runs the
TCP/IP stack itself, and MicroPython's sockets are its hardware sockets.

The wiring is part of the board definition, so the interface needs no
arguments:

```python
import network
nic = network.WIZNET_TOE()
nic.active(True)
nic.ifconfig('dhcp')
```

| Signal | GPIO |
| --- | --- |
| SCK | 12 |
| MOSI | 11 |
| MISO | 13 |
| CS | 10 |
| RESET | 9 |
| INT | 14 (not used by the driver) |

GPIO 9 being the W5500's RESET, the board has no default I2C pins: pass
`scl=` and `sda=` to `machine.I2C`.
