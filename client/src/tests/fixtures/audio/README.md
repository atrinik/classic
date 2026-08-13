# Client audio fixture

`opus-tone.mid` is 120 milliseconds of original, generated stereo sine-wave
PCM (440 Hz left, 660 Hz right) encoded as Ogg Opus. Its legacy `.mid` name is
intentional: the test proves that SDL3_mixer detects the payload by content
rather than extension. The fixture contains no third-party sound material.

`generate_opus_tone.py` is the canonical PCM generator: it writes 5,760
little-endian signed 16-bit stereo frames at 48 kHz, rounding sine samples to
the nearest integer with amplitude 12,000. Regenerate and verify the encoded
fixture with opus-tools 0.2 (libopus 1.6.1):

```sh
python3 src/tests/fixtures/audio/generate_opus_tone.py /tmp/opus-tone.wav
opusenc --bitrate 32 --hard-cbr --framesize 20 --serial 44 --padding 0 \
  --discard-comments /tmp/opus-tone.wav src/tests/fixtures/audio/opus-tone.mid
sha256sum --check src/tests/fixtures/audio/opus-tone.sha256
```

The fixed Ogg serial, zero padding, and discarded input comments make the
encoder output reproducible for the pinned toolchain. The sibling checksum
file is also consumed by CTest so unexpected fixture changes fail validation.

`malformed.mid` is intentionally non-audio text used to exercise decoder
failure behavior.
