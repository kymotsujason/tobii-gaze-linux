#!/usr/bin/env python3
"""Score the host-side gaze correction offline, from the sweeps gaze-cal logs.

This is what makes spec test 5.3 decidable without spending more tracker time.
`gaze-cal accuracy` and `gaze-cal fit` both write a machine-readable block for
every sweep:

    pt 1 target 0.100000 0.100000  gaze 0.088281 0.061806  eye_mm 18.42 70.61 429.10  eproj 0.531198 0.803398  n 18

Given two such sweeps it fits form H on the first and scores BOTH candidate
forms on the second:

    form H, head-aware:    corrected = E_proj + (reported - E_proj - b) / g
    form S, static affine: corrected = (reported - b') / g, b' frozen at the
                           fit position, which is the same arithmetic with
                           E_proj held at its fit-time value

The two differ by ((g-1)/g) * (E_apply - E_fit), which on this panel is
0.63 px per mm of head movement IN THE SCREEN PLANE. E_proj has no z term, so a
seating change purely in DEPTH cannot separate them: the discriminating move is
lateral or vertical.

Usage:
    tools/score_correction.py                       list the sweeps in the log
    tools/score_correction.py FIT_LABEL SCORE_LABEL
    tools/score_correction.py --log PATH ...

A label may be suffixed with #N to pick the Nth sweep carrying it, counting
from 1, because the log repeats labels.
"""

import argparse
import math
import re
import sys

DEFAULT_LOG = "~/.local/share/tobii-gaze/gaze-cal.log"

# The measured display area and the gameplay monitor. Both are pinned rather
# than parsed so a log written against a different geometry cannot be scored
# against these numbers by accident; the checks below refuse instead.
AREA_W_MM, AREA_H_MM = 590.42, 333.72
AREA_OX_MM, AREA_OY_MM = -295.21, 5.0
SCREEN_W_PX, SCREEN_H_PX = 2560, 1440

HDR = re.compile(r"^===== (\S+)(?: label=(\S+))?\s+(\S+ \S+) =====")
PT = re.compile(
    r"^\s*pt (\d) target ([-\d.]+) ([-\d.]+)\s+gaze ([-\d.]+) ([-\d.]+)"
    r"\s+eye_mm ([-\d.]+) ([-\d.]+) ([-\d.]+)\s+eproj ([-\d.]+) ([-\d.]+)\s+n (\d+)"
)


def parse_log(path):
    sweeps = []
    cur = None
    with open(path) as f:
        for line in f:
            m = HDR.match(line)
            if m:
                what, label, when = m.group(1), m.group(2), m.group(3)
                cur = {"what": what, "label": label or what, "when": when, "pts": {}}
                sweeps.append(cur)
                continue
            if cur is None:
                continue
            m = PT.match(line)
            if m:
                g = m.groups()
                cur["pts"][int(g[0])] = {
                    "target": (float(g[1]), float(g[2])),
                    "gaze": (float(g[3]), float(g[4])),
                    "eye": (float(g[5]), float(g[6]), float(g[7])),
                    "eproj": (float(g[8]), float(g[9])),
                    "n": int(g[10]),
                }
    return [s for s in sweeps if s["pts"]]


def pick(sweeps, spec):
    label, _, nth = spec.partition("#")
    want = int(nth) if nth else None
    hits = [s for s in sweeps if s["label"] == label]
    if not hits:
        sys.exit("no sweep labelled %r in the log" % label)
    if want is None:
        if len(hits) > 1:
            sys.exit("%d sweeps are labelled %r; disambiguate with %s#N"
                     % (len(hits), label, label))
        return hits[0]
    if not 1 <= want <= len(hits):
        sys.exit("%r has %d sweeps, asked for #%d" % (label, len(hits), want))
    return hits[want - 1]


def ols(vs, us):
    n = len(vs)
    mv, mu = sum(vs) / n, sum(us) / n
    svv = sum((v - mv) ** 2 for v in vs)
    if svv <= 1e-9:
        sys.exit("the targets do not span an axis; nothing to fit")
    slope = sum((v - mv) * (u - mu) for v, u in zip(vs, us)) / svv
    return slope, mu - slope * mv


def fit(sweep):
    """Two univariate OLS of (reported - E_proj) on (target - E_proj)."""
    pts = [sweep["pts"][i] for i in sorted(sweep["pts"])]
    if len(pts) != 9:
        sys.exit("%s has %d points; a fit needs all nine (do-not #9)"
                 % (sweep["label"], len(pts)))
    out = {}
    for axis in (0, 1):
        vs = [p["target"][axis] - p["eproj"][axis] for p in pts]
        us = [p["gaze"][axis] - p["eproj"][axis] for p in pts]
        out["g" if axis == 0 else "gy"], out["b" if axis == 0 else "by"] = ols(vs, us)
    g = (out["g"], out["gy"])
    b = (out["b"], out["by"])
    ep = tuple(sum(p["eproj"][a] for p in pts) / len(pts) for a in (0, 1))
    return g, b, ep


def correct(g, b, eproj, reported):
    """corrected = E + (r - E - b)/g, spelled so g == 1 and b == 0 is exact."""
    return tuple((reported[a] + eproj[a] * (g[a] - 1.0) - b[a]) / g[a] for a in (0, 1))


