#!/usr/bin/env python3
"""
Reads the wait profiler's own buffer out of a run's log (boot-arg waitprof=NAME[,NAME]; the section
"--- wait profile ---" that arch/arm64/kernel/loader_return.c appends) and says what the threads waited on:
the "wpw" lines grouped by thread and by where the wait was made, the addresses named.

    tools/waitprof.py LOG [--cache DIR] [--kernel FILE] [--min MS] [--max MS] [--from S] [--to S] [--top N]
                          [--thread NAME] [--raw]

  --cache   the directory of dyld_shared_cache_arm64e and its .symbols file (user addresses)
  --kernel  the unstripped kernel of the run (kernel addresses; llvm-nm must be on the PATH)
  --min/--max  only waits of this many ms (default 100 .. 3000: longer ones are threads with nothing to do)

A wpw line: wpw END TID 'NAME' MS B|R PROCESS pPRIORITY eEC tTRAP [mMSGID wWORD] hHINT evEVENT [toTIMEOUT] [rRCV utUTMO]
u:PC,LR,FRAMES [cCONT] [k:KERNEL FRAMES] [dPID PEER HOW SVC]; an R line has sSTATE bBUCKET rqCPU bdCPU lpCPU
(run queue's, bound and last processor) iaMASK ioMASK (processors idle throughout / at some time) recMASK pcPC xX16; "wps" lines are a thread's run% and runnable% of
a period, "wpc" the slides.
"""
import argparse
import bisect
import collections
import glob
import os
import re
import struct
import subprocess
import sys

CACHE_BASE = 0x180000000


