"""tools/omkdebug.py session: its steps against a simulated camera - the debug service's answers, a restart by core or by
hand, the cable taken out for the scenario and put back (or a hub's port switched off and on), a scenario without hands
(the cable in as charge only, a restart at its end), time limits."""
import binascii
import struct
import sys
import tempfile
import unittest
from pathlib import Path

OMKIT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(OMKIT / "tools"))
import omkdebug  # noqa: E402

CORE_BIN = "OMKIT\\CORE.BIN"
SHOOT_RUN = "OMKIT\\SHOOT.RUN"
SHOOT_END = "OMKIT\\SHOOT.END"


def header(boot, tail, head, seq, run=b"RUN00001", flags=0, size=65536):
    r0, r1 = struct.unpack("<II", run.ljust(8, b"\0"))
    w = [omkdebug.MAGIC, 1, boot, size, tail, head, seq, 0, r0, r1, 1234, flags, 0, 0, 0, 0]
    w[14] = binascii.crc32(struct.pack("<6I", *w[0:4], r0, r1))
    return struct.pack("<16I", *w)


def record(typ, seq, ms, payload):
    b = struct.pack("<BBHII", 0xA5, typ, len(payload), seq, ms) + payload
    return b + bytes(-len(b) % 4)


class World:
    """One camera: its card, its run, its USB link and its scenario, on a simulated clock"""

    def __init__(self, restart=True):
        self.now = 0.0
        self.files = {"OMKIT\\OMKIT.INI": b"[debug]\r\nenabled = on\r\nwrite = on\r\n",
                      "DCIM\\100OMSYS\\P1010001.JPG": b"jpeg1", "DCIM\\100OMSYS\\P1010002.JPG": b"jpeg2"}
        self.runs = 1
        self.run = "RUN00001"
        self.online = True                  # in the MTP mode, answering
        self.cable = True
        self.events = []                    # [when, what]
        self.restart_ok = restart
        self.build = "feature/x@1111111"
        self.shoot = {"state": 0, "waiting": 0, "total": 0, "taken": 0}
        self.shoot_ends = None
        self.job = [0, 0, 0, 0, 0]
        self.said = []
        self.hands = None                   # a scenario without hands: what this camera does with it
        self.hands_result = "done"          # ... its result; "missed" - USB not held: it comes up in MTP, armed
        self.end_restart = True             # ... core's restart at its end happens
        self.prev_run = None
        self.mute = False                   # on USB, but core does not run (came on from charging): HELLO fails

    def at(self, dt, what):
        self.events.append([self.now + dt, what])

    def sleep(self, dt):
        self.now += dt
        for e in sorted(self.events, key=lambda e: e[0]):
            if e[0] <= self.now:
                self.events.remove(e)
                e[1]()
        if self.shoot["state"] == 1 and self.shoot["waiting"] and not self.cable:
            self.shoot["waiting"] = 0       # the camera shoots once the cable is out
            self.shoot_ends = self.now + 10 * self.shoot["total"]
        if self.shoot_ends is not None and self.now >= self.shoot_ends:
            self.shoot.update(state=2, taken=self.shoot["total"])
            self.shoot_ends = None

    def boot(self):
        """A real start: the new CORE.BIN's build runs, SHOOT.RUN is taken (removed) and its scenario armed. Without
        hands: the cable stays in but the camera keeps off the computer (charge only), shoots, writes SHOOT.END and
        restarts"""
        self.prev_run = self.run
        self.runs += 1
        self.run = f"RUN{self.runs:05d}"
        self.build = omkdebug.build_of(self.files.get(CORE_BIN, b"")) or self.build
        self.shoot = {"state": 0, "waiting": 0, "total": 0, "taken": 0}
        once = self.files.pop(SHOOT_RUN, None)
        self.online = self.cable
        if once:
            steps = [x for x in once.split(b"steps = ")[1].split(b"\r")[0].split(b",") if x.strip()]
            self.shoot.update(state=1, waiting=1, total=len(steps))
            if b"hands_free = on" in once and self.hands_result != "missed":
                sid = once.split(b"session = ")[1].split(b"\r")[0].decode()
                self.online = False         # charge only: not on the bus
                self.shoot["waiting"] = 0
                self.at(10 * len(steps) + 5, lambda: self.hands_end(sid, len(steps)))

    def hands_end(self, sid, n):
        ok = self.hands_result == "done"
        self.shoot.update(state=2 if ok else 3, taken=n if ok else 0)
        text = (f"session={sid}\r\nrun={self.run}\r\nbuild={self.build}\r\nresult={self.hands_result}\r\n"
                f"why={'' if ok else 'the camera did not come to shooting in time'}\r\ntaken={self.shoot['taken']}\r\n"
                f"total={n}\r\nfailed=0\r\n")
        self.files[SHOOT_END] = text.encode().ljust(254) + b"\r\n"
        if self.end_restart:
            self.at(8, self.boot)

    def unplug(self):
        self.cable = self.online = False

    def plug(self):
        self.cable = self.online = True


