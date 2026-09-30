# Benchmarks and validation tools

Three layers, from "no hardware" to "real dongle".

## 1. Offline DSP benchmarks + accuracy gates (no hardware)
    cmake -DBUILD_TESTS=ON .. && make
    ./tests/bench_fm      # rtl_fm kernels: discriminators (accuracy vs atan2), decimators, FIR, AM, resampler
    ./tests/bench_power   # rtl_power fixed-point FFT (SNR vs double), window gain / ENBW table
    ./tests/bench_adsb    # rtl_adsb: exact-decode rate vs SNR, phantom frames on noise, throughput
    add -q for a quick run and -c to make a missed quality threshold exit non-zero (that is what ctest runs)

Reference run (Intel Xeon 2.1 GHz, one core, `-O2`, so **your numbers will differ**):

| kernel | MS/s | x real time @2.4 MS/s |
|--------|-----:|----:|
| FM discriminator, libm atan2 | 53 | 22 |
| FM discriminator, polynomial (`-A fast`) | 286 | 119 |
| FM discriminator, table (`-A lut`) | 278 | 116 |
| boxcar decimate x8 | 1013 | 422 |
| 5th-order decimate x2 (I+Q) | 666 | 278 |
| 9-tap droop FIR (I+Q, 64-bit accumulate) | 155 | 65 |
| AM envelope | 468 | 195 |
| resampler 170k->32k | 1741 | 725 |
| rtl_power FFT 1024 / 16384 / 65536 | 49 / 31 / 24 | |
| rtl_adsb decoder | 94 | 47 (at 2 MS/s) |

Accuracy (FM, 75 kHz deviation): libm 0.004 % rms; table 0.065 % rms / 0.13 % max (was a full-scale
spike for small positive steps); polynomial 1.93 % rms / 2.27 % max, i.e. `-A fast` adds about
-34 dB of distortion by design.
rtl_power fixed-point FFT vs double: 60 dB (256 pt) .. 36 dB (65536 pt) SNR, quantisation floor about -88 dB re full scale.
rtl_adsb (DF17, 2 MS/s, Gaussian noise): 12 dB 95 %, 16 dB 99.8 %, 20 dB and above 100 %, 0 phantom frames on noise.

## 2. `rtl_validate` (real dongle)
    cmake -DBUILD_VALIDATE=ON .. && make rtl_validate
    ./src/rtl_validate            # full run, ~1 minute
    ./src/rtl_validate -q         # quick sweep
    ./src/rtl_validate -c out.csv # also write results as CSV
    -d index|serial  -t seconds per stream test  -T skip host-clock timing  -v verbose

Connect a 50 ohm terminator or no antenna for the ADC checks. The bias tee is never switched on.
Every check prints PASS / WARN / FAIL / SKIP; exit status 0 = no failure, 1 = failure, 2 = usage/device error.

| check | what it verifies |
|-------|------------------|
| identity, eeprom | USB strings, tuner detected, EEPROM readable with a valid header |
| api-contract | invalid rates / ppm / direct-sampling mode / GPIO pin / EEPROM range are rejected |
| sample-rate | 12 rates from 250 kS/s to 3.2 MS/s program and read back within 5 ppm |
| gain-table | gain list monotonic and every entry accepted and read back |
| tuning-range | sweep 20 MHz..2.3 GHz: the tuner's nominal range must tune and read back, gaps are listed |
| retune-latency | median / p95 / max time of `rtlsdr_set_center_freq()` |
| pll-stress | 500 random retunes inside the nominal range, none may fail |
| adc-noise / dc-offset / iq-balance / clipping | 8-bit ADC sanity: sigma, DC (ideal 127.5), I/Q balance, clipping |
| stream-1024k .. 3200k | async stream with the RTL2832 test-mode byte counter: lost bytes and sample-clock error vs the host clock (3.2 MS/s is only a warning: USB2 hubs) |
| async-cancel | cancel latency |

The same tool runs in CI against the emulated dongle (`ctest`, test `rtl_validate_fake`), and it
fails (or crashes) against the unpatched 2.0.3 library: it reports the wrong `rtlsdr_read_eeprom()`
return value and the missing argument checks.

## 3. Manual RF checks (not automated)
Frequency accuracy needs a reference: tune a known carrier (GSM/LTE/NOAA) and compare with `-p`. `rtl_test -p` and
`rtl_validate`'s stream clock measure the sample clock against the host clock only.
