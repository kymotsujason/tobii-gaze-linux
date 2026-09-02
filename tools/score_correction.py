#!/usr/bin/env python3
"""Score the host-side gaze correction offline, from the sweeps gaze-cal logs.

This is what makes spec test 5.3 decidable without spending more tracker time.
`gaze-cal accuracy` and `gaze-cal fit` both write a machine-readable block for
every sweep:

    pt 1 target 0.100000 0.100000  gaze 0.088281 0.061806  eye_mm 18.42 70.61 429.10  eproj 0.531198 0.803398  n 18

Given two such sweeps it fits BOTH candidate forms on the first and scores both
on the second:

    form H, head-aware:    corrected = E_proj + (reported - E_proj - b) / g
    form S, static affine: corrected = (reported - b) / g, no eye term at all

The two differ by ((g-1)/g) * (E_apply - E_fit), which on this panel is
0.63 px per mm of head movement IN THE SCREEN PLANE. E_proj has no z term, so a
seating change purely in DEPTH cannot separate them: the discriminating move is
lateral or vertical.

THIS QUESTION HAS BEEN ANSWERED. On 2026-07-27, fitted on fit#9 and scored on
lateral2 at a seat displaced 115 mm, form H gave a 69 px median against form
S's 53, so form S is what gaze-cal ships and form S is what this tool fits.
The comparison is kept because spec 5.6 says to re-open the question once more
sweeps carry per-point eye origins, and because it is the record of how the
decision was made. Do not delete it to simplify the tool.

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

# The measured display area and the gameplay monitor, pinned rather than parsed.
# check_geometry() below refuses a log that was not recorded against them: this
# project has already been bitten once by numbers taken from the wrong monitor,
# and every millimetre and pixel figure printed here depends on these.
AREA_W_MM, AREA_H_MM = 590.42, 333.72
AREA_OX_MM, AREA_OY_MM = -295.21, 5.0
SCREEN_W_PX, SCREEN_H_PX = 2560, 1440

# Half a millimetre of eye position, well inside the sensor's own noise and far
# tighter than any real panel difference.
GEOMETRY_TOL = 0.5 / AREA_W_MM

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


def check_geometry(sweep):
    """Refuse a sweep recorded against a different panel.

    gaze-cal writes both the eye position in tracker mm and the projection it
    derived from the display area, so recomputing the second from the first with
    the constants above is a direct test of whether that area is this one. A
    mismatch means every mm and px figure printed here would be wrong.
    """
    for i in sorted(sweep["pts"]):
        p = sweep["pts"][i]
        want = ((p["eye"][0] - AREA_OX_MM) / AREA_W_MM,
                (AREA_OY_MM + AREA_H_MM - p["eye"][1]) / AREA_H_MM)
        if (abs(want[0] - p["eproj"][0]) > GEOMETRY_TOL or
                abs(want[1] - p["eproj"][1]) > GEOMETRY_TOL):
            sys.exit(
                "sweep %r point %d was recorded against a different display area.\n"
                "  logged eproj    %.6f %.6f\n"
                "  from %.2f x %.2f at (%.2f, %.2f)  %.6f %.6f\n"
                "REFUSING: every millimetre and pixel figure here depends on that "
                "geometry." % (sweep["label"], i, p["eproj"][0], p["eproj"][1],
                               AREA_W_MM, AREA_H_MM, AREA_OX_MM, AREA_OY_MM,
                               want[0], want[1]))


def ols(vs, us, use):
    n = sum(1 for k in use if k)
    if n < 3:
        sys.exit("fewer than three usable points; nothing to fit")
    mv = sum(v for v, k in zip(vs, use) if k) / n
    mu = sum(u for u, k in zip(us, use) if k) / n
    svv = sum((v - mv) ** 2 for v, k in zip(vs, use) if k)
    if svv <= 1e-9:
        sys.exit("the targets do not span an axis; nothing to fit")
    slope = sum((v - mv) * (u - mu) for v, u, k in zip(vs, us, use) if k) / svv
    return slope, mu - slope * mv


def fit(sweep, head_aware=False):
    """The same fit gz_correction_fit performs, including its outlier rule.

    The default is form S, which is what gaze-cal stores: if this tool fitted
    differently from the shipped C, it would answer questions about parameters
    nobody is running. `head_aware=True` fits form H instead, which is only for
    the 5.3 comparison.
    """
    check_geometry(sweep)
    pts = [sweep["pts"][i] for i in sorted(sweep["pts"])]
    if len(pts) != 9:
        sys.exit("%s has %d points; a fit needs all nine (do-not #9)"
                 % (sweep["label"], len(pts)))

    if head_aware:
        v = [[p["target"][a] - p["eproj"][a] for p in pts] for a in (0, 1)]
        u = [[p["gaze"][a] - p["eproj"][a] for p in pts] for a in (0, 1)]
    else:
        v = [[p["target"][a] for p in pts] for a in (0, 1)]
        u = [[p["gaze"][a] for p in pts] for a in (0, 1)]
    use = [True] * len(pts)
    rejected = [False] * len(pts)

    g = b = resid = None
    for second_pass in (False, True):
        g = [0.0, 0.0]
        b = [0.0, 0.0]
        for a in (0, 1):
            g[a], b[a] = ols(v[a], u[a], use)
        if g[0] <= 0 or g[1] <= 0:
            sys.exit("the fit produced a non-positive gain; the sweep is unusable")

        # Corrected-minus-target in mm, not the regression residual: they differ
        # by the factor g, and the C fit rejects on the first.
        resid = []
        for i, p in enumerate(pts):
            cor = correct(g, b, p["eproj"] if head_aware else (0.0, 0.0), p["gaze"])
            resid.append(math.hypot((cor[0] - p["target"][0]) * AREA_W_MM,
                                    (cor[1] - p["target"][1]) * AREA_H_MM))
        if second_pass:
            # The post-refit guard, mirroring gz_correction_fit. The second pass
            # only runs when the first dropped a point, so a sweep that rejected
            # nothing never reaches this. When one did go, a survivor still past
            # 3x the REFIT median is the second bad point its own axis absorbed,
            # which neither the envelope nor the isotropy bound can see.
            kept = sorted(r for r, k in zip(resid, use) if k)
            med2 = kept[len(kept) // 2] if len(kept) % 2 else \
                (kept[len(kept) // 2 - 1] + kept[len(kept) // 2]) / 2
            if med2 > 1e-6:     # same floor, so a near-perfect refit is safe
                past = [i for i, (r, k) in enumerate(zip(resid, use))
                        if k and r > 3.0 * med2]
                if past:
                    sys.exit(
                        "%s: point %d still sits past 3x the median residual after "
                        "the refit (%.1f mm against a %.1f mm median). gaze-cal fit "
                        "would have REFUSED this sweep, so there is nothing here to "
                        "score against. Re-run the sweep."
                        % (sweep["label"], past[0] + 1, resid[past[0]], med2))
            break

        kept = sorted(r for r, k in zip(resid, use) if k)
        med = kept[len(kept) // 2] if len(kept) % 2 else \
            (kept[len(kept) // 2 - 1] + kept[len(kept) // 2]) / 2
        if med <= 1e-6:
            break                       # a perfect fit must not reject itself
        drops = [i for i, (r, k) in enumerate(zip(resid, use)) if k and r > 3.0 * med]
        if not drops:
            break
        if len(drops) > 1:
            sys.exit("%s: %d points sit past 3x the median residual. gaze-cal fit "
                     "would have refused this sweep outright, so there is nothing "
                     "here to score against." % (sweep["label"], len(drops)))
        use[drops[0]] = False
        rejected[drops[0]] = True
        # First-pass residuals, which is what the rule is applied to. gaze-cal
        # prints the post-refit numbers instead, so the two differ in the
        # message while agreeing exactly on which point goes and on the gains
        # that come back.
        print("  rejected point %d as an outlier (%.1f mm against a %.1f mm median"
              " before the refit), refitting" % (drops[0] + 1, resid[drops[0]], med))

    # Survivors only, which is what fit_summary averages at calibrate.c:471-476.
    # The two disagree whenever a point was rejected, and one recorded sweep
    # does reject one.
    nk = sum(1 for k in use if k)
    ep = tuple(sum(p["eproj"][a] for p, k in zip(pts, use) if k) / nk
               for a in (0, 1))
    return tuple(g), tuple(b), ep, rejected, resid, use


def correct(g, b, eproj, reported):
    """(r + E(g-1) - b)/g. E is (0,0) for form S, which reduces it to (r-b)/g."""
    return tuple((reported[a] + eproj[a] * (g[a] - 1.0) - b[a]) / g[a] for a in (0, 1))


def score(sweep, gh, bh, gs, bs):
    """Per-point pixel errors: raw, form H with its own fit, form S with its."""
    rows = []
    for i in sorted(sweep["pts"]):
        p = sweep["pts"][i]
        ch = correct(gh, bh, p["eproj"], p["gaze"])        # head-aware
        cs = correct(gs, bs, (0.0, 0.0), p["gaze"])        # no eye term
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
    print("fitted on   %-24s %s" % (fs["label"], fs["when"]))
    print("scored on   %-24s %s" % (ss["label"], ss["when"]))
    g, b, ep_fit, rejected, fit_resid, used = fit(fs)          # form S, the shipped one
    gh, bh, _, _, _, _ = fit(fs, head_aware=True)              # form H, for 5.3 only
    check_geometry(ss)

    print("\nform S, as shipped:")
    print("  gx %.5f  gy %.5f  bx %+.6f  by %+.6f  isotropy %.4f"
          % (g[0], g[1], b[0], b[1], g[0] / g[1]))
    kept_resid = [r for r, k in zip(fit_resid, used) if k]
    print("points used %d of 9%s, median residual on the fit sweep %.0f px" % (
        sum(1 for k in used if k),
        "" if not any(rejected) else
        ", rejected " + ", ".join(str(i + 1) for i, r in enumerate(rejected) if r),
        median(kept_resid) / AREA_W_MM * SCREEN_W_PX))
    # The same envelope gz_correction_check enforces. A tool that scored an
    # out-of-range fit would be answering a question the CLI never asked.
    if not (1.05 <= g[0] <= 1.30 and 1.05 <= g[1] <= 1.30
            and abs(g[0] / g[1] - 1.0) < 0.05):
        print("  WARNING: this fit is outside the measured envelope"
              " (1.05..1.30, isotropy 5 percent).")
        print("  gaze-cal fit would have REFUSED it, so correction.conf does not"
              " hold these numbers.")
    print("fit-time eye projection  %.4f %.4f" % ep_fit)

    rows = score(ss, gh, bh, g, b)
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

    print("\nverdict, against spec 5.3. It was DECIDED for form S on 2026-07-27;")
    print("this rerun is a check, not an open question:")
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
        print("  Form H would win here, which CONTRADICTS the 2026-07-27 result")
        print("  (H 69 px against S 53 at a 115 mm displacement). Do not switch on")
        print("  one sweep: reproduce it at a second displaced seat first.")
    else:
        print("  Form H beats form S but sits above 60 px. The term is earning its")
        print("  place, but something else is degraded. Check the eye origin is")
        print("  read per frame rather than held from the first sample.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