class FakeCam:
    name = "OM-5MarkII (simulated)"

    def __init__(self, w):
        if not w.online:
            raise LookupError("no device")
        self.w, self.path, self.text, self.wbuf, self.want, self.xfer, self.bank = w, "", b"", b"", 0, b"", b""
        self.open = True

    def close(self):
        self.open = False

    def _alive(self):
        if not self.open or not self.w.online:
            raise omkdebug.Refused("WPD error 0x8007001f")

    def _job(self, kind, result=0, nbytes=0, crc=0, extra=0):
        self.w.job = [kind | (4 if result else 3) << 8, result, nbytes, crc, extra]
        return 0x2001, [1]

    def cmd(self, *p):
        self._alive()
        w, sub = self.w, p[0]
        flags = (1 | 4 | 16 | 32 | omkdebug.F_SHOOT | omkdebug.F_INFO | omkdebug.F_HANDS_FREE |
                 (omkdebug.F_RESTART if w.restart_ok else 0))
        if sub == omkdebug.HELLO:
            if w.mute:
                raise omkdebug.Refused("WPD error 0x8007001f")
            return 0x2001, [omkdebug.MAGIC, 1, 1 << 16 | 42, flags, 1]
        if sub == omkdebug.STATUS:
            return 0x2001, list(w.job)
        if sub == omkdebug.WBEGIN:
            self.wbuf, self.want = b"", p[2]
            return self._job(sub)
        if sub == omkdebug.WCOMMIT:
            good = binascii.crc32(self.wbuf) == self.want
            if good:
                w.files[self.path] = self.wbuf
            return self._job(sub, 0 if good else 5, len(self.wbuf), binascii.crc32(self.wbuf))
        if sub == omkdebug.READ:
            data = w.files.get(self.path)
            if data is None:
                return self._job(sub, 2)
            self.xfer = data[p[1]:p[1] + p[2]]
            return self._job(sub, 0, len(self.xfer), binascii.crc32(self.xfer), p[1] + len(self.xfer))
        if sub == omkdebug.LIST:
            ents = sorted({k[len(self.path) + 1:].split("\\")[0] for k in w.files if k.startswith(self.path + "\\")})
            ents = [e for e in ents if "." in e]            # the firmware's findfirst "*.*": names with a dot (camera)
            self.xfer = b"".join(struct.pack("<4I", 0x20 if "." in e else 0x10, 1, 0, 0) + e.encode().ljust(48, b"\0")
                                 for e in ents)
            return self._job(sub, 0, len(self.xfer), 0, len(ents))
        if sub == omkdebug.RESTART:
            if not w.restart_ok:
                return self._job(sub, 10)
            w.at(0.5, lambda: setattr(w, "online", False))
            w.at(12, w.boot)
            return self._job(sub, 0, 0, 0, 500)
        if sub == omkdebug.SHOOT:
            if p[1] == omkdebug.SHOOT_STATUS:
                s = w.shoot
                return 0x2001, [s["state"] | s["waiting"] << 8, s["total"], s["taken"], 0, 0]
            if p[1] == omkdebug.SHOOT_START:
                n = len([x for x in self.text.split(b",") if x.strip()])
                w.shoot = {"state": 1, "waiting": 1, "total": n, "taken": 0}
            return self._job(sub)
        return 0x2005, []

    def put(self, data, *p):
        self._alive()
        if p[0] == omkdebug.PATH:
            self.path = data.decode()
        elif p[0] == omkdebug.TEXT:
            self.text = data
        elif p[0] == omkdebug.WDATA:
            self.wbuf += data
            self.w.job = [omkdebug.WDATA | 3 << 8, 0, len(self.wbuf), 0, 0]
        return 0x2001, []

    def get(self, *p):
        self._alive()
        w = self.w
        if p[0] == omkdebug.INFO:
            st = omkdebug.SHOOT_STATES[w.shoot["state"]]
            return 0x2001, [], f"core=1.42\nbuild={w.build}\nrun={w.run}\nshoot={st}\n".encode()
        if p[0] == omkdebug.XFER:
            return 0x2001, [], self.xfer[p[1]:p[1] + p[2]]
        if p[0] == omkdebug.BANKS:
            self.bank = record(1, 1, 5, b"\x03       5 I OMKit core 1.42 (" + w.build.encode() + b")")
            self.prev = record(1, 1, 7, b"\x03       7 I shoot: done")
            prev = (header(1, 0, len(self.prev), 2, run=w.prev_run.encode(), flags=1) if w.prev_run else
                    header(0, 0, 0, 0))
            return 0x2001, [], (header(2, 0, len(self.bank), 2, run=w.run.encode()) + prev +
                                struct.pack("<4I", 0, 0x20080, 0, 1 if w.prev_run else 0xFF))
        if p[0] == omkdebug.BANK:
            data = self.bank if p[1] == 0 else self.prev
            return 0x2001, [0, 0, len(data)], data[p[2]:p[2] + p[3]]
        return 0x2005, [], b""


