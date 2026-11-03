"""Get control back from ONKAR OS (v14+) + test whether Bluetooth survives the screen.  Ctrl+C to stop.
   v16 also works: it turns "start on power" off. Turn it back on with:  bash run.sh os_ctl set autostart 1
   bash run.sh os_rescue
v14 stops being visible over Bluetooth once its screen is running; for ~6 s after power-up it still advertises.
This script waits for that moment (you: unplug + replug the charger when it beeps twice), connects at once and:
  1. turns autostart OFF and selects drawing mode 1 (one complete transfer per 12-row strip: fixes the glitching)
  2. starts the screen WHILE CONNECTED and reads the status every second for 45 s
     -> tells us whether the connection survives a running screen
  3. disconnects and scans 20 s -> tells us whether the watch advertises again while the screen runs
  4. reconnects if it can, stops the screen
"""
import asyncio, time, subprocess
from bleak import BleakScanner, BleakClient
from tool5 import ADDR, N, W, q, log, beep, cmd, status
from os_ctl import show


async def fast_connect(forever=True, secs=20):
    subprocess.run(["bluetoothctl", "disconnect", ADDR], capture_output=True, timeout=10)
    end, last = time.time() + secs, 0
    while forever or time.time() < end:
        hit = asyncio.Event()
        dev = {}

        def cb(d, adv):
            if d.address.upper() == ADDR or (adv.local_name or "").startswith("ONKAR"):
                dev["d"] = d
                hit.set()
        try:
            async with BleakScanner(cb):
                try:
                    await asyncio.wait_for(hit.wait(), 4)
                except asyncio.TimeoutError:
                    pass
        except Exception as e:
            log("scanner problem:", type(e).__name__)
            await asyncio.sleep(1)
        if "d" not in dev:
            if forever and time.time() - last > 20:
                beep("lost"); beep("lost")
                log("watch not advertising -> UNPLUG the charger, wait 3 s, plug it back in (I'm listening)")
                last = time.time()
            continue
        log("watch seen, connecting now")
        c = BleakClient(dev["d"], timeout=10)
        try:
            await c.connect()
            await c.start_notify(N, lambda _h, d: q.put_nowait(bytes(d)))
            return c
        except Exception as e:
            log("connect failed:", type(e).__name__, e)
            try:
                await c.disconnect()
            except Exception:
                pass
    return None


async def main():
    c = await fast_connect()
    r = await cmd(c, [0x32, 1, 0], 2)
    log("autostart OFF:", "ok" if r and r[0] == 0xB2 else r)
    await cmd(c, [0x30, 0], 2)
    v = await cmd(c, [0x01], 2)
    if v and v[3] < 16:                    # v14/v15 only: key 9 is the drawing mode there (in v16 it means "full rows")
        r = await cmd(c, [0x32, 9, 1], 2)
        log("drawing mode 1:", "ok" if r and r[0] == 0xB2 else r)
    beep("found")
    await status(c)                       # uptime + why it last restarted (power loss vs watchdog)
    show(await cmd(c, [0x30, 2]))

    log("=== starting the screen while connected (takes ~25 s to light). WATCH IT: glitchy or clean?")
    show(await cmd(c, [0x30, 1]))
    survived = True
    for i in range(45):
        await asyncio.sleep(1)
        if not c.is_connected:
            log(f"!!! connection LOST {i + 1} s after screen start -> reconnecting to read why")
            survived = False
            c = await fast_connect(forever=False, secs=30)
            if c:
                await status(c)
                show(await cmd(c, [0x30, 2]))
                await cmd(c, [0x30, 0], 2)
                await c.disconnect()
            beep("found")
            return
        r = await cmd(c, [0x30, 2], 3)
        if r is None:
            log(f"   {i + 1:2d} s: no answer")
        elif i % 5 == 4 or r[1] == 3:
            show(r)
    log("connection survived the running screen:", survived)

    log("=== disconnecting; is the watch visible while the screen runs? (20 s)")
    try:
        await c.disconnect()
    except Exception:
        pass
    await asyncio.sleep(2)
    c = await fast_connect(forever=False, secs=20)
    log("advertising with screen running:", "YES" if c else "NO")
    if c:
        show(await cmd(c, [0x30, 2]))
        await cmd(c, [0x30, 0], 2)
        log("screen stopped. autostart stays OFF until: bash run.sh os_ctl set autostart 1")
        await c.disconnect()
    else:
        log("watch invisible again: unplug/replug -> it boots with autostart OFF now, so it stays reachable")
    beep("found")


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        print("\nstopped.")
