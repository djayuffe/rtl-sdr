# Audit report: osmocom/rtl-sdr 2.0.3 (797f814)

Scope: `src/` (librtlsdr, all 8 tools, 5 tuner drivers, convenience), `include/`, build files, udev rules.
Method: line-by-line review, cppcheck, GCC/Clang warnings, ASan+UBSan builds, and a hardware-free test
bench (`tests/`) that runs the library and tools against a fake libusb. Every fixed item below has a test
or a reproducible run that fails on the original code and passes now (see "Evidence").

**Not verified on real hardware.** The timing changes (small sleeps in `tuner_r82xx.c`, `tuner_fc0012.c`,
`tuner_fc0013.c`) and RF behaviour must be checked with a dongle before release.

## Fixed

### Crashes / memory safety
| where | issue | fix |
|-------|-------|-----|
| `tuner_r82xx.c` set_pll | ppm near -1e6 or xtal 0 -> division by zero (SIGFPE) | ppm range check in `rtlsdr_set_freq_correction`, zero guards |
| `tuner_r82xx.c` set_pll | `nint` was `uint8_t`: values >255 wrapped and passed the range check; `div_num` underflowed; no divider found (LO <~27.7 MHz / >1.77 GHz) silently programmed a bogus divider | 32-bit `nint`, lower/upper bounds, clamp, error when no divider |
| `librtlsdr.c` enumeration | `libusb_get_device_list()` failure left `list` undefined but it was freed/iterated (4 places) | check `cnt < 0` |
| `librtlsdr.c` open | free of device list leaked when no device matched; `index==0xffffffff` could open the first non-RTL USB device | fixed matching + free |
| `librtlsdr.c` async | `libusb_handle_events` error left transfers in flight while their buffers were freed (use-after-free); alloc failures unchecked | cancel/reap before free, `calloc` + checks |
| `librtlsdr.c` gpio | `rtlsdr_set_bias_tee_gpio(dev, 256, ..)` was truncated to pin 0 = bias tee | pin range 0..7 |
| `librtlsdr.c` regs | failed register reads returned uninitialised stack data (used in read-modify-write of GPIO) | zero-init |
| `rtl_fm.c` | `fast_atan2` / LUT discriminator overflowed 32 bit for strong signals (UB + wrong audio); `fm_demod` read `lp[-2]` on empty blocks; dc-block/`mad`/`rms` divided by zero or by the wrong count; `frequency_range` wrote past `freqs[]`; `-o` out of range indexed `lcm_post[]`; history arrays overrun for rates < ~1 kHz | 64-bit math, guards, exit on bad `-o`, clamp passes |
| `rtl_power.c` | zero-width / inverted range -> SIGFPE; `buf_len` int overflow; `n_read` uninitialised after failed read; endless error loop on unplug; FFT window product wrapped int16 | validation, 64-bit, read-error exit, saturation |
| `rtl_eeprom.c` | string-descriptor offsets from the EEPROM indexed outside the buffer; `-w` accepted a short file and wrote stack garbage to the dongle | bounds checks, abort on short read, header check |
| `rtl_test.c` | division by zero on immediate Ctrl-C and with `-p0`; 32-bit sample counters wrapped after ~35 min | 64-bit, guards |
| `tuner_e4k.c` | failed I2C read treated as 0xFF and written back; failed lock-status read counted as "locked"; Z >255 wrapped | error propagation, range check |
| `tuner_fc0012/13.c` | `freq * multi` overflowed 32 bit; gain read errors ignored | 64-bit, checks |
| `convenience.c` | `atofs/atoft/atofp("")` wrote `s[-1]` | guard |

