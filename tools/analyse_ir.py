#!/usr/bin/env python3
"""Report what a room-correction impulse response actually does."""
import sys, numpy as np, scipy.signal as sig, scipy.io.wavfile as wav

path = sys.argv[1] if len(sys.argv) > 1 else 'Test900.wav'
fs, x = wav.read(path)
x = x.astype(np.float64)
if x.ndim == 1:
    x = x[:, None]
n, ch = x.shape
print(f"file        : {path}")
print(f"rate        : {fs} Hz")
print(f"channels    : {ch}")
print(f"taps        : {n}  ({n/fs*1000:.1f} ms)")
print(f"freq. res.  : {fs/n:.3f} Hz  (= 1 / duration)")
print(f"dtype       : {wav.read(path)[1].dtype}")
print()

for c in range(ch):
    h = x[:, c]
    pk = np.argmax(np.abs(h))
    # energy decay
    e = np.cumsum(h[::-1]**2)[::-1]
    e = e / e[0]
    t60_idx = np.argmax(e < 1e-6) if np.any(e < 1e-6) else n
    print(f"--- channel {c} ---")
    print(f"  peak tap      : {pk} ({pk/fs*1000:.2f} ms)   value {h[pk]:+.4f}")
    print(f"  abs peak      : {np.abs(h).max():.4f}")
    print(f"  sum |h|       : {np.abs(h).sum():.4f}   <- worst-case sample gain")
    print(f"  energy -60dB  : tap {t60_idx} ({t60_idx/fs*1000:.1f} ms)")

    H = np.fft.rfft(h)
    f = np.fft.rfftfreq(n, 1/fs)
    mag = 20*np.log10(np.abs(H) + 1e-12)
    print(f"  max gain      : {mag.max():+.2f} dB @ {f[np.argmax(mag)]:.1f} Hz")
    print(f"  min gain      : {mag.min():+.2f} dB @ {f[np.argmin(mag)]:.1f} Hz")
    print("  octave-band gain (dB):")
    for lo, hi in [(20,40),(40,80),(80,160),(160,315),(315,630),(630,1250),
                   (1250,2500),(2500,5000),(5000,10000),(10000,20000),(20000,48000)]:
        m = (f >= lo) & (f < hi)
        if m.any():
            print(f"    {lo:>6}-{hi:<6} Hz : mean {mag[m].mean():+6.2f}   "
                  f"min {mag[m].min():+6.2f}   max {mag[m].max():+6.2f}")
    print()
