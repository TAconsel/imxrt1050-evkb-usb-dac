# IMXRT1050-EVKB — MCUXpresso SDK (west) blinky

## Layout
    RT1050EVKB-DESIGNFILES/   board schematic / gerber / layout / BOM (SPF-30168_B1)
    mcuxsdk/                  west workspace, manifest v26.06.00-LTS
    blinky/                   the application (out-of-tree, builds against the SDK)
    .venv/                    west + pyocd + SDK python deps
    env.sh, build.sh          helpers

## Board facts (from SPF-30168_B1 + SDK board files)
* User LED **D18** = net `USER_LED` = **GPIO_AD_B0_09 = GPIO1_IO09**, **active low**.
  That pin is also **JTAG_TDI**, so driving the LED disturbs a JTAG debug session.
* Boot flash is the **1V8 HyperFlash** (S26KS512, U19) at `0x60000000`, XIP.
  The QSPI part (IS25WP064) is the alternate build option (schematic "OPTION2",
  needs R153~R158 mounted / R356,R361~R366 removed).
* Debug probe: on-board DAPLink (`0d28:0204`), VCOM on `/dev/ttyACM0` @115200.

## Build / flash
    ./build.sh            # build only
    ./build.sh flash      # build + flash + reset

Serial console:  `screen /dev/ttyACM0 115200`  (or `picocom -b 115200 /dev/ttyACM0`)

## Notes
* Drag-and-drop to `/run/media/$USER/RT1050-EVK` works but can silently no-op —
  `pyocd flash` verifies, so prefer `./build.sh flash`.
* `examples/demo_apps/led_blinky` in the SDK does **not** compile for this board in
  v26.06.00-LTS: the shared source uses `BOARD_LED_GPIO` while the board port only
  defines `BOARD_USER_LED_GPIO`. `blinky/` sidesteps that with its own `main`.

## Verify it is really running (no eyes on the board needed)
    .venv/bin/python verify.py mimxrt1050 blinky/build/blinky.elf
Checks flash contents against the ELF segments, the live vector table, SysTick, and
polls `g_systickCounter` over SWD.

Reusable workflow for other NXP boards lives in the `nxp-mcuxsdk` skill
(`~/.claude/skills/nxp-mcuxsdk/`).

## usb_dac — the board as a USB Audio Class 2.0 DAC
SDK example `usb_examples/usb_device_audio_speaker/bm`, officially supported for this
board (`example.yml` lists `evkbimxrt1050`). Signal path:

    PC --USB HS--> J9 (USB_OTG1, EHCI0) --> RT1052 --SAI1--> WM8960 --> J12 headphone jack
                                                  \--LPI2C1--> WM8960 control

    ./build.sh usb_dac flash
    aplay -l                  # -> card N: DEMO [USB AUDIO DEMO]
    aplay -D hw:N,0 -f S16_LE -r 48000 -c 2 file.raw

* Enumerates as `1fc9:0098  NXP SEMICONDUCTORS  USB AUDIO DEMO`, high speed.
* **UAC 2.0**, 48 kHz / 16-bit / 2 ch only (`bcdADC 2.00`), asynchronous isochronous
  with an explicit feedback endpoint — the fixed format is what the descriptors in
  `usb_device_descriptor.h` declare, not an ALSA limitation.
* Audio out is the **J12 headphone jack**. The WM8960 speaker outputs (SPK_L/R) go to
  headers J16/J17 instead.
* Firmware console (`/dev/ttyACM0` @115200) prints `Init Audio SAI and CODEC`,
  `USB device audio speaker demo`, and logs each host volume request.

To make it the system output: `pactl set-default-sink $(pactl list short sinks | grep -i demo | cut -f2)`

## usb_dac32 — 48 kHz / 32-bit USB DAC
Out-of-tree copy of the SDK audio-speaker example (sources **and** its board port) with
the sample format widened. The SDK tree is left untouched.

    ./build.sh usb_dac32 flash
    aplay -D hw:1,0 -f S32_LE -r 48000 -c 2 -t raw tone.raw

