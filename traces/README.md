Recorded gaze traces, written by `gaze-cal record`. Gitignored, and not only because they
are large and machine-specific: a gaze trace is personal biometric data. Nothing under this
directory belongs in git.

Each trace carries a `<name>.csv.correction.conf` beside it, a byte copy of the host-side
fit that produced its corr_* columns. A trace without one was recorded with `--raw` and its
corrected columns are all nan, which makes it useless for fitting the overlay filter.
