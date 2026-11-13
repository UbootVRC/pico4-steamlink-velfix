# Pico 4 Steam Link head-wobble fix (Zygisk module)

Fixes the head wobble / world drift on the **Pico 4** when streaming PCVR through **Steam Link**,
which is at its worst after recentering. It works on the official Steam Link app: no APK patching,
no re-signing, and nothing to redo when Steam Link updates.

On headsets with eye tracking it also corrects where Steam Link aims its foveation when you tilt
your head (new in v1.2).

Tested on a base Pico 4 (A8110), firmware 5.13.7, official Steam Link 2.0.25.

## What it fixes

Both problems are wrong values from Pico's OpenXR runtime that Steam Link forwards unchecked. They
were identified and measured in
[this report by ClaikoCode](https://github.com/ClaikoCode/Pico4e_steamlink_wobble_gaze_fix_report),
which also describes the equivalent patches to Steam Link's binary. This module applies the same two
corrections at the OpenXR layer instead, inside the Steam Link process only.

### 1. Head velocity (the wobble)

The runtime reports the head's linear velocity rotated relative to the head pose. The angle changes
every time Pico resets tracking, which happens on a user recenter and also on its own around
start-up. Steam Link forwards that velocity to the PC, and SteamVR predicts the head pose along the
wrong direction, which you see as the world sliding with your head and slowly correcting.

For the head (`VIEW`) space, the module replaces the reported linear velocity with one derived from
positions: where the head will be 35 ms ahead, minus where it is now. Pose and angular velocity are
left exactly as the runtime reports them.

### 2. Eye-gaze position (misplaced foveation)

On headsets with eye tracking, the runtime reports the eye-gaze pose with a position that can sit far
from the head when the head tilts. Steam Link aims foveation at a point 1 m along the gaze from that
position, so the sharp region lands in the wrong place, typically when looking down.

The module replaces the eye position with the head position, so only the gaze direction is used.
Headsets without eye tracking never make this call and are unaffected.

See [FINDINGS.md](FINDINGS.md) for the analysis and how the module was debugged.

## Limits

- A milder wobble that exists on a fresh boot may remain. Its cause is not established.
- The velocity fix may add a slight stutter on fast head movement. This has been reported but not
  confirmed.
- **Pico 4 Pro and Ultra are untested.** On those models Steam Link adds its own head-offset term to
  the linear velocity, and a derived velocity may count that offset twice.
- The gaze fix is new and has had little testing.

## Requirements

- A rooted Pico 4 with Magisk (or another root solution that provides Zygisk)
- Zygisk enabled
- The official Steam Link app (`com.valvesoftware.steamlinkvr`)

## Install

1. Download the module zip from the Releases page.
2. Install it in the Magisk app (Modules → Install from storage), or over adb:
   `magisk --install-module /path/to/zip`
3. Reboot the headset.

To remove it, disable or uninstall the module in Magisk and reboot.

## Tuning and A/B testing

The module re-reads `/data/local/tmp/pico4velfix-corr` a few times a second while Steam Link is
running, so you can switch modes mid-session. The file holds `mode window_ms gaze`:

| Contents | Effect |
| --- | --- |
| (no file) | Velocity fix on with a 35 ms window, gaze fix on (default) |
| `0` | Velocity fix off / passthrough, for comparison |
| `1 35` | Velocity fix on, derive velocity over 35 ms |
| `1 40` | Same over 40 ms. Lower measured error, but close to the runtime's ~44 ms prediction limit |
| `2` | Zero the linear velocity (comparison only) |
| `1 35 0` | Velocity fix on, gaze fix off |

Example: `adb shell "echo 0 > /data/local/tmp/pico4velfix-corr"`

To confirm the module is active, start streaming and run `adb logcat -s Pico4VelFix`. You should see
`velocity fix active` shortly after streaming starts, and `gaze fix active` on headsets with eye
tracking. Each line repeats about once a minute.

## Build

Needs the Android NDK (r27 used here). Set `ANDROID_NDK_HOME`, then run:

```
pwsh ./build.ps1
```

This compiles `jni/module.cpp` to `zygisk/arm64-v8a.so` and packages a flashable zip.

## Scope and safety

- Only the Steam Link process is touched. Pico system and tracking services are never hooked.
- The hook is a data-only patch of one import slot in Steam Link's `libvrlink_scene.so`, made in
  memory at runtime. Nothing on disk is modified.
- Use at your own risk. This is an unofficial community fix, not affiliated with Pico or Valve.

## Credits

- Root-cause analysis, measurements and the original fixes:
  [ClaikoCode](https://github.com/ClaikoCode), published as
  [Steam Link on Pico 4 Enterprise: head wobble and misplaced foveation](https://github.com/ClaikoCode/Pico4e_steamlink_wobble_gaze_fix_report).
- `jni/zygisk.hpp` is the Zygisk API header by topjohnwu (licence in the file).

## Licence

MIT, see [LICENSE](LICENSE). `jni/zygisk.hpp` keeps its own licence, stated in the file.
