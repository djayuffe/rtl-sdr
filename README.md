# rtl-sdr (audited fork of osmocom/rtl-sdr 2.0.3)

Turns your Realtek RTL2832 based DVB dongle into a SDR receiver.
Upstream: <https://osmocom.org/projects/rtl-sdr/wiki> · <https://github.com/osmocom/rtl-sdr>

This is osmocom/rtl-sdr **2.0.3 (797f814)** plus the fixes below, a hardware-free test bench, offline
benchmarks and an on-device validation tool. Upstream history is not included.
Details, evidence and open items: [AUDIT-REPORT.md](AUDIT-REPORT.md) · [BENCHMARK.md](BENCHMARK.md) · [tests/README.md](tests/README.md).

**Not verified on real hardware.** Everything was tested against an emulated dongle under ASan/UBSan, and the tests fail on the
original code. The small settling delays added to the tuner drivers and all RF behaviour need a real dongle: run `rtl_validate`.

## Build, test, validate
    mkdir build && cd build
    cmake .. -DBUILD_TESTS=ON -DBUILD_VALIDATE=ON      # add -DCMAKE_C_FLAGS="-fsanitize=address,undefined" for sanitizers
    make && ctest --output-on-failure                  # 11 suites, no hardware needed
    ./src/rtl_validate -q                              # on a real dongle (antenna port terminated)
    ./tests/bench_fm; ./tests/bench_power; ./tests/bench_adsb   # offline DSP benchmarks, -q quick, -c quality gates

## Everything that was fixed

### Crashes, memory safety, undefined behaviour
- **R82xx PLL:** division by zero (ppm near -1e6 or crystal 0), 8-bit `nint` wrap that passed the range check, `div_num` underflow, no valid divider (LO below ~27.7 MHz) silently programmed a bogus divider.
- **Async streaming:** use-after-free when `libusb_handle_events` failed (buffers freed while transfers in flight); unchecked allocations; a failed resubmit stalled the stream silently.
- **USB enumeration:** `libusb_get_device_list()` failure was dereferenced/freed (4 places); `rtlsdr_open(0xffffffff)` could open an unrelated USB device; device list leaked.
- **GPIO:** `rtlsdr_set_bias_tee_gpio(dev, 256, ..)` was truncated to pin 0 (the bias tee). Pin range 0..7 enforced.
- **Register reads** returned uninitialised stack data on failure (used in GPIO read-modify-write).
- **rtl_fm:** 32-bit overflow in the FM discriminators for strong signals, `lp[-2]` read on empty blocks, divisions by zero (`dc_block`, `mad`, `rms`), `freqs[]` overflow in frequency ranges, `-o` out of range indexing `lcm_post[]`, history arrays overrun for very low rates, signed-shift UB.
- **rtl_power:** SIGFPE on zero-width/inverted ranges, `buf_len` int overflow, uninitialised `n_read`, FFT window product wrapped int16, endless error loop after unplug.
- **rtl_eeprom:** string descriptors indexed outside the buffer; `-w` accepted a short file and wrote stack garbage into the dongle.
- **rtl_test:** divide by zero on immediate Ctrl-C and `-p0`; 32-bit sample counters wrapped after ~35 min; uninitialised timer struct on Windows.
- **E4000:** a failed I2C read was treated as 0xFF and written back; a failed lock read counted as "locked"; Z >255 wrapped.
- **FC0012/FC0013:** `freq*multi` overflowed 32 bit above ~1.07 GHz; a tuner crystal of a few Hz (settable through `rtl_tcp`) divided by zero.
- **rtl_tcp:** send-queue use-after-free (and off-by-two limit), unchecked `malloc`/`accept`/`pthread_create`, memory leak on client exit, stalled client held the server forever.
- **convenience:** `atofs/atoft/atofp("")` wrote `s[-1]`.