Three edits versus the stock example:

| file | change |
|---|---|
| `usb_device_descriptor.h` | `AUDIO_FORMAT_BITS` 16→**32**, `AUDIO_FORMAT_SIZE` 2→**4** |
| `usb_device_descriptor.c` | UAC 2.0 Type-I format descriptor had `bSubslotSize`/`bBitResolution` hard-coded as `0x02`/`0x10` — now uses the macros (the UAC 1.0 copy already did) |
| `board_port/hardware_init.c` | `kWM8960_AudioBitWidth16bit`→`32bit`, `kHAL_AudioWordWidth16bits`→`32bits` |

Everything else scales off those macros, including the endpoint packet sizes and the
ring buffers. No clock changes: MCLK stays 12.288 MHz (Audio PLL 786.48 MHz / 4 / 16),
and 12.288 / (48k x 32 x 2) = exactly 4, so the SAI bit-clock divider just halves.

**Bit depth, precisely:** USB carries 32-bit samples; the SAI drives 32-bit I2S words;
the WM8960's interface is set to 32-bit word length (`IFACE1.WL = 0b11`) but its
sigma-delta DAC is **24-bit**, so the low 8 bits are discarded in the codec. That is
exactly the "USB 32-bit / DAC 24-bit" split — no software repacking needed, because the
WM8960 accepts a 32-bit frame natively.

### Verified on hardware
* `bcdADC 2.00`, `bSubslotSize 4`, `bBitResolution 32`, `wMaxPacketSize 56` (48 B of
  audio per 125 us microframe + slack), async iso + feedback endpoint.
* ALSA: `FORMAT: S32_LE`, `SAMPLE_BITS: 32`, `CHANNELS: 2`, `RATE: 48000`.
* SAI1 registers while streaming: `TCR5 = 0x1F1F1F00` (W0W = WNW = 32 bits, FBT = 31),
  `TCR4 = 0x10011F3B` (FRSZ = 2 words, SYWD = 32, MF/FSE/FSP/FSD set = I2S master),
  `TCR2 = 0x07000001` (DIV = 1 -> BCLK = 12.288 MHz / 4 = 3.072 MHz = 48k x 32 x 2).
* Firmware console still reports `Init Audio SAI and CODEC` -> `CODEC_Init` succeeded
  with the 32-bit format (it asserts on failure).

### Hardware volume + mute (WM8960, host-controlled)
The stock SDK example only *prints* the USB volume/mute requests. `usb_dac32` now drives
the codec, so attenuation happens in the WM8960 and never touches the sample data.

* **Volume → `kWM8960_ModuleHP`**, the *analog* headphone amp (`LOUT1VOL`/`ROUT1VOL`):
  register `0x30..0x7F` = **-73 dB .. +6 dB in 1 dB steps**, `0x00..0x2F` mutes, 0 dB =
  `0x79`. Attenuating after the DAC keeps the full 24-bit converter range at every
  setting, which digital attenuation would not.
* **Mute → `kWM8960_ModuleDAC`** (`LDACVOL`/`RDACVOL` → `0x100`). Deliberately not
  `kWM8960_ModuleHP`: `WM8960_SetMute()` on the HP module writes a fixed `0x6F` back
  into the analog volume register and would silently discard the host's setting.
* The feature unit now reports the codec's **real** range —
  `volumeControlRange = {1, 0xB700, 0x0600, 0x0100}` — so the host slider maps 1:1 onto
  codec steps, PulseAudio/PipeWire treats it as a hardware control, and the kernel's
  `Unlikely big volume range` warning is gone.
* Default is 0 dB (was `0x1F00` = +31 dB, outside the achievable range).

