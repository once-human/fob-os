"""Control ONKAR OS (firmware v14+, best with v16) from the laptop.  Every call also sets the watch clock to the laptop's local time.
   bash run.sh os_ctl                   status (default)
   bash run.sh os_ctl start | stop      start the screen now (also clears the boot-failure counter) / turn it off
   bash run.sh os_ctl watch             live status every second: touch bytes, button pins, crown (Ctrl+C to stop)
   bash run.sh os_ctl face N            v16: 0 big numbers, 1 analog, 2 modular
   bash run.sh os_ctl set KEY VALUE     keys: autostart bright ramp touch_flags button touch_rst always_on full_rows te_sync
                                        timeout(0..4 = 10s 15s 30s 1min 5min) face accent(0 orange 1 blue 2 green 3 pink 4 purple 5 teal)
                                        e.g.  set full_rows 1  (if parts of the screen show garbage when only a corner changes)
                                              set button P2_2 (or auto)
   bash run.sh os_ctl tap X Y | swipe left|right|up|down | press | longpress | crown up|down   fake input
   bash run.sh os_ctl i2c ADDR W1,W2.. N   raw read on the touch bus, e.g.  i2c 2E 20,00,00,2C 8
   bash run.sh os_ctl probe             search for the touch chip again
 v19:
   bash run.sh os_ctl power            supply voltage, battery %, low-battery mode, last screen start (voltage sag)
   OS always starts by itself (autostart key ignored); "stop" only pauses it for 60 s.
 v18:
   bash run.sh os_ctl buzz tick|soft|tap|click|double|success|long|bump|alarm|boot|all|stop   play a haptic on the watch
   bash run.sh os_ctl set haptic 0-3    off / light / medium / strong        set hap_inv 1  if every haptic feels like a long buzz
   bash run.sh os_ctl crown             live crown view while you turn it (Ctrl+C to stop)
   bash run.sh os_ctl set crown 1       crown decoded from the pins by software (0 = QDEC block);  set crown_div N  counts per click
   bash run.sh os_ctl peek 0x40004000 12   read peripheral registers (here: the QDEC block)
"""
import asyncio, sys, time
from tool5 import connect, cmd, log, pn

KEYS = {"autostart": 1, "bright": 2, "ramp": 3, "touch_flags": 4, "button": 5, "screen": 6, "touch_rst": 7,
        "always_on": 8, "stream": 9, "full_rows": 9, "te_sync": 10, "timeout": 12, "face": 13, "accent": 14,
        "haptic": 15, "crown": 16, "crown_div": 17, "hap_inv": 18, "fastramp": 19}
HAPTICS = ["tick", "soft", "tap", "click", "double", "success", "long", "bump", "alarm", "boot"]
STATES = ["off", "waiting for boot delay", "powering up", "RUNNING", "sleeping", "updating"]
SCREENS = ["clock", "eyes", "paint", "info", "settings"]
SCREENS16 = ["boot animation", "watch face", "app list", "stopwatch", "timer", "sketch", "flashlight", "breathe", "settings", "about"]
FACES = ["big numbers", "analog", "modular"]
EV = {"tap": 1, "left": 2, "right": 3, "up": 4, "down": 5, "long": 6, "press": 7, "longpress": 8, "crownup": 9, "crowndown": 10}
PINS = {f"P{p}_{i}": p * 8 + i for p in range(4) for i in range(8)}


def u16(r, i): return r[i] | r[i + 1] << 8
def u32(r, i): return int.from_bytes(bytes(r[i:i + 4]), "little")
def s32(r, i): return int.from_bytes(bytes(r[i:i + 4]), "little", signed=True)


