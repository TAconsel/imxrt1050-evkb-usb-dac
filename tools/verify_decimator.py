#!/usr/bin/env python3
"""
Check the ON-DEVICE 96->48 kHz path against the host reference.

roomcorr_filter.c decimates an uploaded 96 kHz IR with the fixed FIR in
roomcorr_resample.h, while tools/make_filter.py uses scipy resample_poly. If a filter
built on the board is to behave like one built on the host, those two have to agree.
This re-implements the C routine exactly -- same taps, same delay, same x2 -- and
compares the resulting frequency responses.
"""
import re, numpy as np, scipy.signal as sig, scipy.io.wavfile as wav

src = open('usb_dac32/roomcorr_resample.h').read()
taps = int(re.search(r'RC_DECIM_TAPS\s+\((\d+)U\)', src).group(1))
delay = int(re.search(r'RC_DECIM_DELAY\s+\((\d+)U\)', src).group(1))
fir = np.array([float.fromhex(v) for v in re.findall(r'(-?0x[0-9a-f.p+-]+)f,', src)])
assert len(fir) == taps, (len(fir), taps)
print(f"device FIR: {taps} taps, delay {delay}, sum {fir.sum():.6f}")

def device_decimate(x):
    """literal transcription of decimate_half() in roomcorr_filter.c"""
    n = len(x); out = n // 2
    y = np.zeros(out)
    for i in range(out):
        c = 2 * i
        acc = 0.0
        for k in range(taps):
            idx = c + k - delay
            if 0 <= idx < n:
                acc += fir[k] * x[idx]
        y[i] = acc * 2.0
    return y

fs, x = wav.read('Test900.wav')
x = x.astype(np.float64)
assert fs == 96000

for c in range(2):
    dev  = device_decimate(x[:, c])
    host = sig.resample_poly(x[:, c], 1, 2, window=('kaiser', 12.0)) * 2.0
    n = min(len(dev), len(host))
    f = np.fft.rfftfreq(n, 1 / 48000)
    Hd = np.abs(np.fft.rfft(dev[:n])); Hh = np.abs(np.fft.rfft(host[:n]))
    band = (f >= 10) & (f <= 20000)
    err = 20 * np.log10((Hd[band] + 1e-12) / (Hh[band] + 1e-12))
    # also compare against the original 96k response, which is what actually matters
    f0 = np.fft.rfftfreq(len(x), 1 / 96000); H0 = np.abs(np.fft.rfft(x[:, c]))
    ref = np.interp(f[band], f0, H0)
    err0 = 20 * np.log10((Hd[band] + 1e-12) / (ref + 1e-12))
    print(f"ch{c}: device vs host tool   max {np.abs(err).max():.3f} dB, "
          f"rms {np.sqrt((err**2).mean()):.4f} dB")
    print(f"     device vs original 96k  max {np.abs(err0).max():.3f} dB, "
          f"rms {np.sqrt((err0**2).mean()):.4f} dB")