class Cache:
    """dyld shared cache: which image an address is in, and the nearest symbol below it."""

    def __init__(self, directory):
        main = os.path.join(directory, "dyld_shared_cache_arm64e")
        self.files = []         # (address, size, file offset, path)
        for path in sorted(glob.glob(main + "*")):
            if path.endswith(".symbols") or path.endswith(".map"):
                continue
            with open(path, "rb") as f:
                head = f.read(0x200)
                if not head.startswith(b"dyld_v1"):
                    continue
                off, count = struct.unpack_from("<II", head, 0x10)
                f.seek(off)
                for _ in range(count):
                    address, size, fileoff, _, _ = struct.unpack("<QQQII", f.read(32))
                    self.files.append((address, size, fileoff, path))
        self.files.sort()
        self.handles = {}
        with open(main, "rb") as f:
            head = f.read(0x200)
            text_off, text_count = struct.unpack_from("<QQ", head, 0x88)
            f.seek(text_off)
            raw = f.read(32 * text_count)
            self.images = []    # (address, text size, path)
            for i in range(text_count):
                address, size, path_off = struct.unpack_from("<QII", raw, 32 * i + 16)
                f.seek(path_off)
                name = f.read(256).split(b"\0")[0].decode()
                self.images.append((address, size, name))
        self.images.sort()
        self.starts = [i[0] for i in self.images]
        self.symbols = {}       # image address -> sorted [(address, name)]
        self.locals = {}
        sym = main + ".symbols"
        if os.path.exists(sym):
            self.symfile = open(sym, "rb")
            head = self.symfile.read(0x200)
            self.local_off = struct.unpack_from("<Q", head, 0x48)[0]
            self.symfile.seek(self.local_off)
            (self.nlist_off, _, self.str_off, _, entries_off, entries_count) = struct.unpack("<6I", self.symfile.read(24))
            self.symfile.seek(self.local_off + entries_off)
            raw = self.symfile.read(16 * entries_count)
            for i in range(entries_count):
                dylib, start, count = struct.unpack_from("<QII", raw, 16 * i)
                self.locals[CACHE_BASE + dylib] = (start, count)
        else:
            self.symfile = None

    def read(self, address, size):
        for base, length, fileoff, path in self.files:
            if base <= address < base + length:
                f = self.handles.get(path)
                if f is None:
                    f = self.handles[path] = open(path, "rb")
                f.seek(fileoff + address - base)
                return f.read(size)
        return b""

    def image_of(self, address):
        i = bisect.bisect_right(self.starts, address) - 1
        if i >= 0 and address < self.images[i][0] + self.images[i][1]:
            return self.images[i]
        return None

    def _load(self, image):
        base = image[0]
        out = []
        head = self.read(base, 32)
        if len(head) == 32:
            ncmds, sizeofcmds = struct.unpack_from("<II", head, 16)
            cmds = self.read(base + 32, sizeofcmds)
            at = 0
            linkedit = symtab = None
            for _ in range(ncmds):
                cmd, size = struct.unpack_from("<II", cmds, at)
                if cmd == 0x19 and cmds[at + 8:at + 24].rstrip(b"\0") == b"__LINKEDIT":
                    linkedit = struct.unpack_from("<QQQ", cmds, at + 24)      # vmaddr, vmsize, fileoff
                elif cmd == 0x2:
                    symtab = struct.unpack_from("<IIII", cmds, at + 8)        # symoff, nsyms, stroff, strsize
                at += size
            if linkedit and symtab:
                def va(fileoff):
                    return linkedit[0] + fileoff - linkedit[2]
                nl = self.read(va(symtab[0]), 16 * symtab[1])
                strings = self.read(va(symtab[2]), symtab[3])
                for i in range(len(nl) // 16):
                    strx, kind, _, _, value = struct.unpack_from("<IBBHQ", nl, 16 * i)
                    if kind & 0xe0 or (kind & 0x0e) != 0x0e or value == 0:
                        continue
                    end = strings.find(b"\0", strx)
                    out.append((value, strings[strx:end].decode(errors="replace")))
        if self.symfile is not None and base in self.locals:
            start, count = self.locals[base]
            self.symfile.seek(self.local_off + self.nlist_off + 16 * start)
            nl = self.symfile.read(16 * count)
            for i in range(count):
                strx, kind, _, _, value = struct.unpack_from("<IBBHQ", nl, 16 * i)
                if kind & 0xe0 or (kind & 0x0e) != 0x0e or value == 0:
                    continue
                self.symfile.seek(self.local_off + self.str_off + strx)
                out.append((value, self.symfile.read(200).split(b"\0")[0].decode(errors="replace")))
        out.sort()
        return out

    def name(self, address):
        """address: unslid. -> 'Image`symbol+off' or None"""
        image = self.image_of(address)
        if image is None:
            return None
        syms = self.symbols.get(image[0])
        if syms is None:
            syms = self.symbols[image[0]] = self._load(image)
        short = os.path.basename(image[2])
        i = bisect.bisect_right(syms, (address, "\U0010ffff")) - 1
        if i < 0:
            return "%s+%#x" % (short, address - image[0])
        return "%s`%s+%d" % (short, syms[i][1][:70], address - syms[i][0])


class Kernel:
    def __init__(self, path):
        out = subprocess.run(["llvm-nm", "-n", path], capture_output=True, text=True).stdout
        self.syms = []
        for line in out.splitlines():
            p = line.split()
            if len(p) == 3 and p[1] in "tTsS":
                self.syms.append((int(p[0], 16), p[2]))
        self.syms.sort()

    def name(self, address):
        i = bisect.bisect_right(self.syms, (address, "\U0010ffff")) - 1
        if i < 0 or address - self.syms[i][0] > 0x20000:
            return "%#x" % address
        return "%s+%d" % (self.syms[i][1], address - self.syms[i][0])


TRAPS = {-31: "mach_msg", -47: "mach_msg2", -36: "sem_wait", -38: "sem_timedwait", -90: "mach_wait_until", 93: "select",
         363: "kevent", 374: "kevent_qos", 375: "kevent_id", 515: "ulock_wait", 544: "ulock_wait2", 301: "psynch_mutexwait",
         305: "psynch_cvwait", 334: "nanosleep", 3: "read", 4: "write", 54: "ioctl", 5: "open", 197: "mmap", 368: "workq"}
# the frames every wait has in common: left out of a wait's signature
BORING = ("mach_msg", "__psynch", "_pthread_cond", "__ulock", "_dispatch_", "__CFRunLoop", "CFRunLoopRun", "_os_unfair",
          "kevent", "__workq", "_pthread_wqthread", "start_wqthread", "__semwait", "nanosleep", "usleep", "_dlock_wait",
          "__DISPATCH_WAIT", "_pthread_mutex", "pthread_mutex", "semaphore_", "_xpc_", "xpc_", "__select", "-[NSCondition",
          "std::__1::condition_variable", "std::__1::mutex", "thread_start", "_pthread_start", "__NSThread", "-[NSRunLoop")


def parse(line):
    m = re.match(r"wpw (\d+)\.(\d+) (\d+) '([^']*)' (\d+) ([BR])(.*)", line)
    if not m:
        return None
    w = {"end": int(m.group(1)) + int(m.group(2)) / 1000.0, "tid": int(m.group(3)), "name": m.group(4), "ms": int(m.group(5)),
         "kind": m.group(6), "u": [], "k": [], "trap": None, "msgid": None, "word": None, "to": None, "dst": None, "utmo": None,
         "ec": None, "ev": None, "proc": "", "pri": None}
    w["rest"] = m.group(7) + " "
    rest = m.group(7).split()
    if len(rest) >= 2 and re.fullmatch(r"p-?\d+", rest[1]):       # (the process and the priority: kernels from 2026-11-28)
        w["proc"], w["pri"] = rest[0], int(rest[1][1:])
        rest = rest[2:]
    for i, tok in enumerate(rest):
        if tok.startswith("u:"):
            w["u"] = [int(x, 16) for x in tok[2:].split(",") if x]
        elif tok.startswith("k:"):
            w["k"] = [int(x, 16) for x in tok[2:].split(",") if x]
        elif tok.startswith("ev"):
            w["ev"] = int(tok[2:], 16)
        elif tok.startswith("ut"):
            w["utmo"] = int(tok[2:])
        elif tok.startswith("to"):
            w["to"] = int(tok[2:])
        elif tok[0] == "t" and re.fullmatch(r"t-?\d+", tok):
            w["trap"] = int(tok[1:])
        elif tok[0] == "e" and re.fullmatch(r"e[0-9a-f]+", tok):
            w["ec"] = int(tok[1:], 16)
        elif tok[0] == "m" and tok[1:].isdigit():
            w["msgid"] = int(tok[1:])
        elif tok[0] == "w" and re.fullmatch(r"w[0-9a-f]+", tok):
            w["word"] = int(tok[1:], 16)
        elif tok[0] == "c" and re.fullmatch(r"c[0-9a-f]{9,}", tok):
            w["cont"] = int(tok[1:], 16)
        elif tok[0] == "d" and re.fullmatch(r"d-?\d+", tok):
            w["dst"] = " ".join(rest[i:i + 4])
            break
    return w


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("log")
    ap.add_argument("--cache", default=os.environ.get("DYLD_CACHE_DIR", ""))
    ap.add_argument("--kernel", default="")
    ap.add_argument("--min", type=int, default=100)
    ap.add_argument("--max", type=int, default=3000)
    ap.add_argument("--from", dest="start", type=float, default=0)
    ap.add_argument("--to", dest="stop", type=float, default=1e9)
    ap.add_argument("--top", type=int, default=25)
    ap.add_argument("--thread", default="")
    ap.add_argument("--raw", action="store_true", help="each wait, in time order, named")
    args = ap.parse_args()

    text = open(args.log, errors="replace").read()
    section = re.search(r"--- wait profile \((\d+) bytes, (\d+) lines did not fit\) ---\n(.*?)--- end of wait profile ---", text, re.S)
    if not section:
        sys.exit("no wait profile section in this log (a kernel before the profiler's own buffer, or waitprof= not given)")
    lines = section.group(3).splitlines()
    print("wait profile: %s bytes, %s lines did not fit" % (section.group(1), section.group(2)))
    slides = {}
    kslide = 0
    for l in lines:
        m = re.match(r"wpc (\S+): shared cache at (\S+) slide (\S+) kernel slide (\S+)", l)
        if m:
            slides[m.group(1)] = int(m.group(3), 16)
            kslide = int(m.group(4), 16)
    slide = next(iter(slides.values()), 0)      # (one shared region: the same in every process)
    # this project's own libraries and the programs: where the end-of-run snapshot says they were loaded
    own = sorted((int(m.group(1), 16), os.path.basename(m.group(2)))
                 for m in re.finditer(r"hakios:   (?:frame )?image ([0-9a-f]+) (\S+)", text))
    cache = Cache(args.cache) if args.cache else None
    kernel = Kernel(args.kernel) if args.kernel else None

    def uname(a):
        if cache is not None:
            n = cache.name(a - slide)
            if n:
                return n
        i = bisect.bisect_right(own, (a, "~")) - 1
        if i >= 0 and a - own[i][0] < 0x4000000 and not (CACHE_BASE <= a - slide < CACHE_BASE + (8 << 30)):
            return "%s+%#x" % (own[i][1], a - own[i][0])
        return "%#x" % a

    def kname(a):
        return kernel.name(a - kslide) if kernel else "%#x" % a

    waits = [w for w in map(parse, lines) if w]
    if waits:
        print("waits: %d, from %.1f s to %.1f s" % (len(waits), min(w["end"] - w["ms"] / 1000 for w in waits), max(w["end"] for w in waits)))
    sel = [w for w in waits if args.min <= w["ms"] <= args.max and args.start <= w["end"] <= args.stop and
           (not args.thread or args.thread in w["name"] or args.thread == str(w["tid"]))]

    def signature(w):
        if w["kind"] == "R":
            m = re.search(r" rq(-?\d+) .* ia([0-9a-f]+) ", w["rest"])
            if m:       # (the run queue's processor, and whether a processor was idle for the whole stay)
                return "RUNNABLE priority %s on cpu %s's queue, %s" % (w["pri"], m.group(1),
                                                                      "a processor idle throughout" if int(m.group(2), 16) else "no processor idle throughout")
            return "RUNNABLE (kept off the CPUs) priority %s" % w["pri"]
        frames = [uname(a) for a in w["u"]]
        shown = [f for f in frames if not any(b in f for b in BORING)][:3]
        if not shown:
            shown = frames[-2:]
        trap = TRAPS.get(w["trap"], "trap %s" % w["trap"]) if w["ec"] == 0x15 else "fault ec %#x" % (w["ec"] or 0)
        s = trap
        if w["msgid"] is not None and w["trap"] in (-31, -47):
            s += " msgid %d" % w["msgid"]
            if w["word"]:
                s += " w %#x" % w["word"]
        if w["utmo"] not in (None, 0, 4294967295):
            s += " tmo %d" % w["utmo"]
        if w["dst"]:
            s += " -> " + w["dst"]
        s += " | " + " < ".join(shown)
        ks = [kname(a) for a in w["k"]]
        ks = [k for k in ks if not re.match(r"(thread_block|thread_invoke|thread_continue|lck_|ipc_mqueue|mach_msg|waitq_|assert_wait|"
                                            r"sleh_|fleh_|handle_|unix_syscall|mach_syscall|Call_continuation|arm64_|ksleep|msleep|_sleep|"
                                            r"thread_block_reason|IOLockSleep|IORecursiveLockSleep|semaphore_)", k)][:3]
        if ks:
            s += " | k: " + " < ".join(ks)
        return s

    if args.raw:
        for w in sorted(sel, key=lambda w: w["end"]):
            print("%8.3f %5d ms %s tid %d '%s' %s" % (w["end"] - w["ms"] / 1000, w["ms"], w["proc"], w["tid"], w["name"], signature(w)))
        return
    groups = collections.defaultdict(lambda: [0, 0, 0, None])
    for w in sel:
        g = groups[(w["proc"] + " " + (w["name"] or "tid %d" % w["tid"]), signature(w))]
        g[0] += w["ms"]
        g[1] += 1
        g[2] = max(g[2], w["ms"])
    print("waits of %d..%d ms: %d, by thread and place (total ms, count, longest):" % (args.min, args.max, len(sel)))
    for (name, sig), g in sorted(groups.items(), key=lambda kv: -kv[1][0])[:args.top]:
        print("%7d ms %4d x  max %4d  '%s'  %s" % (g[0], g[1], g[2], name, sig))
    shares = collections.defaultdict(lambda: [0, 0, 0])
    for l in lines:
        m = re.match(r"wps (\d+) (\d+) '([^']*)' (\d+) (\d+) (\d+) ?(\S*)", l)
        if m and args.start <= int(m.group(1)) <= args.stop:
            s = shares[m.group(7) + " " + (m.group(3) or "tid " + m.group(2))]
            s[0] += int(m.group(4)) * int(m.group(6)) // 100
            s[1] += int(m.group(5)) * int(m.group(6)) // 100
            s[2] = max(s[2], int(m.group(5)))
    if shares:
        print("threads' CPU time over the window (ms run, ms runnable, worst runnable % of a period):")
        for name, s in sorted(shares.items(), key=lambda kv: -kv[1][0])[:15]:
            print("%8d %8d %3d%%  '%s'" % (s[0], s[1], s[2], name))


if __name__ == "__main__":
    main()
