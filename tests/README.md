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