def show(r):
    if not r or r[0] != 0xB0 or len(r) < 40:
        log("no/short status answer:", r and bytes(r).hex())
        return
    st = STATES[r[1]] if r[1] < len(STATES) else r[1]
    if len(r) >= 80 and r[79] >= 16:
        show16(r, st)
        return
    log(f"state: {st} | screen: {SCREENS[r[8]] if r[8] < 5 else r[8]} | brightness {r[9]} | boot failures {r[2]} | "
        f"start at boot {'on' if r[3] else 'off'} | drawing mode {r[31]} | frames {u32(r, 14)}")
    tch = f"0x{r[4]:02X} on SCL {pn(r[5])} SDA {pn(r[6])}" if r[4] else "not found"
    log(f"touch: {tch} | reads {u32(r, 18)} | gestures {u32(r, 22)} | last x,y {u16(r, 10)},{u16(r, 12)} | "
        f"raw {bytes(r[32:40]).hex(' ')}")
    if len(r) >= 52:
        lv, e = r[40], [u16(r, 41), u16(r, 43), u16(r, 45)]
        log(f"pins (level/changes): P2_2 {lv & 1}/{e[0]}  P2_4 {lv >> 1 & 1}/{e[1]}  P3_1 {lv >> 2 & 1}/{e[2]} | "
            f"crown {s32(r, 47)} | button {'auto' if r[51] == 0xFF else pn(r[51])} presses {u32(r, 26)}")


def show16(r, st):
    scr = SCREENS16[r[8]] if r[8] < len(SCREENS16) else r[8]
    clock = "synced" if r[76] & 1 else "restored after power cut (may be behind)" if r[76] & 2 else "not set"
    log(f"state: {st} | showing: {scr} | face: {FACES[r[72] % 3]} | brightness {r[9]} | boot failures {r[2]} | "
        f"start on power {'on' if r[3] else 'off'} | clock {clock}")
    log(f"speed: last frame {u32(r, 52) / 1000:.1f} ms ({u32(r, 56) / 1000:.1f} ms of it waiting for the send), "
        f"{u32(r, 60)} px | {u32(r, 64) / 10:.1f} fps | slowest frame since last status {u32(r, 68) / 1000:.1f} ms | "
        f"frames {u32(r, 14)} | ramp step {r[78] * 10} ms")
    tch = f"0x{r[4]:02X} on SCL {pn(r[5])} SDA {pn(r[6])}" if r[4] else "not found"
    lv, e = r[40], [u16(r, 41), u16(r, 43), u16(r, 45)]
    log(f"touch: {tch} | reads {u32(r, 18)} touches {u32(r, 22)} | last x,y {u16(r, 10)},{u16(r, 12)} | "
        f"button presses {u32(r, 26)} (P3_1 now {lv >> 2 & 1}) | crown {s32(r, 47)} | INT P2_4 {lv >> 1 & 1}/{e[1]} changes")


