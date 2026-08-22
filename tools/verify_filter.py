#!/usr/bin/env python3
"""
Run the EXACT algorithm the firmware will run -- uniform-partitioned overlap-save
in CMSIS packed-real-FFT layout -- and compare against a direct convolution.
If this does not match, the firmware cannot be right either.
"""
import struct, sys
import numpy as np
import scipy.signal as sig

blob = open(sys.argv[1] if len(sys.argv) > 1 else 'usb_dac32/roomcorr.bin', 'rb').read()
assert blob[:4] == b'RCF1'
fs, ch, B, P, taps = struct.unpack('<IIIII', blob[4:24])
N = 2 * B
filt = np.frombuffer(blob[24:], dtype='<f4').reshape(ch, P, N)
print(f"fs={fs} ch={ch} B={B} P={P} taps={taps} N={N}")

def unpack(v):
    """CMSIS packed N floats -> numpy rfft complex (N/2+1)"""
    s = np.zeros(N//2 + 1, dtype=np.complex128)
    s[0] = v[0]
    s[N//2] = v[1]
    s[1:N//2] = v[2::2] + 1j*v[3::2]
    return s

def pack(s):
    v = np.empty(N, dtype=np.float64)
    v[0] = s[0].real; v[1] = s[N//2].real
    v[2::2] = s[1:N//2].real; v[3::2] = s[1:N//2].imag
    return v

def cmac_packed(acc, X, H):
    """accumulate acc += X * H, all in packed layout -- what the C code does"""
    acc[0] += X[0] * H[0]           # DC: real
    acc[1] += X[1] * H[1]           # Nyquist: real
    xr, xi = X[2::2], X[3::2]
    hr, hi = H[2::2], H[3::2]
    acc[2::2] += xr*hr - xi*hi
    acc[3::2] += xr*hi + xi*hr
    return acc

rng = np.random.default_rng(0)
nsamp = B * 40
x = rng.standard_normal((nsamp, ch)) * 0.2

# ---- partitioned overlap-save ---------------------------------------------------
y = np.zeros_like(x)
for c in range(ch):
    fdl = np.zeros((P, N))
    head = 0
    prev = np.zeros(B)
    for blk in range(nsamp // B):
        cur = x[blk*B:(blk+1)*B, c]
        X = pack(np.fft.rfft(np.concatenate([prev, cur])))
        prev = cur
        head = (head - 1) % P
        fdl[head] = X
        acc = np.zeros(N)
        for p in range(P):
            cmac_packed(acc, fdl[(head + p) % P], filt[c, p])
        out = np.fft.irfft(unpack(acc), N)
        y[blk*B:(blk+1)*B, c] = out[B:]      # discard the aliased first half

# ---- reference ------------------------------------------------------------------
h = np.zeros((taps, ch))
for c in range(ch):
    # rebuild the time-domain IR from the partitions
    for p in range(P):
        seg = np.fft.irfft(unpack(filt[c, p]), N)[:B]
        if p*B < taps:
            h[p*B:min((p+1)*B, taps), c] = seg[:min(B, taps - p*B)]
    ref = sig.fftconvolve(x[:, c], h[:, c])[:nsamp]
    # the first (taps-1) samples depend on history the block loop did not have
    valid = slice(taps, nsamp)
    err = np.abs(y[valid, c] - ref[valid])
    scale = np.abs(ref[valid]).max()
    print(f"ch{c}: max abs err {err.max():.3e}  ({20*np.log10(err.max()/scale):.1f} dB "
          f"below peak {scale:.3f})")
