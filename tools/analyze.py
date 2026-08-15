"""Measure per-hand keystroke loss from a stickwatch log.

The 2026-07-31 capture showed the errors are RIGHT-hand specific, which points at the
RIGHT half -> LEFT half wireless relay. That link is wireless in EVERY host mode, so
it is not exonerated by "it happens on wired USB too".

METHOD -- deliberately alignment-free. Diffing captured text against a reference drifts
badly on repeated input (a merged word shifts everything after it and the error
cascades; three attempts reported ~30% loss on a capture that was visibly mostly fine).
Instead, type a drill where every key should occur the SAME NUMBER OF TIMES. Then the
counts alone reveal the loss, with no alignment and no reference length:

    asdf asdf asdf ...   -> a,s,d,f must be equal   (all LEFT hand)
    jkl jkl jkl ...      -> j,k,l must be equal     (all RIGHT hand)
    ajskdlf ajskdlf ...  -> all six equal           (alternating hands)

The alternating drill is the decisive one: left and right keys are interleaved in the
same stream, at the same speed, so any asymmetry is the relay and nothing else.

    python analyze.py stickwatch.log
"""
import re
import sys
from collections import Counter

LEFT = set("qwertasdfgzxcvb")
RIGHT = set("yuiophjklnm")


def load(path):
    downs = []
    for line in open(path):
        m = re.match(r"\s*([\d.]+)\s+DOWN\s+(\S+)", line)
        if m:
            downs.append((float(m.group(1)), m.group(2)))
    return downs


def to_text(downs):
    out = []
    for _, v in downs:
        if len(v) == 1:
            out.append(v.lower())
        elif v == "vk0x20":
            out.append(" ")
        elif v == "vk0x08" and out:
            out.pop()
    return "".join(out)


def hand(c):
    return "LEFT" if c in LEFT else "RIGHT" if c in RIGHT else "other"


PANGRAM = "the quick brown fox jumps over the lazy dog"


def score_pangram(text):
    """Per-hand loss for repeated-pangram input.

    Aligns ONE SENTENCE AT A TIME against a clean copy. Resyncing at every
    sentence is what makes this work -- a single long diff over the whole capture
    drifts out of phase after the first merged word and reports ~30% loss on input
    that is visibly mostly correct.
    """
    import difflib
    starts = [m.start() for m in re.finditer(r"t?h?e? ?q", text)]
    cuts, last = [], -10 ** 9
    for s in starts:
        if s - last > len(PANGRAM) * 0.5:
            cuts.append(s)
            last = s
    if not cuts:
        cuts = [0]
    sents = [text[a:b].strip() for a, b in zip(cuts, cuts[1:] + [len(text)])]

    lost, typed = Counter(), Counter()
    for s in sents:
        if len(s) < len(PANGRAM) * 0.5:      # partial trailing sentence
            continue
        for c in s:
            if c.strip():
                typed[c] += 1
        sm = difflib.SequenceMatcher(None, PANGRAM, s, autojunk=False)
        for tag, i1, i2, j1, j2 in sm.get_opcodes():
            if tag in ("delete", "replace"):
                a, b = PANGRAM[i1:i2], s[j1:j2]
                for c in a:
                    if c.strip() and c not in b:
                        lost[c] += 1
    return len(sents), typed, lost


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else "stickwatch.log"
    downs = load(path)
    if not downs:
        print("no keystrokes in log")
        return 1
    text = to_text(downs)

    if "quick" in text or "--pangram" in sys.argv:
        n, typed, lost = score_pangram(text)
        rate = len([c for c in text if c.isalpha()]) / max(
            downs[-1][0] - downs[0][0], 1e-9)
        print(f"pangram mode: {n} sentences, {rate:.1f} keys/s\n")
        agg = {"LEFT": [0, 0], "RIGHT": [0, 0]}
        for c, n_typed in typed.items():
            h = hand(c)
            if h in agg:
                agg[h][0] += n_typed
                agg[h][1] += lost.get(c, 0)
        for h in ("LEFT", "RIGHT"):
            got, l = agg[h]
            print(f"  {h:>5} half: {l:4d} lost / {got + l:4d} expected "
                  f"= {100 * l / max(got + l, 1):5.1f}% loss")
        gl = agg["LEFT"][1] / max(sum(agg["LEFT"]), 1)
        gr = agg["RIGHT"][1] / max(sum(agg["RIGHT"]), 1)
        if gl > 0:
            print(f"\n  right/left loss ratio: {gr / gl:.1f}x")
        print(f"\n  worst keys: {lost.most_common(8)}")
        if rate < 5.0:
            print(f"\n!! only {rate:.1f} keys/s -- too slow to provoke the fault")
        return 0

    letters = [c for c in text if c.isalpha()]
    counts = Counter(letters)

    if not counts:
        print("no letters captured")
        return 1

    # Drop stray keys: a single mistyped letter would otherwise look like ~100%
    # loss of a drill key and swamp the totals (one stray 'g' turned 8% into 26%).
    top = max(counts.values())
    strays = {c: n for c, n in counts.items() if n < top * 0.25}
    for c in strays:
        del counts[c]
    expected = max(counts.values())

    rate = len(letters) / max(downs[-1][0] - downs[0][0], 1e-9)
    gaps = [downs[i + 1][0] - downs[i][0] for i in range(len(downs) - 1)]
    fast = sum(1 for g in gaps if g < 0.060)
    if rate < 5.0:
        print(f"!! TYPED TOO SLOWLY: {rate:.1f} keys/s, only {fast} overlapping "
              f"digraphs (<60ms).\n   The fault clusters on fast overlapping keys; "
              f"at this speed it may simply not\n   occur, so a clean result here "
              f"proves nothing. Redo at full speed.\n")
    if strays:
        print(f"(ignored stray keys, not part of the drill: "
              f"{ {c: n for c, n in strays.items()} })\n")

    print(f"received {len(letters)} letters over "
          f"{downs[-1][0] - downs[0][0]:.0f}s\n")
    print(f"  {'key':>4}  {'hand':>5}  {'got':>5}  {'lost':>5}   loss")
    agg = {"LEFT": [0, 0], "RIGHT": [0, 0]}
    for c, n in sorted(counts.items()):
        lost = expected - n
        h = hand(c)
        if h in agg:
            agg[h][0] += n
            agg[h][1] += lost
        print(f"  {c:>4}  {h:>5}  {n:5d}  {lost:5d}   {100 * lost / expected:5.1f}%")

    print()
    for h in ("LEFT", "RIGHT"):
        got, lost = agg[h]
        tot = got + lost
        if tot:
            print(f"  {h:>5} half: {lost:4d} lost / {tot:4d} expected "
                  f"= {100 * lost / tot:5.1f}% loss")
    gl = agg["LEFT"][1] / max(sum(agg["LEFT"]), 1)
    gr = agg["RIGHT"][1] / max(sum(agg["RIGHT"]), 1)
    if gl > 0 and gr > 0:
        print(f"\n  right/left loss ratio: {gr / gl:.1f}x")
    elif gr > 0:
        print("\n  RIGHT half lost keys; LEFT half lost none")
    elif gl > 0:
        print("\n  LEFT half lost keys; RIGHT half lost none")
    else:
        print("\n  no loss detected in this capture")
    return 0


if __name__ == "__main__":
    sys.exit(main())