async def main():
    a = sys.argv[1:] or ["status"]
    c = await connect()
    r = await cmd(c, [0x01])
    if not r or r[3] < 14:
        raise SystemExit("flash v14 first: bash run.sh ota_flash --go firmware/v14/app_MP.bin")
    t = int(time.time() - time.timezone + (3600 if time.localtime().tm_isdst > 0 else 0))
    await cmd(c, [0x31] + list(t.to_bytes(4, "little")), 2)
    w = a[0]
    if w == "start":
        show(await cmd(c, [0x30, 1]))
        log("screen powering up: takes ~25 s (soft start). Run 'bash run.sh os_ctl' to see it reach RUNNING.")
    elif w == "stop":
        show(await cmd(c, [0x30, 0]))
    elif w in ("screen", "face"):
        await cmd(c, [0x32, 13 if r[3] >= 16 else 6, int(a[1])])
    elif w == "set":
        k, v = KEYS[a[1]], a[2]
        v = 0xFF if v == "auto" else PINS[v.upper()] if v.upper() in PINS else int(v, 0)
        r = await cmd(c, [0x32, k, v])
        log("saved" if r and r[0] == 0xB2 else f"no answer {r}")
    elif w in ("tap", "swipe", "press", "long", "longpress", "crown") and not (w == "crown" and len(a) == 1):
        ev = EV[w if w in ("press", "longpress") else a[1] if w == "swipe" else "crown" + a[1] if w == "crown" else w]
        x, y = (int(a[1]), int(a[2])) if w in ("tap", "long") and len(a) >= 3 else (184, 224)
        await cmd(c, [0x33, ev, x & 255, x >> 8, y & 255, y >> 8])
    elif w == "i2c":
        wr = [int(x, 16) for x in a[2].split(",")] if a[2] != "-" else []
        r = await cmd(c, [0x35, int(a[1], 16), len(wr)] + wr + [int(a[3])])
        log("ACK, data:" if r and r[1] == 0 else "NO ACK:", r and bytes(r[3:]).hex(" "))
    elif w == "probe":
        r = await cmd(c, [0x34], 6)
        log(f"touch chip: 0x{r[1]:02X} on SCL {pn(r[2])} SDA {pn(r[3])}" if r and r[1] else f"none found ({r})")
    elif w == "buzz":
        names = HAPTICS if a[1] == "all" else [a[1]]
        for n in names:
            i = 0xFF if n == "stop" else HAPTICS.index(n) if n in HAPTICS else int(n, 0)
            r = await cmd(c, [0x36, i])
            log(f"haptic {n}:", "played" if r and r[1] else f"NOT played ({r and bytes(r).hex()}) - screen must be running or off, haptics not 0")
            if len(names) > 1:
                await asyncio.sleep(1.2)
    elif w == "peek":
        addr, n = int(a[1], 0), int(a[2]) if len(a) > 2 else 4
        r = await cmd(c, [0x38] + list(addr.to_bytes(4, "little")) + [n])
        if r and r[1] != 0xEE:
            for i in range(r[1]):
                log(f"  0x{addr + 4 * i:08X} = 0x{u32(r, 2 + 4 * i):08X}")
        else:
            log("peek refused (peripheral space 0x40000000-0x400FFFFF only)", r)
    elif w == "crown":
        log("TURN THE CROWN. live view, Ctrl+C to stop")
        last = None
        while True:
            r = await cmd(c, [0x37])
            if r and r[0] == 0xB7:
                cur = (r[2], u16(r, 3), u16(r, 5), s32(r, 7), u32(r, 11), s32(r, 30))
                if cur != last:
                    log(f"mode {'pins' if r[1] else 'QDEC'} | pins A(P1_0)={r[2] >> 1} B(P2_6)={r[2] & 1} | edges A {u16(r, 3)} B {u16(r, 5)} | "
                        f"pin count {s32(r, 7)} | QDEC SR_Y 0x{u32(r, 11):08X} CR_Y 0x{u32(r, 15):08X} INT 0x{u32(r, 19):08X} | "
                        f"crown total {s32(r, 30)} (/{r[34]} per click) | haptics lvl {r[25]} plays {u32(r, 26)}")
                    last = cur
            await asyncio.sleep(0.25)
    elif w == "power":
        r = await cmd(c, [0x37])
        if r and r[0] == 0xB7 and len(r) >= 48:
            real, low = r[39] & 1, r[39] >> 1 & 1
            log(f"supply {u16(r, 36)} mV now, {u16(r, 46)} mV smoothed | "
                f"{'battery ' + str(r[38]) + '%' if real else 'charger only (no battery seen: VBAT never >= 3.6 V)'}"
                f"{' | LOW-BATTERY MODE' if low else ''}")
            log(f"last screen start: idle {u16(r, 42)} mV, lowest {u16(r, 40)} mV, supply waits {r[44]}")
        else:
            log("needs v19:", r and bytes(r).hex())
    elif w == "watch":
        log("live status every second, Ctrl+C to stop")
        while True:
            show(await cmd(c, [0x30, 2]))
            await asyncio.sleep(1)
    if w not in ("watch", "crown"):
        show(await cmd(c, [0x30, 2]))
    await c.disconnect()


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        print("\nstopped.")
