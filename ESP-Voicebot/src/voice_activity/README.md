# On-device WebRTC voice activity detector

The sketch uses the fixed-point WebRTC VAD C core bundled with
[py-webrtcvad 2.0.10](https://github.com/wiseman/py-webrtcvad/tree/bb429dac1a686807c69b916f03dd843fa10b0927),
commit `bb429dac1a686807c69b916f03dd843fa10b0927`. This is the actual spectral
speech/noise classifier with six frequency bands and Gaussian models. No Python
runtime, cloud service, FFT library, or heap allocation is used by the wrapper.

`../../voice_activity.h` exposes:

```cpp
VoiceActivity vad;                 // mode 2, near-silence peak floor 8
bool ok = vad.begin();
bool talking = vad.speech(pcm, 320); // exactly 20 ms of 16 kHz mono PCM16
vad.reset();                       // discard noise/filter/hangover history
```

The constructor optionally takes `(uint8_t mode, uint16_t digitalSilencePeak)`.
Modes 0–3 increase rejection of non-speech; higher modes also miss more quiet
speech. A peak floor of zero disables the optional floor. The default eight
PCM counts are approximately −72 dBFS, so it only suppresses near-digital
silence. Frames still reach the classifier before this floor is applied, to
age its internal state normally. Invalid sizes, null audio, or an uninitialized
instance return `false` without reading PCM. One instance belongs to one capture
task. Its persistent state occupies 744 bytes on the tested native and ESP32-S3
ABIs; processing has no per-frame allocation.

The detector recognizes speech characteristics, including speech from a TV or
another person. It does not identify the intended speaker, remove playback
echo, or guarantee rejection of loud noise. The session controller must suppress
capture while this device speaks and apply onset/end-of-utterance timing. The
classifier can initially label stationary noise as speech while adapting.

## Vendored subset and changes

Ten C files provide the VAD and its required fixed-point DSP routines. The
wrapper calls `WebRtcVad_InitCore`, `WebRtcVad_set_mode_core`, and the 16 kHz
decision function with caller-owned state, avoiding the allocating public C
factory. The 48 kHz resampling helpers remain because the upstream core's reset
and unused entry points reference them; the firmware linker removes unreachable
code. Arduino compiles this sketch's `src` directory recursively.

`upstream-sha256.json` records the archive, exact commit, and original file
hashes. `esp32.patch`, applied inside the copied `cbits` tree with `patch -p1`,
reproduces these local changes:

- Make internal include paths relative so standard Arduino builds need no
  custom include flags.
- Identify Xtensa as a 32-bit little-endian target using the portable C code.
- Correct the minimum-history shifting loop to stop before index 16. This
  matches the subsequent canonical fix in
  [vad_sp.c](https://github.com/wiseman/py-webrtcvad/blob/e283ca41df3a84b0e87fb1f5cb9b21580a286b09/cbits/webrtc/common_audio/vad/vad_sp.c).
- Express four signed left shifts and a variance multiplication with defined
  arithmetic while preserving fixed-point results. The likelihood and variance
  changes correspond to fixes in the
  [later canonical core](https://github.com/wiseman/py-webrtcvad/blob/e283ca41df3a84b0e87fb1f5cb9b21580a286b09/cbits/webrtc/common_audio/vad/vad_core.c).

`LICENSE` retains the complete upstream MIT and WebRTC BSD notices. The C files
are covered by the WebRTC BSD license, not solely the Python wrapper's MIT
license. `PATENTS` and `AUTHORS` preserve the additional notices referenced by
the C copyright headers; they were obtained from the official WebRTC source,
with their exact Git blob and SHA-256 hashes recorded separately in the manifest.

## Validation

The native tests compile these C files and the actual C++ wrapper, including
address/undefined-behavior sanitizers when requested:

```sh
FIRMWARE_SANITIZERS=1 python -m unittest discover -s tests -p test_standalone_voice_activity.py -v
```

The upstream recorded speech fixture reproduces the published decisions for
all four modes. The 16 kHz wrapper is checked with interpolated speech at four
gain levels, moderate stationary noise after adaptation, reset behavior,
invalid frame sizes, near-zero PCM, and full-scale/clipped sample stress.
These checks do not measure microphone accuracy in the user's room.
