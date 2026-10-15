# Known issues

Bugs that also crash the official Azahar build are listed here instead of being hidden.
Nothing has been reported yet; this file starts empty on purpose.

## Open questions about this fork

* The Kotlin side of the Performance Log (settings switch, MARK button, event hooks) has been
  linted and is compiled by CI, but has not been run on a device.
* The Termux helper's on-device commands (dumpsys layouts, `ps -T -o PSR`, `stat -c`,
  `dumpsys SurfaceFlinger --latency` on BLAST layers, adb access to `Android/data`) are best
  effort and have not been run on a real phone.
* Fresh-install defaults for *accurate shader multiplication* are inconsistent upstream
  (C++ `true`, Kotlin `false`). Compare logged effective settings between builds before drawing
  conclusions from a speed difference.