New code lives in `board_port/hardware_init.c` (`BOARD_Codec_SetVolume/SetMute`,
declared in `board_port/app.h`), called from `USB_AudioCodecTask()`.

    amixer -c 1 contents                 # 'Playback Volume' 0..79, dBminmax -73..+6 dB
    amixer -c 1 cset numid=3 53          # -20 dB
    amixer -c 1 cset numid=2 off         # mute

Verified by reading the driver's `reg_cache[]` over SWD while changing the host slider:

| host value | LOUT1/ROUT1 | dB | LDAC |
|---|---|---|---|
| 79 | `0x7F` | +6 | `0x1FF` |
| 73 | `0x79` | 0 | `0x1FF` |
| 40 | `0x58` | -33 | `0x1FF` |
| 0 | `0x30` | -73 | `0x1FF` |
| mute on | `0x79` (kept) | 0 | `0x100` |

Console echoes each request, e.g. `Set Cur Volume : ec00` = -20 dB at slider 53.

#### Making the desktop slider actually reach the codec
Adding the hardware control was not enough on its own. The stock descriptor set
`iTerminal = 2` on the USB-streaming **input terminal**, pointing it at the product
string. `snd-usb-audio` prefers a terminal's string descriptor when naming mixer
elements, so the control came up as **`USB AUDIO DEMO Playback Volume`** — a name that
PulseAudio/PipeWire's ACP mixer paths do not match. ACP therefore found no hardware
volume, fell back to software attenuation, and the desktop slider never touched the
WM8960: the codec just sat at whatever it was last set to.

Fix: `iTerminal = 0` in `usb_device_descriptor.c`. The kernel then derives the name from
the terminal type (`0x0101`, USB Streaming) and calls it **`PCM Playback Volume`**, which
ACP does map. Verified:

| desktop volume | ALSA value | codec |
|---|---|---|
| 100% | 79 | +6 dB |
| 75% | 72 | -1 dB |
| 50% | 61 | -12 dB |
| 25% | 43 | -30 dB |
| 0% | 0 | -73 dB |

**Note:** 100% is now +6 dB of analog gain into J12 — genuinely loud on headphones.

## License / attribution
`usb_dac32/` is derived from the MCUXpresso SDK example
`usb_examples/usb_device_audio_speaker/bm` and its `evkbimxrt1050` board port, both
**BSD-3-Clause, Copyright NXP**. The original copyright and SPDX headers are kept in
every file; the modifications are described above. `blinky/` is original work, also
BSD-3-Clause to match.

The MCUXpresso SDK itself is not vendored here — see `.gitignore` for the `west init`
line that recreates the workspace. The NXP board design files are likewise not
redistributed; download `RT1050EVKB-DESIGNFILES` from nxp.com.

## Room correction (FFT convolution)

`Test900.wav` (32768 taps, 96 kHz, float32, stereo) is convolved with the audio stream
in real time on the Cortex-M7.

    .venv/bin/python tools/analyse_ir.py Test900.wav    # what the filter does
    .venv/bin/python tools/make_filter.py               # -> roomcorr_data.c + .bin
    .venv/bin/python tools/verify_filter.py             # check the algorithm on the host
    CFG=flexspi_nor_release ./build.sh usb_dac32 flash

### 96 kHz -> 48 kHz costs no frequency resolution
The WM8960 tops out at 48 kHz (its SYSCLK is <= 12.288 MHz and the rate divider needs
SYSCLK/fs >= 256), so the IR has to be resampled. That is **not** a loss of resolution:

| | source | resampled |
|---|---|---|
| rate | 96 kHz | 48 kHz |
| taps | 32768 | 16384 |
| duration | 341.3 ms | 341.3 ms |
| **frequency resolution** | **2.93 Hz** | **2.93 Hz** |

Resolution is `1 / duration`, not `1 / taps`. Halving the rate halves the tap count
because each tap now spans twice as long; the filter still covers 341 ms, so it still
resolves 2.93 Hz. The only thing discarded is 24-48 kHz, which the codec cannot
reproduce. Measured magnitude error vs the original, 10 Hz-20 kHz: **0.19 dB rms**.

