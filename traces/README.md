Recorded gaze traces, written by `gaze-cal record`. Gitignored, and not only because they
are large and machine-specific: a gaze trace is personal biometric data. Nothing under this
directory belongs in git.

Each trace carries a `<name>.csv.correction.conf` beside it, a byte copy of the host-side
fit that produced its corr_* columns. A trace with no correction file beside it was
recorded with `--raw` and its corrected columns are all nan, which makes it useless for
fitting the overlay filter. The two records can't disagree, since a `--raw` recording
removes any correction file left beside that path by an earlier corrected one.

Twenty-eight columns. The sixteen raw ones the device sent, then six corrected ones in
normalised display coordinates, then six eye-origin ones in tracker millimetres:

    host_ns device_us frame_counter present_mask validity_L validity_R
    lx ly rx ry combined_x combined_y unfiltered_x unfiltered_y pupil_L pupil_R
    corr_lx corr_ly corr_rx corr_ry corr_cx corr_cy
    eye_lx eye_ly eye_lz eye_rx eye_ry eye_rz

`validity == 0` means valid. A corr_* field whose input eye is invalid is the literal text
`nan`. The eye_* fields are never nan: this firmware writes a plain 0.0 when it sees no
eye, and that zero is recorded as it came, since the validity columns already say which eye
is real. They are there because form S is scoped per seating position and degrades at about
0.63 px per mm of head movement in the screen plane, so how far the head drifted during a
session is the one thing a later analysis can't recover from a trace that omits it.
