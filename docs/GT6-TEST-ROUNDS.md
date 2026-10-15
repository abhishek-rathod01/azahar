# Azahar GT6 / Azahar Optimised: test rounds

Each round is meant to take at most 30 minutes of hands-on time. Everything here is run from the
phone. The Termux helper (`gt6`) and the on-device parts of the Performance Log have been checked
on a Linux machine and by CI compilation, but **not yet on a real phone**, so if a step behaves
differently from what is written, say so and I will fix the tooling rather than the test.

## Which APK, and why the order matters

The app's `versionCode` is derived from the build time, so Android refuses to install an APK
over a *newer* build of the same app. Build and test in this order:

1. **Baseline**: the Performance Log only (commit `7f8656f`), no jitter fixes. It shows as
   "Azahar GT6". Built first.
2. **Optimised**: the current `gt6` head with all fixes, shown as "Azahar Optimised". Built after
   the baseline round, so it installs over the baseline and keeps its settings.

Both use the package `org.azahar_emu.azahar.gt6`, so they install next to the official Azahar,
never over it. They need the three repository secrets (`ANDROID_KEYSTORE_B64`,
`ANDROID_KEYSTORE_PASS`, `ANDROID_KEY_ALIAS`); without them CI only compiles and publishes
nothing. The download is the artifact `azahar-gt6-<short-sha>` of the *gt6-android* run, and the
APK inside is `azahar-optimised-<short-sha>.apk`.

## One-time Termux setup (about 10 minutes)

1. Install Termux from F-Droid or the Termux GitHub releases (the sources its maintainers recommend).
2. In Termux:

   ```
   pkg install -y curl
   curl -fsSL https://raw.githubusercontent.com/abhishek-rathod01/azahar/gt6/tools/gt6/termux/install.sh | bash
   ```

   If it asks for storage access, allow it; that lets `gt6 pack` put the result in `Download/`.
3. Developer options: tap *Build number* 7 times (Settings, About device, Version), then open
   *Developer options* (search for it in Settings) and turn on **Wireless debugging** (Wi-Fi must
   be on).
4. Tap *Pair device with pairing code*. Note the **pairing port** and the **6-digit code** and run
   (in Termux, in split-screen or quickly switching back):

   ```
   gt6 pair <pairing-port> <code>
   ```
5. On the main Wireless debugging screen note the **port** next to the IP address and run:

   ```
   gt6 connect <port>
   ```

   The port changes whenever wireless debugging is switched off and on or Wi-Fi changes. Redo
   this step then.

`gt6 help` lists every command.

## Round 1: baseline (the jitter diagnosis)

Pick the game and scene where you have seen the jitter, and an in-game save close to it.
Close background apps and use the same performance mode you normally play in.

1. Install the baseline APK. Open it, go to **Settings, Debug, Performance Log (GT6)** and turn it
   **on**. (It is off by default and is read when a game starts.)
2. In Termux: `gt6 log start baseline`
3. Start the game and play normally. A small **MARK** button sits at the top right of the game
   screen while the log is on.
4. When the jitter starts, **tap MARK without leaving the game**. Keep playing about 20 seconds.
5. Then do your usual cure: open Settings and come back. Play about 20 more seconds.
6. If no jitter shows up in 20 minutes, tap MARK once anyway and do the Settings round trip
   anyway; that is the control run.
7. Quit the game. In Termux: `gt6 log stop`, then `gt6 pack`.

**Upload to the chat:** the file `Download/gt6-<date>_baseline.tar.gz`. It contains the
Performance Log CSV and Azahar's own log (when `gt6 pack` could reach them), the CPU frequency,
thermal and display logs, SurfaceFlinger frame timing, and the crash buffer.
If `gt6 pack` says it could not pull the app's files, also send `log/perf_*.csv` and
`log/azahar_log.txt` from your Azahar user folder.

If the app crashed: run `gt6 crash` as well and send its output folder. Azahar's own log comes
first; a full Android bug report (it contains personal data) only if I ask for it.

## Round 2: the optimised build

Same steps as Round 1 with the Optimised APK (installed over the baseline). Two new switches in
**Settings, Debug** let you compare on the same APK: *Raise game thread priority (GT6)* and
*Performance hints (GT6)*, both on by default and read when a game starts. For an A/B, play the
same scene once with both on and once with both off, and name the Termux label accordingly
(`gt6 log start opt_on`, `gt6 log start opt_off`).

Acceptance (from the brief): three 60-minute sessions with no jitter episode (p99 frame time above
25 ms for longer than 2 s), and the Settings round trip no longer changes anything in the log.

## Benchmark round (for "faster than the official build")

Same save and scene, same performance mode, background apps closed, Performance Log on for GT6
builds. Play the scene once as a **warm-up that is not counted** (it fills the shader cache), then
**3 runs of 5 minutes** per build with **5 minutes cooldown** between runs, plus one 15-minute
sustained run per milestone.

If both apps use the same Azahar user folder (a good way to make sure the settings are identical),
they also share the driver cache files in `shaders/vulkan/`. The optimised build appends an
integrity footer to them and throws away any file without a valid footer, so the first time after
the official app has written a cache, the optimised build rebuilds it. The official app only checks
the Vulkan header at the start of the file and so keeps a footered file; the driver then decides
whether the 24 extra bytes are fine (not yet checked on the phone). Worst case is a slow first
minute after switching, never wrong output, which is one more reason for the uncounted warm-up run
after every switch between the apps. The official app has no Performance Log, so its frame
timing comes from the Termux logger (`GT6_PKG=org.azahar_emu.azahar gt6 log start official`),
which records what SurfaceFlinger presented. Both builds must run with identical settings; the
log header records the effective settings of the GT6 build, so also screenshot the official
app's graphics settings.

I compute average FPS, 1 % low FPS, p99 frame time and stutter count from the raw files with
`tools/gt6/analysis/analyze_perf.py`.

## Stability checks (Termux)

```
gt6 monkey 1 3000        # menus, three different seeds
gt6 monkey 2 3000
gt6 monkey 3 3000
gt6 lifecycle 5          # home/resume, screen off/on, trim memory, background kill/relaunch
gt6 corrupt-cache        # truncates the Vulkan shader cache (backs it up first); then start a game
gt6 crash                # collects the crash buffer and exit info
```

`gt6 corrupt-cache` prints the exact command to restore the backup. By hand, in addition: 30+
minutes of play per game, Settings round trips, rotation and split-screen, controller
connect/disconnect, in-game save/load, Vulkan to OpenGL ES switch, starting several games in a
row.