`resample_poly` preserves *signal* amplitude, which for a *filter* means sum(h) -- and
so the whole frequency response -- drops by the decimation factor. `make_filter.py`
multiplies by `dn/up` to compensate; without that the response sat 6.02 dB low.

### What this particular filter does
It is pure attenuation -- peak response -0.25 dB (L) / -1.15 dB (R), no boost anywhere.
The deep cuts are exactly where the reported problem is:

| band | ch0 mean | ch1 mean |
|---|---|---|
| 20-40 Hz | -13.1 dB | -13.2 dB |
| **40-80 Hz** | **-19.3 dB** | **-19.0 dB** |
| 80-160 Hz | -12.2 dB | -12.8 dB |
| 160 Hz-20 kHz | -4 to -9 dB | -6 to -10 dB |

Worst case sample gain is sum|h| = +6.6 dB, so the output clamps and counts clips; in
practice the peak response is ~0 dB and the counter stays at zero.

### Algorithm
Uniform-partitioned overlap-save, stereo, float32, CMSIS-DSP:

    B = 1024 frames, N = 2B = 2048, P = 16 partitions
    X = rfft([previous block | this block]) -> push into a P-deep delay line
    Y = sum(p) FDL[head+p] * FILTER[p]      -> complex MAC in CMSIS packed layout
    y = irfft(Y), keep the second half

Memory (`P * N = 2 * taps` floats per channel, independent of B):

| | size | where |
|---|---|---|
| filter | 256 KiB | rodata, HyperFlash |
| delay line | 256 KiB | SDRAM 0x80000000 |
| FIFOs + scratch | 64 KiB | SDRAM |

The SDRAM is brought up by the boot header's DCD (`XIP_BOOT_HEADER_DCD_ENABLE=1`), which
also requires `SKIP_SYSCLK_INIT` so `BOARD_BootClockRUN()` does not re-init the System
PLL that clocks the SEMC underneath the already-running SDRAM. That same macro makes
`BOARD_ConfigMPU()` mark 0x80000000 Normal cacheable -- essential, since the block loop
streams 512 KiB through it every 21 ms.

The SAI DMA callback runs every 125 us and only moves bytes: it pushes what it took from
the USB ring into an input FIFO and pulls processed audio from an output FIFO. The
convolution itself runs in the main loop. All of the asynchronous-feedback bookkeeping in
`txCallback()` is untouched, so the USB side still sees data consumed at the same rate.

### Measured on hardware
* **Numerically exact.** An impulse through the on-device convolver returns the filter's
  own IR: peak `299,861,248` at frame 2188, against the host reference `299,860,992` at
  frame 2188 -- 0.85 ppm apart, i.e. float32 rounding.
* **24% CPU**: 3,103,290 cycles = 5.17 ms of the 21.33 ms block budget (`-Os`).
  At `-O0` it is 11.2 ms / 52%, so build release for real use.
* Steady state: 0 underruns after priming, 0 clips, output FIFO stable at ~1160 frames.
* Added latency ~100 ms: 45.6 ms of filter pre-delay (the IR peaks at tap 2188) +
  2 blocks (42.7 ms) + USB buffering. High latency was acceptable here by design.

Two traps worth recording. CMSIS's `arm_rfft_fast_f32` forward/inverse pair round-trips
at **unity** -- `stage_rfft_f32` and `merge_rfft_f32` each carry 0.5 factors that cancel
the inverse `1/(N/2)` -- so no compensation is needed; assuming `N/2` overdrove the
output by ~7x. And DWT `CYCCNT` is useless for this: it sits in the debug power domain,
which the RT1050 powers down when the probe detaches, so it silently freezes and every
block then measures 0 cycles. The load counter uses free-running SysTick instead.

