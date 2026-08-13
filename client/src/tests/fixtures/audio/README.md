# Client audio fixture

`opus-tone.mid` is 120 milliseconds of original, generated stereo sine-wave
PCM (440 Hz left, 660 Hz right) encoded as Ogg Opus. Its legacy `.mid` name is
intentional: the test proves that SDL3_mixer detects the payload by content
rather than extension. The fixture contains no third-party sound material.

It was generated with opus-tools 0.2 using a fixed Ogg serial number, no
padding or comments, 48 kHz stereo input, 32 kbit/s hard CBR, and 20 ms frames.
Its SHA-256 is
`46cc62c986d0b7859211d1b5a3100b4352f2c24a274e2928caeca49b75f76271`.

`malformed.mid` is intentionally non-audio text used to exercise decoder
failure behavior.
