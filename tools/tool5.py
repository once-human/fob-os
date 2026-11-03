"""Laptop side of ONKAR-LIFE v5.  Every run is logged by run.sh into logs/.
  bash run.sh tool5 display       backlight hunt -> try the screen on the likely pins -> full display hunt -> paint it
  bash run.sh tool5 map           read-only pin dump + 'which pin keeps the watch alive' test

  bash run.sh tool5 status        firmware version, uptime, watchdog, reset reason
  bash run.sh tool5 motor         buzzes the 3 motor suspects one by one, you say which one buzzed
  bash run.sh tool5 findbattery   charger unplugged vs plugged: finds the battery ADC pin + charger pin
  bash run.sh tool5 battery       live battery %, voltage, charging (Ctrl+C to stop)
  bash run.sh tool5 i2c           finds I2C buses + chips (touch, motion sensor, heart-rate...)
  bash run.sh tool5 lcdhunt       finds the display wires by asking every pin combination for a panel ID
  bash run.sh tool5 lcdtest CS CLK MOSI   tries to wake the panel and paint it red/green/blue
  bash run.sh tool5 auto          status -> motor -> (findbattery if battery is in) -> i2c -> lcdhunt
  bash run.sh tool5 release       let go of every pin the watch is driving

Findings are saved in logs/hw.json and reused by the next command.
"""
import asyncio, sys, time, json, os, subprocess
from bleak import BleakScanner, BleakClient

ADDR = "00:E0:12:34:56:78"
W = "0000b002-0000-1000-8000-00805f9b34fb"
N = "0000b003-0000-1000-8000-00805f9b34fb"
NAMES = {**{i: f"P0_{i}" for i in range(8)}, **{8 + i: f"P1_{i}" for i in range(8)},
         **{16 + i: f"P2_{i}" for i in range(8)}, **{24 + i: f"P3_{i}" for i in range(7)},
         32: "P4_0", 33: "P4_1", 34: "P4_2", 35: "P4_3"}
VALID = sorted(NAMES)                      # firmware refuses 31 and H_0/H_1/H_2 anyway
MOTOR_SUSPECTS = [6, 7, 35, 32, 33, 34]   # every pin the v2 screen probe touched when it started buzzing
HERE = os.path.dirname(os.path.abspath(__file__))
HWJ = os.path.join(HERE, "logs", "hw.json")
I2C_KNOWN = {0x15: "touch (CST816/CST716 family)", 0x38: "touch (FT6x06/FT3x68)", 0x5D: "touch (GT911)",
             0x2E: "touch (Hynitron/HYN)", 0x18: "motion sensor (SC7A20/LIS3DH/BMA4xx)",
             0x19: "motion sensor (SC7A20/LIS3DH)", 0x0F: "motion sensor (STK8321/KX)", 0x68: "motion sensor (BMI/MPU)",
             0x6A: "motion sensor (LSM6/QMI8658)", 0x6B: "motion sensor (LSM6/QMI8658)", 0x12: "motion (QMA7981)",
             0x44: "heart-rate (HRS3300)", 0x33: "heart-rate (VC31/VC32)", 0x57: "heart-rate/SpO2 (MAX3010x)",
             0x51: "RTC (PCF8563)", 0x55: "fuel gauge (BQ27xxx)", 0x36: "fuel gauge (MAX1704x)"}
q = asyncio.Queue()


def log(*a):
    print(time.strftime("%H:%M:%S"), *a, flush=True)


def load_hw():
    try:
        return json.load(open(HWJ))
    except Exception:
        return {}


def save_hw(**kv):
    hw = load_hw()
    hw.update(kv)
    json.dump(hw, open(HWJ, "w"), indent=1)
    txt = repr(kv)
    log("saved to logs/hw.json:", txt if len(txt) < 160 else ", ".join(kv) + " (full detail in the file)")


def pn(p):
    return NAMES.get(p, str(p))


class WatchGone(Exception):
    pass


async def cmd(c, b, wait=4.0):
    while not q.empty():
        q.get_nowait()
    if not c.is_connected:
        raise WatchGone()
    if wait > 10:
        # long jobs (display hunt): don't let a slow write-acknowledge abort us; the answer comes as a notification
        async def _w():
            try:
                await c.write_gatt_char(W, bytes(b), response=True)
            except Exception as e:
                log("(write ack slow:", type(e).__name__, "- still waiting for the answer)")
        asyncio.ensure_future(_w())
    else:
        try:
            await c.write_gatt_char(W, bytes(b), response=True)
        except Exception as e:
            if not c.is_connected:
                raise WatchGone()
            log("command 0x%02X failed: %s" % (b[0], e))
            return None
    try:
        return await asyncio.wait_for(q.get(), wait)
    except asyncio.TimeoutError:
        return None