### Wrong results / functional bugs
| where | issue |
|-------|-------|
| `rtl_sdr -n` | when `-n` was an exact multiple of the block size the counter hit 0 which meant "unlimited": capture never stopped (reproduced: 1.2 GB in 5 s instead of 32 KB). Windows stdout was set binary on **stdin**. `fopen` failure returned exit code 0 |
| `rtl_test -p` | the 5 s warm-up discard condition was inverted (never discarded) |
| `rtl_power` | DC removal divided the sum of half the samples by the full length (only half of the DC removed) |
| `rtl_adsb` | callback overwrote the buffer while the demod thread read it; lost wake-ups; `pthread_cancel` could leave the mutex locked; a frame missing its last bit was accepted |
| `rtl_fm` | AM/USB/LSB cast to int16 before scaling (wrap instead of clip); `-t -N` (exit on squelch) was parsed but never acted on; shutdown could deadlock / hang if Ctrl-C arrived before the async loop started; failed tune / sample rate was ignored |
| `rtl_tcp` | Ctrl-C during a client session only ended the session (server kept running, reproduced); client disconnect (`recv()==0`) busy-looped; workers could wait 5 s after disconnect; bind used the first addrinfo entry for every attempt; unchecked `accept`, `pthread_create`, `malloc`; stalled client held the server forever; leaked memory on client exit |
| `librtlsdr.c` | `rtlsdr_get_device_name(idx)` returned the last device's name for an out-of-range index; `rtlsdr_get_device_usb_strings` returned success (0) for an index that does not exist, leaving buffers untouched; `rtlsdr_read_eeprom` returned the transfer size instead of the documented 0; async submit failure was reported as success; failed resubmit stalled the stream silently; sample-rate correction >488 ppm silently wrapped the 14-bit register; IF frequency was truncated instead of rounded; `r820t_set_bw` retuned to 0 Hz before the first frequency; bad `direct_sampling` values accepted |
| `tuner_r82xx.c` | shadow registers were updated before the I2C write succeeded (a later identical write was then skipped); PLL not locked was reported as success; V4 upconverter switch used `<` while band selection used `<=` at exactly 28.8 MHz; `r82xx_init` ignored failures of its first writes; calibration/PLL had no settling delay (commented out) |

## Left as is (documented, needs a decision or hardware)
* `rtl_tcp` has no authentication (a warning is printed when it listens on a non-loopback address).
* `rtl_adsb`: no CRC/parity validation, frames spanning two buffers are lost, leftover magnitude samples 0/1 can be read as bits.
* `rtl_power` CSV repeats the last bin (format kept for `heatmap.py`); `-s`/`-t` are accepted but unused; 1 s time granularity; retune settle time (5 ms + 4 KiB flush) is a heuristic.
* `rtl_fm`: `-o` is documented as buggy; `low_pass_real` uses an integer divisor (gain ripple for non-integer ratios); 16-bit accumulation overflows for input decimation >258 (rates < 3.9 kHz); `deemph_filter` keeps static state.
* All tools call `rtlsdr_set_bias_tee(dev, 0)` when `-T` is absent, i.e. they write GPIO0 even on dongles where GPIO0 is not a bias tee.
* `verbose_device_search` silently picks the first prefix/suffix serial match.
* `fc0013` gain table has an unreachable duplicate `-63` entry; `fc2580` has no frequency range checks; E4000 VCO range is left to the lock detector.
* `rtlsdr_close()` busy-waits for the async loop; the library prints diagnostics to stderr.

## Second pass: numerical / DSP verification
Each claim was checked with a script or a test, not just read.

