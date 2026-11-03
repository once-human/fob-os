"""REAL FLASHER for BSW051 (Realtek RTL8762D silent OTA, single bank, one image per reboot).
Protocol ported from Realtek SDK (dfu_client.c host + dfu_service.c device) and Realsil Android lib.
Per image:  0x09 buffer-check-enable -> 0x06 target info -> 0x01 START (AES header) -> 0x02 offset
            -> data packets (AES per 16B block) in buffers + 0x0a buffer CRC -> 0x03 VALIDATE
            -> 0x04 ACTIVATE+RESET -> wait for watch to come back -> next image.
Usage: bash run.sh ota_flash --go IMG1 [IMG2 ...]     (MP .bin files, 512B MP header)
"""
import asyncio, sys, time, struct
from bleak import BleakScanner, BleakClient
from Crypto.Cipher import AES

ADDR = "00:E0:12:34:56:78"   # new address under our firmware (old: D7:B1:8B:17:BA:3E)
CP   = "00006487-3c17-d293-8e48-14fe2e4da212"
DATA = "00006387-3c17-d293-8e48-14fe2e4da212"
BAT  = "00002a19-0000-1000-8000-00805f9b34fb"
FFE0 = "0000ffe0-0000-1000-8000-00805f9b34fb"
KEY  = bytes.fromhex("4e46f8c5092b29e29a971a0cd1f610fb1f6763df807a7e70960d4cd3118e601a")
AESC = AES.new(KEY, AES.MODE_ECB)
PKT  = 240
NAMES = {0x2791: "SecureBoot", 0x2792: "RomPatch", 0x2793: "App", 0x279A: "UpperStack"}

def log(*a): print(time.strftime("%H:%M:%S"), *a, flush=True)
def enc(b):  # encrypt every full 16-byte block, tail stays plain (Realtek 16N mode)
    n = len(b) // 16 * 16
    return AESC.encrypt(b[:n]) + b[n:] if n else b
def crc(b):
    x = 0
    for i in range(0, len(b), 2): x ^= b[i] | (b[i+1] << 8)
    return ((x & 0xff) << 8) | (x >> 8)
def ver(v): return f"{v&0xf}.{(v>>4)&0xff}.{(v>>12)&0x7fff}.{(v>>27)&0x1f}"