def beep(kind):
    """Only the low warning sound.  found / done: 1 beep.  lost (needs your attention): 2 beeps + popup."""
    snd = "/usr/share/sounds/freedesktop/stereo/dialog-warning.oga"
    for _ in range(2 if kind == "lost" else 1):
        subprocess.Popen(["paplay", snd], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        time.sleep(0.45)
    if kind == "lost":
        subprocess.Popen(["notify-send", "-u", "critical", "WATCH NOT VISIBLE - replug the charger"],
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


async def connect(max_wait=None):
    """Never gives up. Beeps 'lost' every ~15 s while the watch is missing, 'found' when it shows up."""
    subprocess.run(["bluetoothctl", "power", "on"], capture_output=True, timeout=10)
    subprocess.run(["bluetoothctl", "disconnect", ADDR], capture_output=True, timeout=10)
    subprocess.run(["bluetoothctl", "remove", ADDR], capture_output=True, timeout=10)
    await asyncio.sleep(1)
    missing_since, last_beep, fails = None, 0, 0
    while True:
        dev = None
        try:
            found = await BleakScanner.discover(timeout=5, return_adv=True)
            dev = next((d for a, (d, adv) in found.items()
                        if (adv.local_name or "").startswith("ONKAR") or a.upper() == ADDR), None)
        except Exception as e:
            log("laptop Bluetooth problem:", type(e).__name__, "- turning it back on")
            subprocess.run(["bluetoothctl", "power", "on"], capture_output=True, timeout=10)
            await asyncio.sleep(2)
        if not dev:
            missing_since = missing_since or time.time()
            gone = time.time() - missing_since
            if time.time() - last_beep > 15:
                beep("lost")
                last_beep = time.time()
            log(f"watch NOT visible for {gone:.0f} s -> unplug + replug the charger (I keep trying forever, Ctrl+C to quit)")
            continue
        if missing_since and time.time() - missing_since > 10:
            beep("found")
        missing_since = None
        c = BleakClient(dev, timeout=20)
        try:
            await c.connect()
            await c.start_notify(N, lambda _h, d: q.put_nowait(bytes(d)))   # can fail right after a reboot: retry too
            break
        except Exception as e:
            fails += 1
            log(f"watch seen but connect failed ({type(e).__name__}) - retry {fails}")
            try:
                await c.disconnect()
            except Exception:
                pass
            subprocess.run(["bluetoothctl", "disconnect", ADDR], capture_output=True, timeout=10)
            if fails % 3 == 0:
                log("resetting the laptop's Bluetooth")
                subprocess.run(["bluetoothctl", "power", "off"], capture_output=True, timeout=10)
                await asyncio.sleep(2)
                subprocess.run(["bluetoothctl", "power", "on"], capture_output=True, timeout=10)
            await asyncio.sleep(3)
    r = await cmd(c, [0x01])
    ver = r[3] if r and r[0] == 0x81 else None
    log("connected:", dev.address, f"firmware v{ver}" if ver else f"(ping reply {r})")
    if ver is not None and ver < 6:
        raise SystemExit("watch runs v%d - flash v6 first: bash run.sh ota_flash --go firmware/v6/app_MP.bin" % ver)
    return c


# ---------------------------------------------------------------- status

RESET = {0x00: "power-on / hardware reset", 0x01: "WATCHDOG fired (it froze and rebooted itself)",
         0xD4: "firmware update finished", 0xD5: "firmware update failed", 0xD3: "switched to OTA mode"}


async def status(c):
    r = await cmd(c, [0x10])
    if not r or r[0] != 0x90:
        log("no status reply", r)
        return
    up = int.from_bytes(r[2:6], "little")
    driven = int.from_bytes(r[8:13], "little")
    log(f"firmware v{r[1]}  uptime {up // 60}m{up % 60:02d}s  last reset: {RESET.get(r[6], hex(r[6]))} (best guess, chip says it may be unreliable)")
    log("watchdog:", "ARMED (8 s)" if r[7] & 1 else "not armed yet", "| paused for update" if r[7] & 2 else "",
        "| heartbeats", int.from_bytes(r[13:17], "little"))
    log("pins being driven:", [pn(p) for p in VALID if driven >> p & 1] or "none")


async def release(c):
    r = await cmd(c, [0x0B])
    log("released", r[1] if r else "?", "pins")


# ---------------------------------------------------------------- pins + adc

async def read_all(c, pull):
    r = await cmd(c, [0x05, pull], 6)
    if not r or r[0] != 0x85:
        return None
    bits = int.from_bytes(r[1:6], "little")
    skipped = int.from_bytes(r[6:11], "little")
    return {p: (bits >> p & 1) for p in VALID if not skipped >> p & 1}


async def adc_sweep(c):
    r = await cmd(c, [0x11], 6)
    if not r or r[0] != 0x91:
        return None
    out = {}
    for i in range(9):
        raw = int.from_bytes(r[1 + 4 * i:3 + 4 * i], "little")
        mv = int.from_bytes(r[3 + 4 * i:5 + 4 * i], "little")
        out["VBAT" if i == 8 else f"P2_{i}"] = None if raw == 0xFFFF else mv
    return out


async def sample(c, n=4):
    adcs, pins_none, pins_down = [], [], []
    for _ in range(n):
        adcs.append(await adc_sweep(c))
        pins_none.append(await read_all(c, 0))
        pins_down.append(await read_all(c, 2))
        await asyncio.sleep(1)
    avg = {}
    for k in adcs[0]:
        vals = [a[k] for a in adcs if a and a[k] is not None]
        avg[k] = round(sum(vals) / len(vals)) if vals else None
    def stable(snaps):
        out = {}
        for p in VALID:
            v = {s[p] for s in snaps if s and p in s}
            out[p] = v.pop() if len(v) == 1 else "flicker"
        return out
    return avg, stable(pins_none), stable(pins_down)


def ask(prompt):
    try:
        return input(time.strftime("%H:%M:%S") + " >>> " + prompt + " ").strip().lower()
    except EOFError:
        return ""


async def findbattery(c):
    ask("UNPLUG the charger, then press Enter")
    log("measuring (unplugged)...")
    a0, n0, d0 = await sample(c)
    log("ADC unplugged (mV):", a0)
    ask("Now PLUG the charger in, wait 3 seconds, press Enter")
    log("measuring (charging)...")
    a1, n1, d1 = await sample(c)
    log("ADC charging  (mV):", a1)
    rows = []
    for k in a0:
        if a0[k] is None or a1[k] is None:
            continue
        rows.append((a1[k] - a0[k], k, a0[k], a1[k]))
    rows.sort(reverse=True)
    for dlt, k, x, y in rows:
        log(f"  {k:5s} {x:5d} -> {y:5d} mV  ({dlt:+d})")
    # battery through a divider sits at ~1.6-2.2 V and rises a little on charge; a charger-sense pin jumps a lot
    batt = [k for dlt, k, x, y in rows if k != "VBAT" and 1300 <= x <= 2300 and dlt >= 5]
    chg_adc = [k for dlt, k, x, y in rows if k != "VBAT" and dlt >= 800]
    chg_pins = sorted({p for p in VALID if n0.get(p) in (0, 1) and n1.get(p) in (0, 1) and n0[p] != n1[p]} |
                      {p for p in VALID if d0.get(p) in (0, 1) and d1.get(p) in (0, 1) and d0[p] != d1[p]})
    log("pins that changed when the charger went in:", [f"{pn(p)} {n0.get(p)}->{n1.get(p)} (pull-down {d0.get(p)}->{d1.get(p)})" for p in chg_pins] or "none")
    log("battery ADC candidates (1.3-2.3 V and rising):", batt or "none")
    log("charger-sense ADC candidates:", chg_adc or "none")
    if not batt and a0.get("VBAT") and a1.get("VBAT") and a1["VBAT"] - a0["VBAT"] >= 30:
        log("VBAT itself rose on charge -> the chip's own VBAT pin is the battery after all")
        batt = ["VBAT"]
    if not batt:
        flat = [k for dlt, k, x, y in rows if k != "VBAT" and 1300 <= x <= 2300]
        if flat:
            log(f"no channel ROSE on charge, but {flat} sits in battery range - using {flat[0]} (UNCERTAIN: check the battery wires are making contact)")
            batt = flat[:1]
    if not batt:
        log("NO battery channel found. Paste this log to Claude - the battery may sit on a pin outside P2_0..P2_7.")
    save_hw(battery_adc=batt[0] if batt else None, battery_unplugged_mv=a0.get(batt[0]) if batt else None,
            charger_pins=chg_pins, charger_pin_unplugged={str(p): n0.get(p) for p in chg_pins},
            charger_adc=chg_adc, adc_unplugged=a0, adc_charging=a1)


PCT = [(3400, 0), (3500, 6), (3600, 14), (3650, 22), (3700, 32), (3750, 42), (3800, 52), (3900, 65), (4000, 78), (4100, 90), (4200, 100)]


def pct(mv):
    if mv <= PCT[0][0]:
        return 0
    for (m0, p0), (m1, p1) in zip(PCT, PCT[1:]):
        if mv <= m1:
            return round(p0 + (mv - m0) * (p1 - p0) / (m1 - m0))
    return 100


async def battery(c):
    hw = load_hw()
    ch = hw.get("battery_adc")
    ratio = hw.get("battery_ratio", 2.0 if ch and ch != "VBAT" else 1.0)
    chg_pins = hw.get("charger_pins", [])
    if not ch:
        log("battery pin not known yet -> run: bash run.sh tool5 findbattery   (showing raw values meanwhile)")
    else:
        log(f"battery on {ch}, divider ratio {ratio} (ASSUMED 1:2 unless you calibrated it - compare with a multimeter once)")
    hist = []
    while True:
        a = await adc_sweep(c)
        if not a:
            log("no reply")
            await asyncio.sleep(3)
            continue
        line = " ".join(f"{k}={v}" for k, v in a.items())
        if ch and a.get(ch):
            mv = round(a[ch] * ratio)
            hist = (hist + [mv])[-10:]
            trend = hist[-1] - hist[0] if len(hist) >= 5 else 0
            state = ""
            unpl = hw.get("charger_pin_unplugged") or {}
            if chg_pins and unpl:
                snap = await read_all(c, 0)
                if snap:
                    plugged = any(snap.get(p) is not None and snap[p] != unpl.get(str(p)) for p in chg_pins)
                    state = "CHARGING (charger pin)" if plugged else "not charging (charger pin)"
            if not state:
                state = "CHARGING (by trend)" if trend >= 10 else ("not charging" if len(hist) >= 5 else "measuring...")
            log(f"BATTERY {pct(mv):3d}%  {mv} mV  {state}    [{line}]")
        else:
            log("raw mV:", line)
        await asyncio.sleep(3)


# ---------------------------------------------------------------- map (v4)

PMX = {0: "idle", 1: "HCI_UART", 6: "UART0_TX?", 0x37: "DWGPIO?", 0x47: "DWGPIO"}


def decode_pad(b0, b1, pmx):
    sw = (b1 & 0x02) == 0
    oe = bool(b0 & 0x02)
    pull = "none" if not (b0 & 0x04) else ("down" if b0 & 0x08 else "up")
    return {"mode": "software" if sw else "peripheral", "output": oe, "level": b0 & 1, "pull": pull,
            "pinmux": pmx, "powered": bool(b1 & 0x01), "boot_output": sw and oe}


async def pad_dump(c):
    r = await cmd(c, [0x18], 6)
    if not r or r[0] != 0x98:
        log("pad dump: no reply", r)
        return {}
    pads = {}
    for pin in range(39):
        if pin == 31:
            continue
        b0, b1, pmx = r[1 + 3 * pin:4 + 3 * pin]
        pads[pin] = decode_pad(b0, b1, pmx) | {"raw": f"{b0:02x} {b1:02x} {pmx:02x}"}
    return pads


async def watch_back(c_old):
    try:
        await c_old.disconnect()
    except Exception:
        pass
    log("The watch went down. Unplug the charger, plug it back in - I'll find it again by myself.")
    return await connect()


async def map_pins(c):
    pads = await pad_dump(c)
    names = {**NAMES, 36: "H_0", 37: "H_1", 38: "H_2"}
    log("PIN STATE AT BOOT (read-only, nothing touched):")
    for pin, d in pads.items():
        flag = "  <== DRIVEN BY BOOT CODE (will never be touched)" if d["boot_output"] else ""
        log(f"  {names[pin]:5s} {d['mode']:10s} {'OUT=' + str(d['level']) if d['output'] else 'in   '} pull={d['pull']:4s} func=0x{d['pinmux']:02x} raw={d['raw']}{flag}")
    hw = load_hw()
    danger = set(hw.get("danger", []))
    done = set(hw.get("float_ok", []))
    todo = [p for p in VALID if p not in danger and p not in done and not pads.get(p, {}).get("boot_output")]
    log(f"Now letting each pin float for 1.5 s, one at a time, to find any pin the watch needs to stay on ({len(todo)} pins).")
    for p in todo:
        try:
            r = await cmd(c, [0x19, p, 0xDC, 0x05], 6)
            alive = await cmd(c, [0x01], 3) if r else None
        except WatchGone:
            r = alive = None
        if r and alive:
            done.add(p)
            hw2 = load_hw(); hw2["float_ok"] = sorted(done); json.dump(hw2, open(HWJ, "w"), indent=1)
            log(f"  {pn(p)} ok")
            continue
        log(f"!!! {pn(p)} IS CRITICAL - floating it killed the watch. It goes on the never-touch list.")
        danger.add(p)
        save_hw(danger=sorted(danger))
        c = await watch_back(c)
    log("safe to use:", len(done), "pins | never touch:", [pn(p) for p in sorted(danger)] or "none")
    save_hw(pads={str(k): v for k, v in pads.items()})
    return c


# ---------------------------------------------------------------- display (v5)

# pins that already do a job at boot: debug (SWD), log/HCI UART, our button input, the two power pins
NOT_DISPLAY = {16 + 4: "P2_4 button input", 8: "P1_0 SWD", 9: "P1_1 SWD", 3: "P0_3 log UART",
               24: "P3_0 HCI UART", 25: "P3_1 HCI UART", 13: "P1_5 pulled-up at boot",
               11: "P1_3 keeps the watch alive", 12: "P1_4 keeps the watch alive"}


def safe_pins(hw):
    danger = set(hw.get("danger", []))
    return [p for p in VALID if p not in danger and p not in NOT_DISPLAY]


async def alive(c):
    try:
        r = await cmd(c, [0x01], 3)
        return bool(r and r[0] == 0x81)
    except WatchGone:
        return False


async def mark_dead(c, p, why):
    hw = load_hw()
    d = set(hw.get("danger", []))
    d.add(p)
    save_hw(danger=sorted(d))
    log(f"!!! {pn(p)} {why} killed the watch -> never-touch list.")
    return await watch_back(c)


async def backlight(c):
    hw = load_hw()
    pins = safe_pins(hw)
    for level in (1, 0):
        word = "HIGH" if level else "LOW"
        ask(f"BACKLIGHT TEST ({word}): look at the WATCH SCREEN, not the laptop. Press Enter to start ({len(pins)} pins, ~{len(pins) * 1.5:.0f} s)")
        for i, p in enumerate(pins, 1):
            print(f"\r   #{i:2d} {pn(p)} {word}   ", end="", flush=True)
            try:
                await cmd(c, [0x03, p, level], 3)
                await asyncio.sleep(1.2)
                await cmd(c, [0x0A, p], 3)
            except WatchGone:
                pass
            if not await alive(c):
                print()
                c = await mark_dead(c, p, f"driving {word}")
        print()
        a = ask("Did the screen glow at any point? Type the # number you saw (e.g. 7), or just Enter for no:")
        if a.strip().isdigit() and 1 <= int(a) <= len(pins):
            n = int(a)
            for p in pins[max(0, n - 2):n + 1]:          # confirm, allowing for reaction delay
                await cmd(c, [0x03, p, level], 3)
                b = ask(f"Is it glowing NOW? ({pn(p)} {word}) (y/n)")
                if b.startswith("y"):
                    save_hw(backlight={"pin": p, "level": level})
                    log(f"BACKLIGHT = {pn(p)} {word}. Keeping it on.")
                    return c, (p, level)
                await cmd(c, [0x0A, p], 3)
    log("no backlight found (AMOLED panels have none - that's fine, carrying on)")
    save_hw(backlight=None)
    return c, None


async def keep(c, held):
    if held:
        await cmd(c, [0x03, held[0], held[1]], 3)


async def blind_try(c, held):
    """Realtek's reference board wiring: CS P4_3, CLK P4_0, DATA P4_2. Just init + paint, no reading."""
    log("Trying the screen on Realtek's reference wiring (CS P4_3, CLK P4_0, DATA P4_2)...")
    await keep(c, held)
    await lcdtest(c, 35, 32, 34, held=held, ask_user=True)


async def hunt_chunked(c, held, rst_list, cs_only=None):
    hw = load_hw()
    pins = [p for p in safe_pins(hw) if not (held and p == held[0])]
    # P3_4..6 and P4_0..2 share GPIO lines: P4 side first (reference board), P3 side second
    passes = [[p for p in pins if p not in (28, 29, 30)], [p for p in pins if p not in (32, 33, 34)]]
    for rst in rst_list:
        if rst is not None:
            log(f"reset candidate {pn(rst)}")
        for cands in passes:
            cs_order = sorted(cands, key=lambda p: (p != 35, p))       # P4_3 first
            if cs_only is not None:
                cs_order = [p for p in cs_order if p in cs_only]
            for cs in cs_order:
                await keep(c, held)
                try:
                    r = await cmd(c, [0x15, 0xFF if rst is None else rst, 0, *mask_of(cands), cs], 15)
                except WatchGone:
                    r = None
                if not r or r[0] != 0x95:
                    if not await alive(c):
                        c = await mark_dead(c, cs, "used as CS in the hunt")
                        await keep(c, held)
                    continue
                hits = []
                for i in range(r[3]):
                    h = r[4 + 9 * i:13 + 9 * i]
                    hits.append({"cs": h[0], "clk": h[1], "mosi": h[2], "miso": h[3], "proto": h[4], "data": h[5:9].hex(), "rst": rst})
                print(f"\r   CS={pn(cs):5s} {len(hits)} answers   ", end="", flush=True)
                if hits:
                    print()
                    for x in hits:
                        log(f"DISPLAY ANSWERED: CS={pn(x['cs'])} CLK={pn(x['clk'])} DATA={pn(x['mosi'])} READ={pn(x['miso'])} RST={pn(rst) if rst is not None else '-'} {PROTO[x['proto']]} id={x['data']}")
                    save_hw(lcd_hits=hits, lcd=hits[0])
                    return c, hits[0]
        print()
        if rst is not None:
            await cmd(c, [0x0A, rst], 3)
    return c, None


async def p25_check(c):
    ask("P2_5 dropped the watch last time. Look at the SCREEN, press Enter: P2_5 goes HIGH for 2 s")
    try:
        await cmd(c, [0x08, 21, 1, 0xD0, 0x07], 5)
    except WatchGone:
        pass
    a = ask("Did the screen FLASH or glow, even briefly, before/while it dropped? (y/n)")
    save_hw(p2_5_flash=a.startswith("y"))
    if not await alive(c):
        c = await watch_back(c)
    return c


async def display(c, redo_backlight=False):
    hw = load_hw()
    if "backlight" in hw and not redo_backlight:
        log("backlight sweep already done (nothing glowed) - skipping it")
        held = (hw["backlight"]["pin"], hw["backlight"]["level"]) if hw.get("backlight") else None
        c = await p25_check(c)
    else:
        c, held = await backlight(c)
    await blind_try(c, held)
    if load_hw().get("lcd_working"):
        return c
    log("Asking every pin combination for a screen ID (one CS per step, ~1 s each)...")
    c, hit = await hunt_chunked(c, held, [None])
    if not hit:
        log("No answer without reset. Now each pin as the screen's RESET line, with CS on P4_3 (reference wiring)...")
        c, hit = await hunt_chunked(c, held, safe_pins(load_hw()), cs_only={35})
    if not hit:
        log("Still nothing. Full sweep: every RESET x every CS (long - Ctrl+C is safe, results are saved)...")
        c, hit = await hunt_chunked(c, held, safe_pins(load_hw()))
    if hit:
        await lcdtest(c, hit["cs"], hit["clk"], hit["mosi"], held=held, ask_user=True, rst=hit.get("rst"))
    else:
        log("No screen answered. Send this log to Claude.")
    return c


# ---------------------------------------------------------------- motor

async def motor(c):
    found = None
    k = len(MOTOR_SUSPECTS)
    for i, p in enumerate(MOTOR_SUSPECTS, 1):
        ask(f"[{i}/{k}] Hold the watch. Press Enter: {pn(p)} goes HIGH for 0.5 s, then LOW for 0.5 s")
        await cmd(c, [0x08, p, 1, 0xF4, 0x01], 4)
        await asyncio.sleep(0.3)
        await cmd(c, [0x08, p, 0, 0xF4, 0x01], 4)
        a = ask("Did it vibrate at all? (y/n)")
        if a.startswith("y"):
            found = p
            break
    await release(c)
    if found is None:
        log("none of them buzzed (on charger-only power the motor may also just lack current) - moving on")
    else:
        log("MOTOR =", pn(found))
    save_hw(motor=found)


# ---------------------------------------------------------------- i2c

async def i2c(c):
    hw = load_hw()
    skip = {hw.get("motor")} | set(MOTOR_SUSPECTS)
    down = await read_all(c, 2)
    up = await read_all(c, 1)
    if not down:
        log("pin read failed")
        return
    # an I2C line has a pull-up resistor on the board: it stays HIGH even when we pull it down
    cands = [p for p in VALID if down.get(p) == 1 and p not in skip]
    log("pins held high by the board (I2C suspects):", [pn(p) for p in cands])
    buses = []
    for scl in cands:
        for sda in cands:
            if scl == sda:
                continue
            r = await cmd(c, [0x12, scl, sda], 6)
            if not r or r[0] != 0x92:
                continue
            if r[1] == 0 and r[2] > 0:
                addrs = [a for a in range(0x08, 0x78) if r[3 + (a >> 3)] >> (a & 7) & 1]
                if len(addrs) > 20:
                    continue                      # every address 'answering' = not a real bus
                log(f"I2C BUS  SCL={pn(scl)} SDA={pn(sda)}:", ", ".join(f"0x{a:02X} {I2C_KNOWN.get(a, '?')}" for a in addrs))
                buses.append({"scl": scl, "sda": sda, "addrs": addrs})
    await release(c)
    if not buses:
        log("no I2C chips found (touch/sensors may be powered off or need a reset pin first)")
    for b in buses:
        for a in b["addrs"]:
            for reg in (0x0F, 0x00, 0xA7, 0xA8, 0x75, 0x01):
                r = await cmd(c, [0x13, b["scl"], b["sda"], a, reg, 1], 4)
                if r and r[0] == 0x93 and r[1] == 0:
                    log(f"   0x{a:02X} reg 0x{reg:02X} = 0x{r[3]:02X}")
    await release(c)
    save_hw(i2c=buses)


# ---------------------------------------------------------------- display hunt

PROTO = {0: "SPI RDDID 0x04", 1: "QSPI-AMOLED read 03-00-04-00", 2: "3-wire 9-bit LCD"}


def mask_of(pins):
    m = 0
    for p in pins:
        m |= 1 << p
    return list(m.to_bytes(5, "little"))


async def hunt(c, rst, cands, protos=0):
    t = time.time()
    r = await cmd(c, [0x15, 0xFF if rst is None else rst, protos << 4, *mask_of(cands)], 90)
    if not r or r[0] != 0x95:
        log("hunt: no reply", r)
        return []
    hits = []
    for i in range(r[3]):
        h = r[4 + 9 * i:13 + 9 * i]
        hits.append({"cs": h[0], "clk": h[1], "mosi": h[2], "miso": h[3], "proto": h[4], "data": h[5:9].hex()})
    log(f"hunt rst={pn(rst) if rst is not None else 'none'}: {r[2]} pins, {len(hits)} hits, {time.time() - t:.1f}s")
    return hits


async def lcdhunt(c):
    hw = load_hw()
    skip = set()
    if hw.get("motor") is not None:
        skip.add(hw["motor"])
    for b in hw.get("i2c", []):
        skip |= {b["scl"], b["sda"]}
    skip |= set(hw.get("charger_pins", []))
    skip |= set(hw.get("danger", []))
    skip |= {int(k) for k, v in (hw.get("pads") or {}).items() if v.get("boot_output")}
    base = [p for p in VALID if p not in skip]
    # P3_4..P3_6 and P4_0..P4_2 share GPIO lines inside the chip, so they are hunted in two passes.
    # Realtek's reference board puts the screen on P4_x, so that pass goes first.
    passes = [[p for p in base if p not in (28, 29, 30)], [p for p in base if p not in (32, 33, 34)]]
    log("skipping", [pn(p) for p in sorted(skip)] or "nothing")
    all_hits = []
    for i, cands in enumerate(passes, 1):
        await release(c)
        log(f"pass {i}/2: {len(cands)} pins, every CS/CLK/DATA combination (can take up to a minute)...")
        all_hits = await hunt(c, None, cands)
        if all_hits:
            break
    if not all_hits:
        log("no answer without a reset pulse. Trying each pin as the display RESET (up to ~1 min each, Ctrl+C is safe)...")
        for i, cands in enumerate(passes, 1):
            for n, rst in enumerate(cands, 1):
                await release(c)
                log(f"  pass {i}, reset candidate {n}/{len(cands)}: {pn(rst)}")
                h = await hunt(c, rst, cands)
                for x in h:
                    x["rst"] = rst
                all_hits += h
                if h:
                    break
            if all_hits:
                break
    await release(c)
    if not all_hits:
        log("NO DISPLAY ANSWER. Possible reasons: panel needs a power-enable pin, or it never answers reads. Send this log to Claude.")
        save_hw(lcd_hits=[])
        return
    # group identical wiring
    seen = {}
    for x in all_hits:
        k = (x["cs"], x["clk"], x["mosi"], x["miso"], x["proto"], x.get("rst"))
        seen.setdefault(k, x)
    for k, x in seen.items():
        log(f"DISPLAY? CS={pn(x['cs'])} CLK={pn(x['clk'])} MOSI={pn(x['mosi'])} MISO={pn(x['miso'])} "
            f"RST={pn(x['rst']) if x.get('rst') is not None else '-'}  {PROTO[x['proto']]}  answer={x['data']}")
    best = all_hits[0]
    log(f"next: bash run.sh tool5 lcdtest {best['cs']} {best['clk']} {best['mosi']}")
    save_hw(lcd_hits=list(seen.values()), lcd=best)


# ---------------------------------------------------------------- display test

async def spi(c, cs, clk, mosi, data, flags=0, miso=0xFF, nr=0, dc=0):
    return await cmd(c, [0x16, cs, clk, mosi, miso, dc, flags, len(data), *data, nr], 4)


async def fill(c, cs, clk, mosi, header, rgb565, count, flags=0):
    return await cmd(c, [0x17, cs, clk, mosi, 0, flags, len(header), *header, rgb565 >> 8, rgb565 & 0xFF,
                         *count.to_bytes(4, "little")], 30)


async def lcdtest(c, cs, clk, mosi, held=None, ask_user=True, rst="auto"):
    hw = load_hw()
    if rst == "auto":
        rst = (hw.get("lcd") or {}).get("rst")
    if held is None and hw.get("backlight"):
        held = (hw["backlight"]["pin"], hw["backlight"]["level"])
    await keep(c, held)
    if rst is not None:
        await cmd(c, [0x03, rst, 0]); await asyncio.sleep(0.02); await cmd(c, [0x03, rst, 1]); await asyncio.sleep(0.15)
    W_, H_ = 368, 448
    # 1) QSPI AMOLED drivers (SH8601 / CO5300 / ICNA3311 / RM67162) in 1-line mode: 02 00 CMD 00 params
    q = lambda cmd_, *p: [0x02, 0x00, cmd_, 0x00, *p]
    log("trying QSPI-AMOLED wake-up...")
    for f in (q(0x11),):
        await spi(c, cs, clk, mosi, f)
    await asyncio.sleep(0.15)
    for f in (q(0x3A, 0x55), q(0x53, 0x20), q(0x51, 0xFF), q(0x2A, 0, 0, (W_ - 1) >> 8, (W_ - 1) & 0xFF),
              q(0x2B, 0, 0, (H_ - 1) >> 8, (H_ - 1) & 0xFF), q(0x29)):
        await spi(c, cs, clk, mosi, f)
    for name, col in (("RED", 0xF800), ("GREEN", 0x07E0), ("BLUE", 0x001F)):
        await keep(c, held)
        await fill(c, cs, clk, mosi, [0x02, 0x00, 0x2C, 0x00], col, W_ * H_)
        log("painted", name, "- look at the screen")
        await asyncio.sleep(1.5)
    a = ask("Did the screen light up / change colour? (y/n)")
    if a.startswith("y"):
        save_hw(lcd_working={"cs": cs, "clk": clk, "mosi": mosi, "rst": rst, "mode": "qspi-1line"})
        return
    # 2) plain 3-wire 9-bit LCD (ST7789-like)
    log("trying 3-wire 9-bit LCD wake-up...")
    for f in ([0x11],):
        await spi(c, cs, clk, mosi, f, flags=1)
    await asyncio.sleep(0.15)
    for f in ([0x3A, 0x55], [0x2A, 0, 0, (W_ - 1) >> 8, (W_ - 1) & 0xFF], [0x2B, 0, 0, (H_ - 1) >> 8, (H_ - 1) & 0xFF], [0x29]):
        await spi(c, cs, clk, mosi, f, flags=1)
    await keep(c, held)
    await fill(c, cs, clk, mosi, [0x2C], 0xF800, W_ * H_, flags=1)
    a = ask("Red now? (y/n)")
    if a.startswith("y"):
        save_hw(lcd_working={"cs": cs, "clk": clk, "mosi": mosi, "rst": rst, "mode": "9bit"})
        return
    log("no picture on these pins.")


# ---------------------------------------------------------------- main

async def main():
    a = sys.argv[1:] or ["status"]
    c = await connect()
    try:
        if a[0] == "status":
            await status(c)
        elif a[0] == "release":
            await release(c)
        elif a[0] == "motor":
            await motor(c)
        elif a[0] == "findbattery":
            await findbattery(c)
        elif a[0] == "battery":
            await battery(c)
        elif a[0] == "i2c":
            await i2c(c)
        elif a[0] == "display":
            c = await display(c, redo_backlight="backlight" in a[1:])
        elif a[0] == "map":
            c = await map_pins(c)
        elif a[0] == "lcdhunt":
            await lcdhunt(c)
        elif a[0] == "lcdtest":
            await lcdtest(c, int(a[1]), int(a[2]), int(a[3]))
        elif a[0] == "auto":
            await status(c)
            c = await map_pins(c)
            await lcdhunt(c)
            hw = load_hw()
            if hw.get("lcd"):
                L = hw["lcd"]
                await lcdtest(c, L["cs"], L["clk"], L["mosi"])
            log("ALL DONE - tell Claude 'done'")
        else:
            print(__doc__)
    finally:
        try:
            if c.is_connected:
                await cmd(c, [0x0B], 2)
                await c.disconnect()
        except Exception:
            pass


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        print("\nstopped (Ctrl+C). The watch releases its pins by itself within 30 s.")
    except WatchGone:
        print(time.strftime("%H:%M:%S"), "WATCH DISCONNECTED (lost power?). Results so far are in logs/hw.json. Re-plug the charger and run the same command again.")