### Headroom for a longer filter
CPU scales with the partition count, memory with `2 * taps`. At 24% for 341 ms there is
room for roughly a 1 second IR before the block budget gets tight, and SDRAM (32 MB
against 320 KiB used) is nowhere near a limit. Nothing in the firmware is hard-coded to
this filter -- `make_filter.py` regenerates `roomcorr_params.h` and the geometry follows.

### SW8 toggles the correction, D18 shows it

The board has exactly one free user button and one user LED, and the SDK board header
already names both:

| | net | pin | polarity |
|---|---|---|---|
| **SW8** `USER_BUTTON` | `WAKEUP` | GPIO5_IO00 (SNVS, pin L6) | active low, external pull-up |
| **D18** `USER_LED` | `USER_LED` | GPIO_AD_B0_09 -> GPIO1_IO09 | active low |

Press SW8 to toggle. **D18 lit = correction engaged; dark = bypassed.** The console
echoes `room correction BYPASSED` / `engaged`.

Neither pin is touched by the audio example's `pin_mux.c`, so `roomcorr_ui.c` muxes them
itself rather than editing the generated file. The button is polled from the main loop
with a debounce counter, not interrupt driven -- the loop already turns over every few
milliseconds, which is the right timescale for a button, and it keeps another interrupt
out of the audio path. GPIO_AD_B0_09 doubles as JTAG_TDI, so driving the LED disturbs a
live debug session; that is the board, not the firmware.

**Bypass is a crossfade, and the convolution never stops.** Two reasons: the delay line
has to keep being fed or switching back would play out of a stale FDL, and having the
wet signal always available is what allows a click-free transition. `s_mix` ramps over
one block (21 ms). CPU is therefore the same ~24% in both modes. The two signals are not
phase aligned during the ramp -- the filter carries ~46 ms of its own pre-delay -- so the
crossfade is not coherent, but over one block it reads as a smooth transition rather than
the hard click a bare switch gives.

**Expect bypass to sound louder.** This filter is net attenuation: about -5 to -6 dB
through the midband and up to -20 dB at 40-80 Hz. Bypass is a true bypass, not level
matched, so A/B will favour it on loudness alone. Match levels with the volume control
before judging.

Verified by driving GPIO5_IO00 from the debug probe (`GPIO_PinRead()` reads `DR`, so
setting `GDIR` and clearing `DR` exercises exactly the path the switch does), then
handing the pin back:

    at boot        : LED ON (corrected)
    after press 1  : LED off (bypassed)     console: room correction BYPASSED
    after press 2  : LED ON (corrected)     console: room correction engaged

Still 24-25% CPU, 0 clips, and no new underruns in either mode. Confirm the physical
press yourself -- I could only drive the pin, not push the switch.

## 16-band EQ and preamp

Sixteen peaking biquads per channel (ISO centres 20 Hz .. 20 kHz, Q = 1.4, +/-12 dB) plus
a -40..+12 dB preamp, applied **after** the convolution and **regardless of bypass** --
they are tone and level controls, not part of the calibration.

Deliberately not folded into the convolution filter. Multiplying each partition spectrum
by the EQ response is only equivalent to convolving with it when the EQ's impulse
response fits inside one partition (B = 1024 taps, 21 ms), and a 20 Hz bell decays far
more slowly than that. Sixteen biquads per channel costs ~15 Mflop/s at 48 kHz, nothing
next to the convolution, and it is exact. Coefficients are recomputed on change and take
effect at the next block boundary, since the web handler and the DSP share the main loop.

## Runtime filter store and IR upload

The built-in filter stays in flash as the fallback. An uploaded IR is decoded into one of
two SDRAM slots and swapped in by a single pointer store; `RC_ProcessBlock()` snapshots
that pointer once per block, so a block is never built from half of each filter.

