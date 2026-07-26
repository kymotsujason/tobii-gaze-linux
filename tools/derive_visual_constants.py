#!/usr/bin/env python3
"""Normative derivation of the gaze overlay's visual constants.

This script IS the provenance for section 9 of
docs/superpowers/specs/2026-07-26-tobii-obs-overlay-design.md.
Every number in that section is printed below. If you change the design,
change this script and re-run it; if they disagree, the script wins.

Supersedes the scratch files rederive.py / rederive2.py, which scaled the
tail's ring radius per tap. That was the defect that filled the clear centre,
and three independent reviewers caught the resulting contradiction.

Implements exactly the combine the fragment shader will implement:
    rgb = max over contributing taps of the tap's UNPREMULTIPLIED linear colour
    a   = 1 - product over taps of (1 - a_i)
    a  *= exclusion(distance from CURRENT gaze)      <-- applied to TOTAL alpha
    emit float4(rgb * a, a)                          <-- premultiplied
"""
import math
import numpy as np

# ---- geometry, in canvas px at 2560x1440 -----------------------------------
R_C = 240.0            # annulus ring centre
HALF = 110.0           # annulus half-width; ring spans R_C-HALF .. R_C+HALF
PEAK = 0.35            # PROVISIONAL. Must be re-picked against a real OBS render.
TAIL_TAPS = 10
TAIL_MS = 240.0
HIT_R = 118.0          # osu hit circle radius
TRACK_ERR = 45.0       # 1 degree of ET5 error at 45 px/deg
EXCL_R = HIT_R + TRACK_ERR      # 163 px
EXCL_SOFT = 40.0
PX_PER_DEG = 45.0

# tail tapers in WEIGHT ONLY; ring geometry is identical for every tap
TAP_W = [0.15 + 0.30 * (k / TAIL_TAPS) for k in range(TAIL_TAPS)] + [1.0]

IRIS_HALO_S = (116, 61, 255)    # hsl(257,100%,62%)
IRIS_MID_S = (100, 52, 240)     # hsl(257,100%,58%)
BG_S = (20, 23, 31)             # dim osu background


def srgb_to_linear(c):
    c = np.asarray(c, dtype=np.float64) / 255.0
    return np.where(c <= 0.04045, c / 12.92, ((c + 0.055) / 1.055) ** 2.4)


def linear_to_srgb(c):
    c = np.clip(np.asarray(c, dtype=np.float64), 0.0, 1.0)
    return np.where(c <= 0.0031308, c * 12.92, 1.055 * c ** (1 / 2.4) - 0.055) * 255.0


IRIS_HALO_L, IRIS_MID_L, BG_L = map(srgb_to_linear, (IRIS_HALO_S, IRIS_MID_S, BG_S))


def ring(d):
    """Smooth annulus bump, zero outside R_C +/- HALF. Same geometry every tap."""
    t = (d - R_C) / HALF
    w = np.clip(1.0 - t * t, 0.0, 1.0)
    return w * w


def exclusion(d):
    """0 inside EXCL_R, ramps to 1 by EXCL_R + EXCL_SOFT. Applied to TOTAL alpha."""
    return np.clip((d - EXCL_R) / EXCL_SOFT, 0.0, 1.0)


def field(tap_offsets, grid=520, step=2.0):
    """Alpha and unpremultiplied linear rgb over a square around the current gaze.

    tap_offsets: (dx, dy) of each tap relative to CURRENT gaze, oldest first,
    len == len(TAP_W). The last entry must be (0,0), the current sample.
    """
    n = int(grid / step)
    ax = np.arange(-n, n + 1) * step
    gy, gx = np.meshgrid(ax, ax, indexing="ij")
    d_cur = np.hypot(gx, gy)

    one_minus = np.ones_like(d_cur)
    rgb_max = np.zeros(d_cur.shape + (3,))
    for (dx, dy), w in zip(tap_offsets, TAP_W):
        d = np.hypot(gx - dx, gy - dy)
        prof = ring(d)
        col = IRIS_HALO_L[None, None, :] * prof[..., None] + IRIS_MID_L[None, None, :] * (1 - prof[..., None])
        rgb_max = np.maximum(rgb_max, col)
        one_minus *= (1.0 - PEAK * w * prof)
    alpha = (1.0 - one_minus) * exclusion(d_cur)
    return alpha, rgb_max, d_cur


def stationary():
    return [(0.0, 0.0)] * len(TAP_W)


def saccade(jump):
    """All history at the old position, current sample at the new one."""
    return [(-float(jump), 0.0)] * TAIL_TAPS + [(0.0, 0.0)]


def report():
    print("=" * 74)
    print("VISUAL CONSTANTS  (annulus r_c=%.0f half=%.0f peak=%.2f, excl=%.0f+%.0f)"
          % (R_C, HALF, PEAK, EXCL_R, EXCL_SOFT))
    print("=" * 74)

    a, rgb, d = field(stationary())
    disc = d <= HIT_R
    outside = (d > HIT_R) & (d < 500)
    print("\nHeld fixation:")
    print("  mean alpha over the %.0f px hit circle : %.3f" % (HIT_R, a[disc].mean()))
    print("  max  alpha over the hit circle        : %.3f" % a[disc].max())
    print("  halo peak outside the hit circle      : %.2f" % a[outside].max())

    clear = 0
    for r in np.arange(0, 400, 2.0):
        if a[d <= r].max() >= 0.05:
            break
        clear = r
    print("  clear-centre radius (alpha < 0.05)    : %.0f px" % clear)
    outer = d[a > 0.02].max()
    print("  outer extent (alpha > 0.02)           : %.0f px = %.1f deg on DP-1-2"
          % (outer, outer / PX_PER_DEG))

    print("\nSaccade: history at the old point, current sample at the new one.")
    print("  Without the exclusion mask, trailing rings whose radius equals the")
    print("  jump distance land their brightest part on the new gaze point.")
    print("  %-10s %16s %16s" % ("jump px", "centre, no excl", "centre, with excl"))
    for jump in (0, 120, 180, 240, 300, 360, 480):
        offs = saccade(jump)
        one_minus = 1.0
        for (dx, dy), w in zip(offs, TAP_W):
            one_minus *= (1.0 - PEAK * w * ring(np.array(math.hypot(dx, dy))))
        raw = 1.0 - one_minus
        print("  %-10d %16.3f %16.3f" % (jump, raw, raw * exclusion(np.array(0.0))))

    print("\nColour, composited in LINEAR light then encoded to sRGB:")
    band = (d > R_C - 40) & (d < R_C + 40)
    am = a[band].mean()
    out = linear_to_srgb(IRIS_HALO_L * am + BG_L * (1 - am))
    print("  ring-band mean alpha                  : %.2f" % am)
    print("  ring colour over a dim background     : rgb(%3.0f,%3.0f,%3.0f)" % tuple(out))
    wrong = np.array(IRIS_HALO_S, float) * am + np.array(BG_S, float) * (1 - am)
    print("  (the same numbers composited in sRGB) : rgb(%3.0f,%3.0f,%3.0f)" % tuple(wrong))

    print("\nApparent size, by observer. The player never sees the overlay.")
    for who, w_px, span_mm in (("player, DP-1-2 native", 2560, 597),
                               ("viewer, 1080p in a 1280px window", 1707, 338)):
        f = w_px / 2560.0
        deg = lambda v: math.degrees(math.atan((v * f) / w_px * span_mm / 600.0))
        print("  %-34s clear %.1f deg, ring %.1f deg, outer %.1f deg"
              % (who, deg(EXCL_R), deg(R_C), deg(outer)))


if __name__ == "__main__":
    report()