| topic | result |
|-------|--------|
| **Sample-rate ratio** (`rtlsdr_set_sample_rate`) | The 28-bit ratio field mirrors bit 27 into bit 28, so only ratios with bit28==bit27 are representable. The fixed 225..300 kS/s / 900 kS/s limits are exact for a 28.8 MHz crystal, but `rtlsdr_set_xtal_freq()` allows +-1 kHz: with 28.799 MHz, 300000 S/s and with 28.801 MHz, 225001 S/s were programmed with a ratio the hardware cannot hold (wrong rate, silently). Now validated from the real ratio. `dev->rate` is rounded (249999.9999 no longer reads back as 249999) |
| **R82xx manual gain** | The 29 published gains equal the cumulative LNA+mixer sums exactly (verified). But any request above 496 selected mixer step 15 (-8 dB): 500 or 1000 ended at 488, *lower* than the maximum. Saturates at 496 now |
| **R82xx PLL** | Simulated over 30 MHz..1.766 GHz: LO quantisation error <= 220 Hz (16-bit SDM, theory 2*xtal/65536/2), nint always 30..61, lowest LO with a valid divider 27.66 MHz (so tuning below ~24.1 MHz has no divider; it now reports an error instead of programming a wrong one) |
| **IF frequency register** | 22-bit two's complement; above +-14.4 MHz it wraps, i.e. direct sampling above fs/2 shows a *mirrored* spectrum (aliasing, documented not fixed). Rounded instead of truncated (up to 6.9 Hz) |
| **Corrected crystal** | `xtal*(1+ppm/1e6)` was truncated, now rounded |
| **Offset tuning** | `freq - offs_freq` wrapped to ~4 GHz below ~1.7 MHz at 2.048 MS/s: now rejected |
| **rtl_fm 170k->32k resampler** | Divided every window by the constant 5 although windows hold 5 or 6 samples: gain 1.0..1.2 that changed from output to output (AM ripple on the audio). Now a true average |
| **rtl_fm `-F` decimator** | State handling dropped the last sample of every block (phase glitch once per buffer) and truncated instead of rounding. Rewritten with a 4-sample history; continuity across blocks is tested |
| **rtl_fm/rtl_power droop FIR** | `int16 * 77818` overflowed 32 bit for large samples (UBSan reproduced) -> 64-bit accumulate + saturate |
| **rtl_fm `-o`** | `low_pass_simple` summed without saturation and read a partial last group: fixed |
| **rtl_power windows** | Hann-Poisson and Youssef were shifted half a sample (`|N-1-2i|` instead of `|N-1-2i+1|`) and Bartlett divided by L/2 instead of (L-1)/2, so they were not symmetric (measured); `-w kaiser` is a rectangle, now says so |
| **CIC droop tables** | DC gain 1.072..1.098 (+0.6..0.8 dB), i.e. `-F 9` changes the level slightly (not changed, empirical tables) |
| **E4000 PLL** | VCO = flo*R checked against 2.6..3.9 GHz for 50 MHz..2.2 GHz: outside at 50-54 MHz, 432-433, 650.5-666.5, 975-1300 (documented L-band gap) and everything above 1950 MHz, so the advertised 2.2 GHz limit is not reachable; the tuner's lock detector already reports failures |
| **rtl_test -p (Windows)** | `ppm_gettime()` read `init`/`frequency` from an uninitialised stack struct (cppcheck): timing divided by garbage. Made static (found by cppcheck, not testable here) |
| **rtl_power level scale** | dB values are relative: FFT modes and the `-f a:b:>=1M` "rms" mode differ by a constant (sum vs mean of squares), window gain is not normalised. Not changed (would change stored data) |

## Verification summary
`ctest` (4 suites) passes under ASan+UBSan. The second-pass tests fail on the first-pass code:
sample-rate edges (2), gain saturation (1), resampler gain, decimator continuity, window symmetry (2).
GCC `-fanalyzer` and cppcheck report nothing actionable in the final tree.