### Wrong results and functional bugs
- **Sample rate:** the 28-bit ratio (bit 28 mirrors bit 27) was checked only against fixed limits that are right for exactly 28.8 MHz; with the allowed +-1 kHz crystal, 300000 / 225001 S/s were programmed wrongly. Now validated from the real ratio; reported rate is rounded.
- **rtl_sdr `-n`:** an exact multiple of the block size meant "unlimited" (captured 1.2 GB instead of 32 KB); Windows stdout was never put in binary mode; failed `fopen` returned 0.
- **rtl_fm `-A lut`:** small positive phase steps (< 0.22 deg) returned +pi, a full-scale spike (max error 16370/16384). Fixed, 0.065 % rms.
- **rtl_fm resampler 170k->32k:** gain wandered 1.0-1.2 (constant divisor for 5- or 6-sample windows); now a true average.
- **rtl_fm `-F` decimator:** dropped the last sample of every buffer; rewritten with a continuous history. AM/USB/LSB wrapped instead of clipping; `-t -N` (exit on squelch) was never acted on; shutdown could deadlock; failed tune/sample rate was ignored; condition variables lost wake-ups.
- **rtl_adsb:** noise was accepted as a preamble and the decoder swallowed the real frame: 83 % -> 99.8 % exact decodes at 16 dB SNR (100 % at 20 dB, 0 phantom frames). Data race between USB callback and decoder, `pthread_cancel` with a held mutex, and a frame missing its last bit was accepted.
- **rtl_power:** with `-c` the CSV frequency axis was shifted by 1-2 bins; narrow (decimated) scans clipped at modest signal levels and the clipped tone's harmonic aliased into the wrong bin (peak off by up to 27 kHz); DC removal removed only half the DC; Hann-Poisson, Youssef and Bartlett windows were asymmetric; `-w kaiser` silently used a rectangle (now warns).
- **rtl_test `-p`:** the warm-up discard was inverted.
- **rtl_tcp:** Ctrl-C during a client session only ended the session; disconnect busy-looped; bind used the first address for every attempt.
- **R82xx:** gain requests above 496 fell back to 488; shadow registers were updated before the I2C write succeeded; PLL-not-locked was reported as success; V4 upconverter switch disagreed with band selection at exactly 28.8 MHz; calibration/PLL had no settling delay.
- **Library API:** `rtlsdr_get_device_name(idx)` returned the last device's name for a bad index; `rtlsdr_get_device_usb_strings` reported success for a missing device; `rtlsdr_read_eeprom` returned the transfer size instead of 0; async submit failure was reported as success; sample-rate correction above 488 ppm silently wrapped its 14-bit register; ppm range unchecked; IF frequency truncated instead of rounded; offset tuning could wrap the LO to ~4 GHz; `set_bandwidth` retuned to 0 Hz before the first frequency; invalid direct-sampling modes accepted.
- **rtl_eeprom:** now reads back and verifies what it wrote.
- **rtl_biast:** unchecked open, unchecked GPIO range.

### New
- `rtl_validate`: on-device validation and benchmark tool with about 40 checks (`-DBUILD_VALIDATE=ON`; `-l` lists them, `-r <Hz>` adds reference-carrier checks).
- `tests/`: fake libusb (RTL2832U + R820T/E4000/FC0012 + EEPROM, can emit tone/FM/AM signals), 11 ctest suites incl. end-to-end `rtl_power` and `rtl_fm` signal tests, 3 offline benchmarks with quality gates.
- `AUDIT-REPORT.md` (findings, numerical verification), `BENCHMARK.md` (numbers and how to run).

### Known, intentionally not changed
`rtl_tcp` has no authentication (a warning is printed for non-loopback addresses) · `rtl_adsb` does no CRC check · FC2580 bandwidth
is fixed at 1.53 MHz and ignores `-p` · `-A fast` has ~2 % phase error by design · `rtl_power` dB values are relative and windows are not
gain-normalised · direct sampling above fs/2 shows a mirrored spectrum. See AUDIT-REPORT.md.

## License
GPL-2.0-or-later, as upstream (see COPYING).