def score(sweep, g, b, eproj_fixed):
    """Returns per-point pixel errors for form H and for form S."""
    rows = []
    for i in sorted(sweep["pts"]):
        p = sweep["pts"][i]
        ch = correct(g, b, p["eproj"], p["gaze"])          # head-aware
        cs = correct(g, b, eproj_fixed, p["gaze"])         # eye frozen at fit time
        eh = math.hypot((ch[0] - p["target"][0]) * SCREEN_W_PX,
                        (ch[1] - p["target"][1]) * SCREEN_H_PX)
        es = math.hypot((cs[0] - p["target"][0]) * SCREEN_W_PX,
                        (cs[1] - p["target"][1]) * SCREEN_H_PX)
        raw = math.hypot((p["gaze"][0] - p["target"][0]) * SCREEN_W_PX,
                         (p["gaze"][1] - p["target"][1]) * SCREEN_H_PX)
        rows.append((i, raw, eh, es, p["eproj"]))
    return rows


def median(v):
    v = sorted(v)
    n = len(v)
    return v[n // 2] if n % 2 else (v[n // 2 - 1] + v[n // 2]) / 2


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("fit_label", nargs="?")
    ap.add_argument("score_label", nargs="?")
    ap.add_argument("--log", default=DEFAULT_LOG)
    args = ap.parse_args()

    import os
    path = os.path.expanduser(args.log)
    sweeps = parse_log(path)
    if not sweeps:
        sys.exit("no sweep in %s carries the machine-readable `pt` lines.\n"
                 "Those arrive with the task 13b build; older sweeps recorded\n"
                 "only the rounded pixel table and no eye position at all." % path)

    if not args.fit_label:
        print("sweeps carrying per-point eye origins in %s:\n" % path)
        seen = {}
        for s in sweeps:
            seen[s["label"]] = seen.get(s["label"], 0) + 1
            print("  %-24s %s  %d points  (%s#%d)"
                  % (s["label"], s["when"], len(s["pts"]), s["label"], seen[s["label"]]))
        return 0

    if not args.score_label:
        sys.exit("give both a sweep to fit on and a sweep to score on; "
                 "fitting and scoring on the same sweep measures nothing")

    fs, ss = pick(sweeps, args.fit_label), pick(sweeps, args.score_label)
    g, b, ep_fit = fit(fs)

    print("fitted on   %-24s %s" % (fs["label"], fs["when"]))
    print("scored on   %-24s %s" % (ss["label"], ss["when"]))
    print("\ngx %.5f  gy %.5f  bx %+.6f  by %+.6f  isotropy %.4f"
          % (g[0], g[1], b[0], b[1], g[0] / g[1]))
    print("fit-time eye projection  %.4f %.4f" % ep_fit)

    rows = score(ss, g, b, ep_fit)
    eps = [r[4] for r in rows]
    ep_score = (sum(e[0] for e in eps) / len(eps), sum(e[1] for e in eps) / len(eps))
    dx_mm = (ep_score[0] - ep_fit[0]) * AREA_W_MM
    dy_mm = (ep_score[1] - ep_fit[1]) * AREA_H_MM
    print("score-time eye projection %.4f %.4f  -> moved %+.1f, %+.1f mm in the "
          "screen plane" % (ep_score[0], ep_score[1], dx_mm, -dy_mm))

    gm = (g[0] + g[1]) / 2
    sep = (gm - 1) / gm * math.hypot(dx_mm * SCREEN_W_PX / AREA_W_MM,
                                     dy_mm * SCREEN_H_PX / AREA_H_MM)
    print("predicted H-minus-S separation %.0f px  (%.3f px per mm of in-plane "
          "movement)" % (sep, (gm - 1) / gm * SCREEN_W_PX / AREA_W_MM))

    print("\n  pt      raw px    form H px    form S px")
    for i, raw, eh, es, _ in rows:
        print("  %2d %10.0f %12.0f %12.0f" % (i, raw, eh, es))

    mh = median([r[2] for r in rows])
    ms = median([r[3] for r in rows])
    mr = median([r[1] for r in rows])
    print("\nmedian   raw %.0f px   form H %.0f px   form S %.0f px" % (mr, mh, ms))
    print("worst    raw %.0f px   form H %.0f px   form S %.0f px"
          % (max(r[1] for r in rows), max(r[2] for r in rows), max(r[3] for r in rows)))

    print("\nverdict, against spec 5.3:")
    if sep < 20:
        print("  INCONCLUSIVE. The head moved %.0f mm in the screen plane, which"
              % math.hypot(dx_mm, dy_mm))
        print("  separates the two forms by only %.0f px, inside the noise. A"
              % sep)
        print("  change in DEPTH does not count: E_proj has no z term. Re-run with")
        print("  a clearly LATERAL or VERTICAL shift, 250 mm or more.")
    elif mh > 100 and ms > 100:
        print("  BOTH ABOVE 100 px. The gain itself does not transfer between")
        print("  positions. Go to spec 5.6.")
    elif mh > ms:
        print("  FORM S WINS. The scale centre is not the eye: section 1.3a's risk")
        print("  is real. Ship form S and go to spec 5.6.")
    elif mh < 60:
        print("  HEAD-AWARE CONFIRMED. Ship form H.")
    else:
        print("  Form H beats form S but sits above 60 px. The term is earning its")
        print("  place, but something else is degraded. Check the eye origin is")
        print("  read per frame rather than held from the first sample.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