`RC_FILTER_LoadWav()` accepts RIFF/WAVE, PCM 16/24/32-bit or IEEE float32, mono or
stereo, at 48 kHz or 96 kHz. Mono is duplicated to both channels. 96 kHz is decimated by
two on-device through the 127-tap FIR in `roomcorr_resample.h`. An IR longer than
`RC_TAPS` is rejected rather than truncated -- a truncated room correction is a different
filter, not a worse one.

Rebuilding the filter blocks the main loop for a few tens of milliseconds, so audio will
glitch briefly when a new IR is loaded. That is a deliberate trade for much simpler code;
the FIFO underrun path already degrades to silence rather than misbehaving.

### The on-device decimator is exact
`tools/verify_decimator.py` transcribes `decimate_half()` literally -- same taps, same
delay, same x2 -- and compares against the original 96 kHz response:

| | vs original 96 kHz | vs host `make_filter.py` |
|---|---|---|
| ch0 | max **0.001 dB**, rms 0.0002 dB | max 1.154 dB, rms 0.191 dB |
| ch1 | max **0.001 dB**, rms 0.0002 dB | max 1.159 dB, rms 0.190 dB |

The device path reproduces the source response essentially perfectly. The larger figure
against the host tool is the *host tool's* own error -- `resample_poly`'s Kaiser window
rolls off approaching 20 kHz -- so a filter built on the board is slightly more faithful
than one built by the script.

## Host control app (USB)

Ethernet was dropped -- see the note at the end -- and replaced by a native GTK4
application that talks to the board over the **same USB cable the audio uses**.

    cd host && make          # needs libgtk-4-dev and libusb-1.0-0-dev
    ./rcdac-gtk              # control panel
    ./rcdac-cli status       # same thing without a GUI

`host/` contains:

| file | role |
|---|---|
| `rcdac.h` / `rcdac.c` | transport: libusb control transfers, no GUI dependency |
| `rcdac-gtk.c` | GTK4 control panel |
| `rcdac-cli.c` | CLI, useful for scripting and for proving the transport |

### Why control transfers on the audio device
The protocol rides on **vendor control requests to endpoint 0 of the existing UAC
device** (`1fc9:0098`), defined once in `usb_dac32/roomcorr_usbctl.h` and compiled into
both firmware and host app, so the wire format cannot drift.

That choice matters: endpoint 0 needs no interface claim, so the host app never detaches
`snd-usb-audio` and **playback keeps running while you change settings**. No composite
descriptor, no second USB interface, no second cable. The firmware side is one
`kUSB_DeviceEventVendorRequest` case in the existing device callback.

Requests: `STATUS` (IN, returns the whole state), `SET` (bypass / preamp / EQ band),
and `IR_BEGIN` / `IR_DATA` / `IR_COMMIT` for uploads. Install
`/etc/udev/rules.d/60-rt1050-dac.rules` so the app can open the device without root:

    SUBSYSTEM=="usb", ATTR{idVendor}=="1fc9", ATTR{idProduct}=="0098", MODE="0666"

### Level meters and statistics
Four peak meters -- **in L, in R, out L, out R** -- showing dBFS on a -60..0 scale, plus
a **Reset statistics** button that zeros the block, underrun, clip and peak-load
counters.

The device does **peak-hold**, not instantaneous sampling: the host polls every 100 ms
while blocks are produced every 21 ms, so an instantaneous read would simply miss
transients. Reading the status takes the held peak and clears it, so nothing is missed
between polls and nothing is counted twice. Decay ballistics live on the host -- instant
attack, 36 dB/s fall -- which is what makes a meter readable instead of a flicker.

The out meter doubles as an independent check on the DSP. With a -20 dBFS tone:

| | correction engaged | bypassed |
|---|---|---|
| in L/R | -20.0 / -20.0 dBFS | -20.0 / -20.0 |
| out L/R | **-24.9 / -25.9** | **-20.0 / -20.0** |

The engaged figures match the filter's measured midband attenuation of -5 to -6 dB, and
bypass passes through at unity. That is the convolution's gain confirmed end to end by a
path that shares no code with the filter generator.

