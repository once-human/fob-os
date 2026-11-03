"""Listen on the USB-serial adapter and show what the watch sends while it boots.
   bash tools/run.sh serial_probe            list ports, then listen on the first one at several speeds
   bash tools/run.sh serial_probe /dev/ttyUSB0 2000000      listen at one speed
Wire first: adapter GND -> a G pad, adapter RX -> the watch's TX pad. Leave the adapter's VCC/5V/3V3 pin UNconnected.
Then power-cycle the watch (charger out, in) while this runs. Any bytes at all mean that pad is a live serial output."""
import glob, sys, time
import serial
from serial.tools import list_ports

def log(m): print(time.strftime("%H:%M:%S"), m, flush=True)

ports = [p.device for p in list_ports.comports()]
log("ports: " + (", ".join(ports) if ports else "none (is the adapter plugged in?)"))
if not ports:
    sys.exit(1)
port = sys.argv[1] if len(sys.argv) > 1 else ports[0]
speeds = [int(sys.argv[2])] if len(sys.argv) > 2 else [115200, 921600, 2000000, 1500000, 460800, 9600]
while True:
    for b in speeds:
        try:
            s = serial.Serial(port, b, timeout=0.3)
        except Exception as e:
            log(f"cannot open {port}: {e}"); time.sleep(2); continue
        log(f"listening at {b} baud for 8 s ... (power-cycle the watch now)")
        end, n = time.time() + 8, 0
        while time.time() < end:
            d = s.read(256)
            if d:
                n += len(d)
                log(f"  {len(d)} bytes: {d[:64].hex(' ')}  |  {''.join(chr(c) if 32 <= c < 127 else '.' for c in d[:64])}")
        s.close()
        if n:
            log(f"*** {n} bytes seen at {b} baud: this pad is sending something ***")
