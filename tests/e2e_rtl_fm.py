#!/usr/bin/env python3
"""End-to-end test of rtl_fm against the emulated dongle.

The fake bus emits a modulated carrier at a chosen offset from the tuned
centre (see fake_libusb.h). The real rtl_fm binary tunes, rotates by fs/4,
decimates and demodulates; the audio it writes must contain the 1 kHz
modulation tone. A carrier on the wrong side of the centre (negative control)
must NOT produce it, which proves the test can tell right from wrong.
usage: e2e_rtl_fm.py /path/to/rtl_fm_fake
"""
import math, os, struct, subprocess, sys

exe = sys.argv[1]
fails = 0


def goertzel(x, fs, f):
    w = 2 * math.pi * f / fs
    c = 2 * math.cos(w)
    s1 = s2 = 0.0
    for v in x:
        s0 = v + c * s1 - s2
        s2, s1 = s1, s0
    return (s1 * s1 + s2 * s2 - c * s1 * s2) / len(x) ** 2 * 4   # power of a sinusoid of that amplitude


def run(args, signal, env_extra, fs_out, seconds=2.5):
    env = dict(os.environ, FAKE_SIGNAL=signal, ASAN_OPTIONS="detect_leaks=0", **env_extra)
    need = int(fs_out * 2 * seconds)
    p = subprocess.Popen([exe] + args + ["-"], stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, env=env)
    data = b""
    try:
        while len(data) < need:
            chunk = p.stdout.read(need - len(data))
            if not chunk:
                break
            data += chunk
    finally:
        p.terminate()
        try:
            p.wait(timeout=10)
        except subprocess.TimeoutExpired:
            p.kill()
    n = len(data) // 2
    x = list(struct.unpack("<%dh" % n, data[:2 * n]))
    return x


def tone_share(x, fs, f0):
    x = x[int(fs * 0.5):]                      # skip start-up
    if len(x) < fs:
        return 0.0, 0
    mean = sum(x) / len(x)
    x = [v - mean for v in x]
    total = sum(v * v for v in x) / len(x)
    if total <= 0:
        return 0.0, 0
    p = goertzel(x, fs, f0) / 2                # amplitude^2/2 = power
    return p / total, int(math.sqrt(total))


def check(name, args, signal, env, fs, f0, want_tone):
    global fails
    x = run(args, signal, env, fs)
    share, rms = tone_share(x, fs, f0)
    ok = share > 0.7 if want_tone else share < 0.2
    print("  %-34s samples %6d  rms %5d  %4.0f Hz tone = %5.1f %% of power  %s"
          % (name, len(x), rms, f0, share * 100, "ok" if ok else "FAIL"))
    if not ok:
        fails += 1


print("rtl_fm end-to-end (emulated dongle):")
# narrow FM: rtl_fm tunes f + fs/4, the carrier therefore sits at -fs/4 in the baseband
check("fm (carrier at -fs/4)", ["-f", "100M", "-M", "fm", "-s", "24k"],
      "fm:1000:5000:60", {"FAKE_CARRIER_FRAC": "-0.25"}, 24000, 1000, True)
check("fm negative control (+fs/4)", ["-f", "100M", "-M", "fm", "-s", "24k"],
      "fm:1000:5000:60", {"FAKE_CARRIER_FRAC": "0.25"}, 24000, 1000, False)
check("fm fast atan (-A fast)", ["-f", "100M", "-M", "fm", "-s", "24k", "-A", "fast"],
      "fm:1000:5000:60", {"FAKE_CARRIER_FRAC": "-0.25"}, 24000, 1000, True)
check("fm lut atan (-A lut)", ["-f", "100M", "-M", "fm", "-s", "24k", "-A", "lut"],
      "fm:1000:5000:60", {"FAKE_CARRIER_FRAC": "-0.25"}, 24000, 1000, True)
check("fm 5th order decimator (-F 9)", ["-f", "100M", "-M", "fm", "-s", "24k", "-F", "9"],
      "fm:1000:5000:60", {"FAKE_CARRIER_FRAC": "-0.25"}, 24000, 1000, True)
check("am", ["-f", "100M", "-M", "am", "-s", "24k"],
      "am:1000:0:60", {"FAKE_CARRIER_FRAC": "-0.25"}, 24000, 1000, True)
# wide FM: freqs are shifted by +16 kHz, output is resampled to 32 kHz
check("wbfm (resampled to 32 kHz)", ["-f", "100M", "-M", "wbfm"],
      "fm:1000:50000:60", {"FAKE_CARRIER_FRAC": "-0.25", "FAKE_CARRIER_HZ": "-16000"}, 32000, 1000, True)

if fails:
    print("%d check(s) failed" % fails)
    sys.exit(1)
print("e2e_rtl_fm: all checks passed")