### Control panel behaviour
* **EQ is 16 vertical faders**, mixer-strip style: value on top, fader, centre frequency
  below. GTK vertical ranges run low-at-top by default, so they are inverted to read the
  way a graphic EQ should.
* **Double-click any fader (or the preamp) to reset it to 0 dB.** The gesture is attached
  in the *capture* phase on purpose: `GtkScale` has its own click and drag gestures, and
  in the bubble phase they claim the sequence first so the second press never arrives.
* Slider labels update **immediately** from the change handler rather than waiting for
  the poll, and the poll itself runs at 200 ms (a status read is about 1 ms).
* At that rate, relying on keyboard focus to decide "don't overwrite this slider" is too
  fragile -- a touchpad drag may not focus the widget -- so any user movement gives that
  slider a 0.7 s grace period during which the poll leaves it alone.

### Verified over USB
* Status read live from the running DAC while audio plays.
* Bypass, preamp and individual EQ bands all apply immediately.
* **262,188-byte IR uploaded in 0.16 s**, decimated 96 -> 48 kHz on the board, accepted
  as `uploaded, 16384 taps @96000 Hz src`, result `ok`.
* 48 kHz and 96 kHz sources both accepted; a non-WAV is rejected with
  `not a RIFF/WAVE file` rather than being loaded as noise.
* Running the GUI for six seconds with no interaction changes **nothing** on the device
  -- checked field by field before and after.
* Clips stayed 0 throughout.

Underruns step once per upload, by about 320. That is not a leak: the rebuild blocks the
main loop for roughly 40 ms, and the SAI callback runs every 125 us, so 40 ms / 125 us is
almost exactly the step. With the board idle the counter is flat.

The rebuild originally ran inside `RC_USBCTL_Handle()`, which executes in the **USB
interrupt** -- tens of milliseconds with interrupts blocked, stalling the SAI DMA
completion that feeds the codec. `RC_REQ_IR_COMMIT` now only raises a flag and
`RC_USBCTL_Task()` does the work from the main loop; the host polls until the result
stops reading "in progress".

DSP load is reported as **current and peak** rather than peak alone, since a one-off
event like a rebuild otherwise pins the peak somewhere unrepresentative forever. The peak
is reseeded from the last block after a filter load. Steady state is 24 % with the
built-in filter and 34 % with an uploaded one -- the built-in lives in XIP flash while an
upload lives in SDRAM, where it contends with the delay line for the 32 KB D-cache.

### D18 works again
With Ethernet gone, `GPIO_AD_B0_09` is no longer needed for `ENET_RST`, so **D18 is back
as the calibration indicator**: lit = correction engaged, dark = bypassed. SW8 still
toggles, and the host app follows along on its 1 s poll.

### Why Ethernet was dropped
The stack was correct -- PHY alive on MDIO (KSZ8081, ID `0022:1561`, addr 2),
autonegotiation enabled, energy detect seeing the router -- but autonegotiation never
completed, and every register I could compare against NXP's working `lwip_dhcp` example
was byte-identical: `PLL_ENET`, `GPR1`, all ENET pad mux/config/daisy registers, ENET
`ECR`/`MSCR`/`RCR`/`TCR`, `CCM_CCGR1`, `GPIO1 GDIR/DR`. The reference example links on
this exact board, cable and router in both debug and release, so the fault was in this
firmware and not the hardware, but it stayed unfound. USB is a better fit anyway: one
cable instead of two, and no dependency on the network.

Two genuine ENET bugs were found and are recorded in the git history:
`BOARD_ENET_PHY_RESET` only writes `DR`, so the reset line floats without `GPIO_PinInit`
setting `GDIR` first; and `GPIO_AD_B0_10` is the KSZ8081's `INTRP/NAND_TREE#` strap,
sampled at reset release, so it needs a pull-up.