class Flasher:
    def __init__(self): self.q = asyncio.Queue()
    def on_notify(self, _c, d): self.q.put_nowait(bytes(d))
    async def cp(self, c, payload, expect, timeout=8.0):
        while not self.q.empty(): self.q.get_nowait()
        await c.write_gatt_char(CP, payload, response=True)
        if expect is None: return None
        end = time.time() + timeout
        while time.time() < end:
            try: r = await asyncio.wait_for(self.q.get(), end - time.time())
            except asyncio.TimeoutError: break
            if len(r) >= 3 and r[0] == 0x10 and r[1] == expect: return r
        return None

    async def connect(self, timeout=10**9):
        import subprocess
        subprocess.run(["bluetoothctl", "power", "on"], capture_output=True, timeout=10)
        subprocess.run(["bluetoothctl", "disconnect", ADDR], capture_output=True, timeout=10)
        await asyncio.sleep(2)
        end = time.time() + timeout
        while time.time() < end:
            dev = None
            found = await BleakScanner.discover(timeout=8, return_adv=True)
            for a, (d, adv) in found.items():
                n = (adv.local_name or d.name or "")
                if a.upper() == ADDR or n.startswith("ONKAR") or n.startswith("BSW051"):
                    dev = d; log(f"  found {a} name={n!r}")
                    import subprocess as _sp
                    _sp.Popen(["paplay", "/usr/share/sounds/freedesktop/stereo/dialog-warning.oga"], stdout=_sp.DEVNULL, stderr=_sp.DEVNULL)
                    break
            if not dev:
                dev = ADDR   # may already be connected to this laptop (not advertising) - try direct
            if dev:
                c = BleakClient(dev, timeout=25)
                try:
                    await c.connect(); return c
                except Exception as e: log("connect failed:", e)
            log("watch NOT visible - unplug + replug the charger (I keep trying forever, Ctrl+C to quit)")
            import subprocess as _sp
            for _ in range(2):
                _sp.Popen(["paplay", "/usr/share/sounds/freedesktop/stereo/dialog-warning.oga"], stdout=_sp.DEVNULL, stderr=_sp.DEVNULL)
                time.sleep(0.35)
        return None

    async def send_image(self, c, img, size_incl_header):
        h = img[:12]; iid = struct.unpack_from("<H", h, 4)[0]; total = struct.unpack_from("<I", h, 8)[0] + 1024
        assert h[0] == 9 and len(img) >= total, "bad image"
        img = img[:total]
        r = await self.cp(c, bytes([0x09]), 0x09)
        if not r or r[2] != 1: log("buffer check enable failed:", r and r.hex()); return False
        maxbuf = struct.unpack_from("<H", r, 3)[0]; mtu = struct.unpack_from("<H", r, 5)[0]
        pkt = min(PKT, (mtu - 3) & 0xFFF0) if mtu > 20 else 16
        per_buf = max(1, (maxbuf - 12) // pkt)
        log(f"  buffer={maxbuf} mtu={mtu} packet={pkt} packets/buffer={per_buf}")
        r = await self.cp(c, bytes([0x06]) + iid.to_bytes(2, "little"), 0x06)
        log("  target info:", r and r.hex())
        r = await self.cp(c, bytes([0x01]) + enc(h + b"\0" * 4), 0x01)
        if not r or r[2] != 1: log("  START rejected:", r and r.hex()); return False
        await self.cp(c, bytes([0x02]) + iid.to_bytes(2, "little") + (12).to_bytes(4, "little"), None)
        off, first, t0 = 12, True, time.time()
        while off < total:
            buf = b""
            for _ in range(per_buf):
                if off + len(buf) >= total: break
                p = enc(img[off + len(buf): off + len(buf) + pkt])
                await c.write_gatt_char(DATA, p, response=False)
                buf += p
                await asyncio.sleep(0.008)
            size = len(buf) + (12 if (first and size_incl_header) else 0)
            r = await self.cp(c, bytes([0x0A]) + size.to_bytes(2, "little") + crc(buf).to_bytes(2, "little"), 0x0A, 10)
            if not r or r[2] != 1:
                log(f"  buffer at offset {off} failed:", r and r.hex()); return False
            off += len(buf); first = False
            if (off // len(buf)) % 10 == 0 or off >= total:
                log(f"  {NAMES.get(iid, hex(iid))}: {off}/{total} bytes ({100*off//total}%) {time.time()-t0:.0f}s")
        r = await self.cp(c, bytes([0x03]) + iid.to_bytes(2, "little"), 0x03, 20)
        log("  VALIDATE:", r and r.hex())
        return bool(r and r[2] == 1)

    async def flash_one(self, path):
        img = open(path, "rb").read()[0x200:]
        iid = struct.unpack_from("<H", img, 4)[0]
        log(f"=== {NAMES.get(iid, hex(iid))} from {path.split('/')[-1]} ({len(img)} bytes)")
        for attempt, incl in enumerate((False, True, False)):   # watch wants size WITHOUT the 12B header (verified)
            c = await self.connect()
            if not c: log("watch not found - stopping"); return False
            try:
                b = bytes(await c.read_gatt_char(BAT))[0]
                log(f"  battery raw={b} ({'charging '+str(b-100)+'%' if b > 100 else str(b)+'%'})")
                await c.start_notify(CP, self.on_notify); await asyncio.sleep(0.5)
                ok = await self.send_image(c, img, incl)
                if ok:
                    log("  ACTIVATE + RESET (0x04) - watch reboots now. KEEP IT ON THE CHARGER.")
                    try: await self.cp(c, bytes([0x04]), None)
                    except Exception as e: log("  (disconnect during activate is normal:", e, ")")
                    return True
                log(f"  attempt {attempt+1} failed, nothing activated. Retrying.")
            finally:
                try: await c.disconnect()
                except Exception: pass
            await asyncio.sleep(5)
        return False

    async def after_reboot(self):
        log("waiting for the watch to come back...")
        await asyncio.sleep(15)
        c = await self.connect(timeout=120)
        if not c: log("!!! watch did not come back within ~2 min"); return False
        try:
            name = c.address
            v = bytes(await c.read_gatt_char(FFE0))
            vs = [struct.unpack_from("<I", v, i)[0] for i in range(0, len(v) - 3, 4)]
            log("  ALIVE. versions now:", [ver(x) for x in vs])
        except Exception as e:
            log("  ALIVE (could not read versions:", e, ")")
        finally:
            await c.disconnect()
        return True

async def main():
    args = sys.argv[1:]
    if not args or args[0] != "--go":
        print("refusing to run without --go"); return
    f = Flasher()
    for i, path in enumerate(args[1:]):
        if not await f.flash_one(path):
            log("STOPPED: image failed before activation. Watch still runs its previous firmware."); return
        alive = await f.after_reboot()
        if not alive:
            log("STOPPED after", path.split('/')[-1], "- watch is not advertising."); return
    log("ALL DONE")

asyncio.run(main())