## Third pass: tuner drivers, remote-controllable state
| topic | result |
|-------|--------|
| **Tuner crystal from the network** | `rtl_tcp` command 0x0c sets the tuner crystal to any value. With FC0012/FC0013 a value below 2 Hz made `xtal_freq_div_2` zero and the driver divided by zero (remote crash). Now `rtlsdr_set_xtal_freq()` accepts 8..60 MHz only and both drivers refuse a clock below 2 kHz. Tested with an emulated FC0012 (fails on the earlier code) |
| **FC0012/13 PLL** | `freq * multi` was 32 bit: above ~1.07 GHz (multi=4) the VCO frequency wrapped and the driver programmed a garbage but "valid" divider. Now 64 bit, out-of-range frequencies return an error (tested) |
| **EEPROM write** | `rtl_eeprom` now reads the EEPROM back and reports failure if it differs (a worn/write-protected part acknowledges writes but keeps its content) |
| **FC2580** | `k` rounding up to 2^20 would carry into the R field of register 0x18; guarded. Exhaustive search shows it is unreachable with the hard-coded 16.384 MHz crystal, so this is defensive only |

## Known, not changed (need hardware or a decision)
* `fc2580_set_bw()` ignores the requested bandwidth and always selects the 1.53 MHz filter; `fc2580_set_freq()` then re-selects a band filter, so the effective filter depends on whether the frequency or the sample rate is set last (`rtl_fm` sets the frequency first).
* The FC2580 crystal (16.384 MHz) is hard-coded: `-p` ppm correction does not reach its LO.
* A `-p` correction is applied to the sample clock and the tuner LO together (correct for the usual shared 28.8 MHz crystal, wrong for dongles with a separate tuner crystal).
* `rtlsdr_set_tuner_gain()` on R820T/R828D also switches LNA and mixer to manual mode even if automatic gain was selected.
* Direct sampling on/off does not reset an active offset-tuning offset.
* The FC2580/FC0012/FC0013 calibration loops have no delays (USB latency is assumed to be enough).

## Fourth pass: static analysis sweep
clang `--analyze` and GCC `-fanalyzer` over every source file. Only one real finding:
* `rtl_tcp` send queue (`rtlsdr_callback`): with `-n <= -2` the drop-oldest branch freed the *tail* and then appended to it (use-after-free, clang `unix.Malloc`); and the limit was off by two (kept `n+2` buffers). Rewritten: at most `n` buffers, `n <= 0` is unlimited, negative values rejected (`tests/test_tcp_queue.c`, fails on the old code).
* Everything else was dead stores (`custom_ppm`, `smoothing`, `fft_threads`: options that are parsed but unused) and a one-time leak of the `-f` string in `rtl_power`.

`src/getopt/` is the unmodified GNU getopt from glibc 2001 (LGPL), used on Windows only; it was skimmed, not audited or changed.

## Fifth pass: benchmarks and on-device validation (BENCHMARK.md)
Building the accuracy benchmarks exposed two more real bugs that no code reading had caught:
* **`rtl_fm -A lut` produced full-scale spikes.** `polar_disc_lut` decided the quadrant from the sign of the quantised ratio `x`; `x` is 0 for every phase step below 0.22 degrees, which fell into the "negative" branch and returned **+pi** for small positive steps (max error 16370 of 16384, reproduced). Quadrant now comes from the signs of cr/cj. Table discriminator accuracy is 0.065 % rms.
* **`rtl_adsb` lost 15-17 % of frames at any SNR.** `preamble()` compared each sample with the *most recent* opposite sample, so noise in the quiet gap before a frame matched as a preamble; the greedy decoder then decoded through the real preamble and lost the frame. Requiring all four pulses to exceed every quiet sample by 50 % (the min/max variant was commented out in the source) raises exact decodes from 83 % to 99.8 % at 16 dB and 100 % at 20 dB, with 0 phantom frames on noise. The 1.5x margin was chosen by sweeping 0.5x..4x: 2x costs 4 points at 12 dB, 3x costs 25.
* Measured (not changed): `-A fast` polynomial discriminator has 1.93 % rms / 2.27 % max phase error; the fixed-point FFT of `rtl_power` loses 24 dB SNR between 256 and 65536 points (quantisation floor about -88 dB re full scale) and none of the windows are gain-normalised (coherent gain 0.30 .. 1.0).
* New tools: `rtl_validate` (on-device checks and benchmark, `-DBUILD_VALIDATE=ON`), `tests/bench_fm|power|adsb` (offline, with `-c` quality gates in ctest), see BENCHMARK.md. `rtl_validate` has about 40 checks (groups self/api/rf/stream/ref/opt, `-l` lists them). It found one more library bug: `rtlsdr_set_direct_sampling(dev, 0)` returned an error when no frequency was set yet or the stored direct-mode frequency was unreachable for the R820T (fixed, regression test in test_librtlsdr).

