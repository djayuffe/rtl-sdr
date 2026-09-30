# Hardware-free regression tests

`fake_libusb.c` replaces libusb with a tiny emulation of an RTL2832U + R820T/E4000
tuner + 24C02 EEPROM, so librtlsdr and the tool helpers can be tested (and run under
AddressSanitizer / UBSan) without a dongle.

    cmake -DBUILD_TESTS=ON -DCMAKE_C_FLAGS="-fsanitize=address,undefined" ..
    make && ctest --output-on-failure

| test | covers |
|------|--------|
| `test_librtlsdr` | enumeration, open, tuning limits, ppm range, EEPROM API, bias-tee pin range, async streaming incl. submit/events failures, E4000 PLL |
| `test_dsp_fm` | `rtl_fm` atan/LUT overflow, rms/mad, saturation, empty blocks, frequency ranges, suffix parsers |
| `test_dsp_power` | `rtl_power` DC removal, FFT, invalid range handling (child process) |
| `test_adsb` | end-to-end decode of a synthesised DF17 frame, truncated frame rejection |

The fake bus only proves logic and memory safety; it says nothing about RF behaviour or timing on real hardware.

| `bench_fm`, `bench_power`, `bench_adsb` | offline benchmarks with accuracy gates (`-c`), see ../BENCHMARK.md |
| `rtl_validate_fake` | the on-device validation tool (about 40 checks, see BENCHMARK.md) run against the emulated dongle |
| `test_rtl_power_e2e` | tone -> rtl_power scanner/FFT/CSV: peak frequency vs the tone, cropped and decimated scans |
| `e2e_rtl_fm.py` | the real rtl_fm binary demodulating FM/AM/wbfm from the emulated dongle (python3), incl. a wrong-side negative control |

Signals for the emulator: `fake_set_signal()` in C, or `FAKE_SIGNAL=fm:1000:5000:60` (kind:audio_hz:dev_hz:amp),
`FAKE_CARRIER_FRAC` (fraction of the sample rate) and `FAKE_CARRIER_HZ` in the environment.