class Args:
    def __init__(self, **kw):
        self.put, self.scenario, self.shots, self.start_delay, self.restart, self.wait = [], None, None, None, False, False
        self.pull_log, self.pull_pictures, self.out = False, 0, "."
        self.connect_timeout, self.restart_timeout, self.scenario_timeout = 60.0, 120.0, 600.0
        self.usb_off = self.usb_on = self.offline = None
        self.hands_free = False
        self.__dict__.update(kw)


class SessionSteps(unittest.TestCase):
    def make(self, w, user=True, shell=None, on_say=None):
        def say(m):
            w.said.append(m)
            if user and "take the USB cable out" in m:      # the owner: out now, back in a minute
                w.at(3, w.unplug)
                w.at(60, w.plug)
            if on_say:
                on_say(m)
        return omkdebug.Session(lambda: FakeCam(w), say, clock=lambda: w.now, sleep=w.sleep, shell=shell)

    def hands(self, w, **kw):
        """A session without hands: nobody touches the cable"""
        a = Args(scenario="look:1, stock", hands_free=True, restart=True, wait=True, **kw)
        ses = self.make(w, user=False)
        return ses.run(a), ses

    def test_hands_free(self):
        w = World()
        with tempfile.TemporaryDirectory() as d:
            core = Path(d) / "CORE.BIN"
            core.write_bytes(b"\nbuild=feature/usb-handsfree@4444444\n")
            out = Path(d) / "out"
            w.files["OMKIT\\LOG\\CORE.LOG"] = b"log"
            rc, ses = self.hands(w, put=[str(core)], pull_log=True, pull_pictures=1, out=str(out))
            self.assertEqual(rc, 0, w.said)
            self.assertEqual(w.run, "RUN00003")             # two restarts: into the scenario and after it
            self.assertFalse(any("take the USB cable out" in m for m in w.said))
            self.assertIn(f"session={ses.session_id}", w.files[SHOOT_END].decode())
            self.assertTrue(any("scenario: done: 2 of 2 pictures made" in m and "run RUN00002" in m for m in w.said))
            self.assertIn("run RUN00002", (out / "ramlog_prev.txt").read_text())
            self.assertIn("run RUN00003", (out / "ramlog.txt").read_text())
        self.assertIn("hands-free run", w.said[-1])

    def test_hands_free_failed_scenario_still_pulls_the_log(self):
        w = World()
        w.hands_result = "failed"
        w.files["OMKIT\\LOG\\CORE.LOG"] = b"log"
        with tempfile.TemporaryDirectory() as d:
            rc, _ = self.hands(w, pull_log=True, out=d)
            self.assertTrue((Path(d) / "ramlog_prev.txt").exists())
        self.assertEqual(rc, 1)
        self.assertIn("FAILED at 'hands-free run'", w.said[-1])
        self.assertIn("the camera did not come to shooting in time", w.said[-1])

    def test_hands_free_camera_not_back(self):
        w = World()
        w.end_restart = False                               # hangs, or stays in charge only
        rc, _ = self.hands(w, restart_timeout=30, scenario_timeout=60)
        self.assertEqual(rc, 1)
        self.assertIn("no camera in 90 s", w.said[-1])
        self.assertIn("take the battery out", w.said[-1])

    def test_hands_free_usb_not_held(self):
        w = World()
        w.hands_result = "missed"                           # comes up in MTP with the scenario armed
        rc, _ = self.hands(w)
        self.assertEqual(rc, 1)
        self.assertIn("USB was not held as charge only", w.said[-1])

    def test_hands_free_stale_result(self):
        w = World()
        w.files[SHOOT_END] = b"session=0BADF00D\r\nresult=done\r\n"
        w.end_restart = True
        orig = w.hands_end
        w.hands_end = lambda sid, n: (orig("FFFFFFFF", n))  # an older core: writes nothing of this session
        rc, _ = self.hands(w)
        self.assertEqual(rc, 1)
        self.assertIn("no result of this session", w.said[-1])

    def test_camera_on_usb_without_core(self):
        w = World()
        w.mute = True                                       # came on from charging by the cable: no core

        def on_say(m):
            if "OMKit does not answer" in m:                # the owner: the lever off and on - a full start
                w.at(5, lambda: setattr(w, "mute", False))
        rc = self.make(w, user=False, on_say=on_say).run(Args(pull_log=False, pull_pictures=0, restart=True))
        self.assertEqual(rc, 0, w.said)
        told = [m for m in w.said if "OMKit does not answer" in m]
        self.assertEqual(len(told), 1)
        self.assertIn("switch it off with the lever and on again", told[0])
        self.assertFalse(any("connect: no camera yet" in m for m in w.said))

    def test_camera_on_usb_without_core_for_good(self):
        w = World()
        w.mute = True
        rc = self.make(w, user=False).run(Args(restart=True, connect_timeout=40))
        self.assertEqual(rc, 1)
        self.assertIn("no camera in 40 s (one was on USB, but OMKit did not answer", w.said[-1])

    def test_hands_free_needs_a_core_that_can(self):
        w = World()
        orig = FakeCam.cmd

        def cmd(self, *p):
            code, r = orig(self, *p)
            if p[0] == omkdebug.HELLO:
                r[3] &= ~omkdebug.F_HANDS_FREE
            return code, r
        FakeCam.cmd = cmd
        try:
            rc, _ = self.hands(w)
        finally:
            FakeCam.cmd = orig
        self.assertEqual(rc, 1)
        self.assertIn("cannot run a scenario without hands", w.said[-1])

    def test_full_cycle(self):
        w = World()
        with tempfile.TemporaryDirectory() as d:
            core = Path(d) / "CORE.BIN"
            core.write_bytes(b"\0" * 100 + b"core=1.42\nbuild=feature/usb-session@2222222\nrun=" + b"\0" * 50)
            out = Path(d) / "out"
            a = Args(put=[str(core)], scenario="look:Kodak Portra 400, stock", restart=True, wait=True, pull_log=True,
                     pull_pictures=1, out=str(out))
            w.files["OMKIT\\LOG\\CORE.LOG"] = b"the card's log"
            rc = self.make(w).run(a)
            self.assertEqual(rc, 0, w.said)
            self.assertEqual((w.build, w.run), ("feature/usb-session@2222222", "RUN00002"))
            self.assertNotIn(SHOOT_RUN, w.files)           # taken by the start
            self.assertEqual((w.shoot["state"], w.shoot["taken"]), (2, 2))
            self.assertTrue(any("take the USB cable out" in m for m in w.said))
            self.assertIn("OMKit core 1.42 (feature/usb-session@2222222)", (out / "ramlog.txt").read_text())
            self.assertEqual((out / "CORE.LOG").read_bytes(), b"the card's log")
            self.assertEqual(sorted(x.name for x in out.glob("*.JPG")), ["P1010002.JPG"])

    def test_restart_by_hand_and_wrong_build(self):
        w = World(restart=False)

        def by_hand():                                      # the owner: battery out and in - but another card
            w.online = False
            w.files[CORE_BIN] = b"\nbuild=old@0000000\n"
            w.at(20, w.boot)

        def on_say(m):
            if "cannot restart" in m:
                w.at(5, by_hand)
        with tempfile.TemporaryDirectory() as d:
            core = Path(d) / "CORE.BIN"
            core.write_bytes(b"\nbuild=feature/new@3333333\n")
            rc = self.make(w, on_say=on_say).run(Args(put=[str(core)], restart=True))
        self.assertEqual(rc, 1)
        self.assertIn("the CORE.BIN put is feature/new@3333333", w.said[-1])
        self.assertEqual(w.run, "RUN00002")

    def test_scenario_over_usb_with_a_hub(self):
        w = World()
        calls = []

        def shell(cmd):
            calls.append(cmd)
            (w.unplug if cmd == "off" else w.plug)()
            return 0
        rc = self.make(w, user=False, shell=shell).run(Args(scenario="keep, keep", wait=True, usb_off="off", usb_on="on",
                                                            offline=30))
        self.assertEqual(rc, 0, w.said)
        self.assertEqual(calls, ["off", "on"])
        self.assertEqual((w.shoot["state"], w.shoot["taken"], w.run), (2, 2, "RUN00001"))

    def test_scenario_lost_by_a_restart(self):
        w = World()

        def on_say(m):
            if "take the USB cable out" in m:
                w.at(2, w.unplug)
                w.at(4, w.boot)                             # the camera started again meanwhile
                w.at(8, w.plug)
        rc = self.make(w, user=False, on_say=on_say).run(Args(scenario="keep", wait=True))
        self.assertEqual(rc, 1)
        self.assertIn("it restarted, the scenario is lost", w.said[-1])

    def test_restart_not_taken(self):
        w = World()
        orig = FakeCam.cmd

        def cmd(self, *p):
            if p[0] == omkdebug.RESTART:                    # accepted, but the camera stays (not in the PC mode)
                return self._job(p[0], 0, 0, 0, 500)
            return orig(self, *p)
        FakeCam.cmd = cmd
        try:
            rc = self.make(w).run(Args(restart=True))
        finally:
            FakeCam.cmd = orig
        self.assertEqual(rc, 1)
        self.assertTrue(any("the request was not taken" in m for m in w.said))
        self.assertEqual(w.run, "RUN00001")

    def test_no_pictures_fails(self):
        w = World()
        del w.files["DCIM\\100OMSYS\\P1010001.JPG"], w.files["DCIM\\100OMSYS\\P1010002.JPG"]
        with tempfile.TemporaryDirectory() as d:
            rc = self.make(w).run(Args(pull_pictures=1, out=d))
        self.assertEqual(rc, 1)
        self.assertIn("pictures: 0 found in DCIM, 1 asked for", w.said[-1])

    def test_pictures_in_two_folders(self):
        w = World()
        w.files["DCIM\\101OMSYS\\P1020003.JPG"] = b"j3"
        w.files["DCIM\\101OMSYS\\P1020003.ORF"] = b"r3"
        with tempfile.TemporaryDirectory() as d:
            rc = self.make(w).run(Args(pull_pictures=2, out=d))
            got = sorted(x.name for x in Path(d).iterdir())
        self.assertEqual(rc, 0, w.said)
        self.assertEqual(got, ["P1010002.JPG", "P1020003.JPG", "P1020003.ORF"])

    def test_camera_gone_for_good(self):
        w = World()
        w.online = False
        rc = self.make(w).run(Args(restart=True, connect_timeout=20))
        self.assertEqual(rc, 1)
        self.assertIn("no camera in 20 s", w.said[-1])

    def test_no_write_key(self):
        w = World()
        orig = FakeCam.cmd

        def cmd(self, *p):
            code, r = orig(self, *p)
            if p[0] == omkdebug.HELLO:
                r[3] &= ~4
            return code, r
        FakeCam.cmd = cmd
        try:
            rc = self.make(w).run(Args(scenario="keep", restart=True))
        finally:
            FakeCam.cmd = orig
        self.assertEqual(rc, 1)
        self.assertIn("write = on is needed", w.said[-1])

    def test_texts(self):
        t = omkdebug.shoot_run_text("look:1, stock", 2, 0).decode()
        self.assertIn("[shoot]\r\nsteps = look:1, stock\r\nshots = 2\r\nstart_delay = 0\r\n", t)
        self.assertNotIn("hands_free", t)
        t = omkdebug.shoot_run_text("keep", None, None, "12AB34CD").decode()
        self.assertIn("hands_free = on\r\nsession = 12AB34CD\r\narm_wait = 120000\r\n", t)
        r = omkdebug.parse_result(b"session=12AB34CD\r\nresult=failed\r\nwhy=stuck\r\ntaken=1\r\n".ljust(254) + b"\r\n")
        self.assertEqual((r["session"], r["result"], r["taken"]), ("12AB34CD", "failed", "1"))
        self.assertIn("failed: 1 of ? pictures made", omkdebug.result_text(r))
        self.assertEqual(omkdebug.default_target("x/filmlook.omm")[1], "OMKIT\\MODULES\\FILMLOOK.OMM")
        self.assertEqual(omkdebug.default_target("a.ini=omkit/ini/a.ini")[1], "OMKIT\\INI\\A.INI")
        self.assertEqual(omkdebug.parse_info("core=1.42\nbuild=b@c\n")["build"], "b@c")
        self.assertEqual(omkdebug.build_of(b"xx\nbuild=main@abcdef0\nrun="), "main@abcdef0")


if __name__ == "__main__":
    unittest.main()
