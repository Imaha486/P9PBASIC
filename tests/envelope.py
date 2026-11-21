#!/usr/bin/env python3
"""Compare the audio phase landmarks of a genuine-machine capture
(QUASI88 sdlaudio.raw, 44.1 kHz stereo s16le) with a vessel embed
capture (embed_audio.wav). Sample-exact comparison is impossible --
the FM cores and gain staging differ -- so this compares the
*structure*: a loud onset (the loading BGM), a long loud span, a
quiet gap of >= 4 s, then loud attract songs again. That is the
order the genuine machine produces and the order the 2026-11-18
disk-image regression broke.

Usage: envelope.py GENUINE.raw VESSEL.wav
Exit 0 = same landmark structure, 1 = mismatch (table printed).
"""
import struct
import sys
import wave

LOUD = 3000


def peaks_raw(path):
    raw = open(path, "rb").read()
    n = len(raw) // 2
    samples = struct.unpack("<%dh" % n, raw[:n * 2])
    sr = 44100
    frames = n // 2
    out = []
    for s in range(frames // sr):
        seg = samples[s * sr * 2:(s + 1) * sr * 2]
        out.append(max((abs(x) for x in seg), default=0))
    return out


def peaks_wav(path):
    w = wave.open(path, "rb")
    n, sr, ch = w.getnframes(), w.getframerate(), w.getnchannels()
    raw = w.readframes(n)
    w.close()
    samples = struct.unpack("<%dh" % (len(raw) // 2), raw)
    out = []
    for s in range(n // sr):
        seg = samples[s * sr * ch:(s + 1) * sr * ch]
        out.append(max((abs(x) for x in seg), default=0))
    return out


def landmarks(p):
    """(onset, bgm_loud_seconds, gap_start, gap_len, attract_loud_seconds)"""
    onset = next((i for i, v in enumerate(p) if v >= LOUD), None)
    if onset is None:
        return None
    # first run of >= 4 quiet seconds after onset+5 = the gap
    gap = None
    for i in range(onset + 5, len(p) - 4):
        if all(v < LOUD for v in p[i:i + 4]):
            gap = i
            break
    if gap is None:
        return (onset, sum(1 for v in p[onset:] if v >= LOUD), None, 0, 0)
    gl = 0
    while gap + gl < len(p) and p[gap + gl] < LOUD:
        gl += 1
    bgm = sum(1 for v in p[onset:gap] if v >= LOUD)
    att = sum(1 for v in p[gap + gl:] if v >= LOUD)
    return (onset, bgm, gap, gl, att)


def main():
    g = landmarks(peaks_raw(sys.argv[1]))
    v = landmarks(peaks_wav(sys.argv[2]))
    print("landmarks:            onset  bgm_loud  gap_at  gap_len  attract_loud")
    print("genuine (QUASI88):   ", g)
    print("vessel (embed):      ", v)
    if g is None or v is None:
        print("FAIL: one side never gets loud")
        return 1
    fails = []
    if g[2] is None or v[2] is None:
        fails.append("one side has no quiet gap after the BGM")
    else:
        if v[1] < 15:
            fails.append("vessel BGM span too short: %d loud s" % v[1])
        if v[4] < 10:
            fails.append("vessel attract too short: %d loud s" % v[4])
        if abs(v[0] - g[0]) > 8:
            fails.append("onsets differ by %d s" % abs(v[0] - g[0]))
    if fails:
        for f in fails:
            print("FAIL:", f)
        return 1
    print("OK: landmark structure matches")
    return 0


if __name__ == "__main__":
    sys.exit(main())
