# Pico 4 Steam Link head-wobble fix (Zygisk module)

Fixes the strong head wobble / world drift that appears on the **Pico 4** when streaming PCVR
through **Steam Link** after recentering. It works on the official Steam Link app: no APK patching,
no re-signing, and nothing to redo when Steam Link updates.

Tested on Pico 4, firmware 5.13.x, official Steam Link 2.0.25.

## What it does

After a recenter, the Pico OpenXR runtime reports the head's pose and its linear velocity in
different yaw frames. Steam Link forwards that velocity to the PC, and SteamVR predicts the head
pose along the wrong direction, which you see as the world sliding with your head and slowly
correcting.

The module hooks `xrLocateSpace` inside the Steam Link process only. For the head (`VIEW`) space it
replaces the reported linear velocity with one derived from positions (where the head will be 35 ms
ahead, minus where it is now). Pose and angular velocity are left exactly as the runtime reports
them. See [FINDINGS.md](FINDINGS.md) for the full analysis.

What it does **not** fix: a milder wobble that exists even on a fresh boot with no recenter. That
one appears to come from Steam Link's own prediction pipeline.

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

The module reads `/data/local/tmp/pico4velfix-corr` while Steam Link is running, so you can switch
modes mid-session. The file holds `mode window_ms`:

| Contents | Effect |
| --- | --- |
| (no file) | Fix on, 35 ms window (default) |
| `0` | Off / passthrough, for comparison |
| `1 35` | Fix on, derive velocity over 35 ms (35–40 tested best) |
| `2` | Zero the linear velocity (comparison only) |

Example: `adb shell "echo 0 > /data/local/tmp/pico4velfix-corr"`

To confirm the module is active, start streaming and run `adb logcat -s Pico4VelFix`. Lines
beginning `HEAD in STAGE` mean head locates are being intercepted.

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

- Root-cause analysis and the original derived-velocity fix: [ClaikoCode](https://github.com/ClaikoCode)
- `jni/zygisk.hpp` is the Zygisk API header by topjohnwu (licence in the file).