Datasheets: the R820T/R820T2 and RTL2832U datasheets are hosted on rtl-sdr.com, homepages.uni-regensburg.de, theretroweb.com, datasheet4u.com and deepwiki.com, all of which this build environment's network policy blocks. Only search-result excerpts were available, and they agree with the code: register 0x10 bits [7:5] = divider number (dividers 2..64, LO ranges 864-1785.6 MHz for /2 and 432-892.8 MHz for /4, i.e. a VCO of about 1728-3571 MHz; the code uses the narrower 1770-3540 MHz), N = 13 + 4*Ni + Si with Si in register 0x14 bits [7:6]. One excerpt mentions a 3.9 GHz upper VCO limit; the code stops at 3.54 GHz. The RTL2832U register widths (28-bit resample ratio, 14-bit ppm offset, 22-bit IF) could not be confirmed from a datasheet; `rtl_validate` checks the resulting behaviour on real hardware instead.

## Sixth pass: end-to-end signal tests
The fake bus can now emit a tone, an FM or an AM carrier at a chosen offset from the tuned centre (`fake_set_signal()`, or `FAKE_SIGNAL` / `FAKE_CARRIER_*` for the real binaries). Two end-to-end tests run the real code paths:
* `test_rtl_power_e2e`: tone -> library -> `scanner()` -> FFT -> CSV; the strongest bin must be at the tone frequency (`low + j*step`, the axis `heatmap.py` uses) for above/below-centre tones, cropped and decimated scans.
* `e2e_rtl_fm.py`: the real `rtl_fm` binary demodulates FM (`-A std|fast|lut`, `-F 9`), AM and `-M wbfm` audio; the 1 kHz tone must carry >70 % of the audio power, and a carrier on the wrong side of the centre (negative control) must not. This also proves the fs/4 rotation direction.

Bugs found and fixed by them:
* **`rtl_power -c` mislabelled the frequency axis.** The CSV `low`/`high` came from a truncated bin count while the row holds bins `i1..i2`; with cropping the whole row was shifted by 1-2 bins (up to 12.5 kHz at 9.8 kHz bins). Now derived from the bins actually printed. Uncropped output is unchanged.
* **`rtl_power` clipped the decimated scans at modest levels.** With the default boxcar decimator (any range < 1 MHz wide) the window product `x*256` (x up to 127*ds) overflowed int16: signals of only ~20 counts clipped (int16 wrap in the original), the clipped tone's 3rd harmonic aliased onto the DC region and the reported peak moved by up to 27 kHz (DC bin for ds=6). The `w /= ds` that was commented out in the source is now active and `csv_dbm()` restores `ds^2`, so levels of unclipped signals are unchanged.
* `rtl_fm -A lut` (spike bug from pass five) also shows up end to end: 68 % of the audio power in the tone before, 100 % now.

## Evidence
`tests/` (`-DBUILD_TESTS=ON`) - 4 suites, all pass under ASan+UBSan. The same tests run against the untouched 2.0.3 sources fail
(division by zero in `r82xx_set_pll`, 32-bit overflow in `fast_atan2`, half DC removal, `rtl_power` SIGFPE, wrong device
names, ...). `rtl_sdr -n` and `rtl_tcp` SIGINT were additionally reproduced with the real tool binaries against the fake bus.
