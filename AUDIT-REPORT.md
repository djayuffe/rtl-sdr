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

## Evidence
`tests/` (`-DBUILD_TESTS=ON`) - 4 suites, all pass under ASan+UBSan. The same tests run against the untouched 2.0.3 sources fail
(division by zero in `r82xx_set_pll`, 32-bit overflow in `fast_atan2`, half DC removal, `rtl_power` SIGFPE, wrong device
names, ...). `rtl_sdr -n` and `rtl_tcp` SIGINT were additionally reproduced with the real tool binaries against the fake bus.
