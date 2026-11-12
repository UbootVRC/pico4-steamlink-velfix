# Pico 4 — Steam Link head-wobble: findings & status (updated 2026-11-12)

A shareable writeup of the Pico 4 head-wobble ("head velocity calculated off angle") seen when
streaming PCVR through **Steam Link / SteamVR**. This version supersedes an earlier one whose main
conclusion ("not fixable app-side") was **wrong** — an independent, rigorous investigation found the
real mechanism and a working app-side fix. This doc covers: the corrected root cause, the working
fix, a version-independent Zygisk module that applies the same fix to the official app, and the
firmware-side analysis (now mostly moot).

All findings are from static analysis of extracted firmware/app binaries plus on-device testing.
No proprietary code is reproduced — only symbol names, offsets, behavior, and measurements.

---

## 1. Symptom

- The world drifts with head movement, **late and only partially**, then slowly corrects. Moving
  forward/back/up/down shifts the world the same way; turning couples into translation.
- **Much worse after recentering** the Pico (moving the playspace away from where tracking booted).
- The error is **dynamic** (motion/velocity-driven). Held still, the pose is correct.
- The Pico 4 Ultra had the same bug on older firmware and Pico fixed it on the Ultra in a firmware
  update; the Pico 4 never got that fix.

---

## 2. CORRECTED root cause — it's the forwarded velocity, not the pose

Steam Link is a streamer: the headset app reads the head pose from the Pico OpenXR runtime and sends
it to the PC, where **SteamVR extrapolates the pose forward using the velocity Steam Link forwards**
(measured at **H ≈ 78 ms**, gain ≈ 1.0). The bug is in that **forwarded velocity**:

- Steam Link's `GetPose` reads the head via `xrLocateSpace(VIEW, STAGE, now, …)` with an
  `XrSpaceVelocity` chained, and forwards pose + velocities to the PC.
- From the Pico 5.13.x firmware (traced + emulated): the `VIEW`-in-`STAGE` **pose** is built from the
  *un-reset* tracking pose + the inverse stage origin → it does **not** rotate when you recenter.
  The reported **linear velocity** has the *reset offset* applied with no stage conversion → its
  **direction rotates when you recenter**.
- So after a recenter, **pose and linear velocity are in different yaw frames**. The velocity-
  direction error matched the recenter offset yaw + a per-session constant to within ~0.6°. SteamVR
  then predicts ~78 ms along the wrong direction → the drift. A 90° error gives 1.41× the error of
  doing no prediction at all. (Only direction is wrong: speed matched within ~1%, timing within
  ~5 ms.)

This corrects a wording both earlier analyses used ("velocity fails to follow the recenter"): the
firmware shows the opposite — the **velocity follows the recenter and the pose does not**. Same-size
mismatch, opposite attribution.

**What is NOT fixed by any of this:** a milder wobble is present even on a **fresh boot with no
recenter** (both patched and unpatched scored similarly there). That residual is likely inherent to
Steam Link's prediction pipeline — it is the only PCVR streamer using a quad-views rendering
approach, and even the *fixed* Pico 4 Ultra and the Quest 3 still show some bounce/warp. Lowering the
PC-side **HMD prediction latency to 0** helps the mild cases but not the strong post-recenter one.

---

## 3. The working fix (app-side, proven)

Replace the forwarded head **linear velocity** with one **derived from positions** (how far the head
moves over the next ~35 ms), keeping the official pose and angular velocity. A position-derived
velocity is automatically in the correct frame, so SteamVR extrapolates the right direction.

- Delivered as a **binary patch to Steam Link's `lib/arm64-v8a/libvrlink_scene.so`** inside the APK.
  The earliest version was ~27 bytes / 7 instructions (request the pose 70 ms ahead + stop sending
  head velocities); the current "derived35" build keeps pose + angular velocity and sets
  `linearVelocity = (pos(now+35ms) − pos(now)) / 0.035` (failed future-locate → zero velocity).
- A **blind, checksum-verified 8-trial A/B** (one person) on the turn-step-recenter case: official
  scored 4,4,4,4 (unplayable), patched 1,2,2,1 (p = 0.0286). The **strong post-recenter wobble is
  removed.** The mild fresh-boot wobble is not, and a slight new stutter can appear on quick random
  motion.
- Notes: the runtime clamps a future-pose request to ~44 ms (`xrMaxPredictedTime`); a 35–40 ms
  derivation window tested best. The idea was adapted from how ALVR already handles Pico velocity
  (it avoids the runtime's reported head velocity, citing a wrong frame of reference).

**The catch:** the patch edits **version-specific byte addresses**, so it breaks on every Steam Link
update and must be redone per version. The patched APK also contains Valve's app (re-signed with a
personal key), so it is shared as a *recipe*, not a file, and installing it means uninstalling the
official app and re-pairing.

---

## 4. Version-independent fix (Magisk/Zygisk module) — working

Goal: do the same velocity replacement at the **OpenXR layer** instead of patching Steam Link's
bytes, so it survives Steam Link updates and needs no APK modification (run official Steam Link,
enable a module). A Zygisk module scoped to the Steam Link process hooks `xrLocateSpace`, and for the
head (VIEW) space recomputes `linearVelocity` from a position sample at `now` and at `now + 35 ms`,
overwriting the chained `XrSpaceVelocity.linearVelocity` and leaving pose + angular velocity
untouched.

**Result on official Steam Link 2.0.25: the strong post-recenter wobble is gone** (one person,
non-blind, with the module's log confirming every head locate was intercepted and corrected).

**Why the first attempt appeared not to work:** the module's first build intercepted nothing on the
head path, and this was wrongly attributed to Steam Link calling `xrLocateSpace` through a pointer
cached from `xrGetInstanceProcAddr`. Disassembly showed otherwise: all five `xrLocateSpace` call
sites in `libvrlink_scene.so` use the ordinary named import (Steam Link only uses
`xrGetInstanceProcAddr` for extension functions), so patching that import slot does reach the
per-frame head read in `QSVLDeviceHmd::GetPose`. The real bug was in the module: it searched the
`next` chain for the wrong structure type. `XrSpaceVelocity` is a core structure with
`XR_TYPE_SPACE_VELOCITY = 43`; the module was looking for an extension-style value and never matched.
Correcting that one constant made the module work.

What the module depends on: Steam Link reading the head with `xrLocateSpace` on a `VIEW` reference
space with an `XrSpaceVelocity` chained. It needs no offsets or version-specific addresses.

(The module is scoped strictly to the Steam Link process and never touches Pico system/tracking; it
is reversible via Magisk disable + reboot.)

---

## 5. Why the Ultra's firmware fix can't be ported to the Pico 4 (now mostly moot)

Earlier work isolated the Ultra's fix to a single reworked function (`inverseOffset`, 198→161
instructions) inside a **newer tracking subsystem (`HeadClientSDK`) that the Pico 4 does not have**.
The Pico 4's offset functions are byte-identical to the Ultra's *pre-fix* build and were never the
bug; there is no 1:1 target to patch, and the client↔service ABI differs by hardware generation. So
a firmware-level backport can't be grafted from outside — it needs Pico to do it, as they did for the
Ultra. This matters less now that the problem is understood as an **app-side (Steam Link) velocity**
issue that can be corrected without touching firmware.

---

## 6. Earlier dead-ends (ruled out, for completeness)

- **Correcting the head *pose*** in `xrLocateViews`/`xrLocateSpace`: the delivered pose is consistent
  across all OpenXR reference frames (pos err ~0.001 m, ang err ~0.4° with a large offset), so a
  static rotation only displaces you — it can't undo a dynamic velocity-frame error. (This is what
  led to the earlier, wrong "no app-side fix" conclusion — it was looking at the pose, not the
  velocity.)
- **App-side prediction-time change** (`displayTime`): the runtime ignores/clamps it from the app;
  felt like no effect.
- **System-level origin resets**: Large Space mode (re-base origin) and `ResetTracking` (SLAM
  re-init) both still produced the bug — any non-boot origin triggers it, and these don't help.
- **Zeroing velocity alone** (an early test): appeared to change nothing, which was wrongly taken to
  rule velocity out; it only showed that removing velocity without supplying a derived one leaves
  position uncorrected (plain latency can look similar).

---

## 7. Practical status / recommendations

1. **Working without root:** the Steam Link APK byte-patch (derived35 approach). Downside: must be re-done
   per Steam Link version, and it's shared as a recipe (contains Valve's app). A separate variant
   also fixes an eye-tracking/foveation bug (eye position reported in the wrong frame on head tilt;
   fix = use gaze rotation only).
2. **Version-independent module (recommended if rooted):** a Zygisk module that applies the same
   derived-velocity fix to official Steam Link at the OpenXR layer. Works on 2.0.25; needs root with
   Zygisk but no APK changes and no per-version rework (see §4).
3. **Alternative streamer:** Virtual Desktop or ALVR do their own head-pose prediction and don't use
   Steam Link's quad-views path, so they may avoid this entirely — the most likely route to a good
   experience without per-version patching.
4. **Residual mild wobble** (fresh boot) and a possible new stutter appear inherent to Steam Link's
   prediction/quad-views pipeline and aren't fixed by the velocity correction; PC-side HMD prediction
   = 0 helps the mild cases only.

---

## 8. Methodology / tooling (for anyone reproducing)

- Firmware: unpack Android A/B `payload.bin` → EROFS images → extract to read `/system` and
  `/system_ext` tracking libraries (`libSlamTools.pxr.so`, `libtrackingclient.pxr.so`,
  `libpvrtrackingcommon.so`, `libpvrtrackingservice.so`, `libpxrruntime.so`,
  `libpxrcompositor.so`), and Steam Link's `libvrlink_scene.so`.
- Analysis: capstone ARM64 disassembly with PC-relative masking for cross-build diffs; PLT→`.rela.plt`
  resolution to name imported calls; a symbolic NEON-FP tracer for the quaternion math; an ARM
  emulator to replay recorded motion through the real binaries and measure predicted error.
- Key method: diff the **same device across pre/post-fix firmware** (Ultra 5.14.5 vs 5.15.2) to
  isolate the firmware change; and **measure the velocity-direction error against the recenter
  offset** from the runtime's own logs (they agreed to within ~0.6° plus a per-session constant).
- APK patching: edit `libvrlink_scene.so`, rezip + sign (a personal key means uninstall the official
  app and re-pair). Builds are tracked by SHA-256.
- Runtime hooking: Zygisk module scoped to the Steam Link process; patching the named
  `xrLocateSpace` import in `libvrlink_scene.so` reaches the per-frame head locate (see §4).

---

## 9. Bottom line

- The wobble is a **recenter-offset / velocity-frame error**: after a recenter the head's pose and
  its forwarded linear velocity are in different frames, and SteamVR predicts ~78 ms along the wrong
  one.
- It **is fixable app-side** (this corrects the earlier doc): replace the forwarded linear velocity
  with a position-derived one. A per-version Steam Link byte-patch does this and removes the strong
  post-recenter wobble (blind-tested). A version-independent Zygisk module does the same on the
  official app with no APK changes, and is the better delivery for rooted headsets.
- The mild fresh-boot wobble and quad-views prediction behavior appear to be **Steam Link-side** and
  aren't addressed by the velocity fix; a different streamer (Virtual Desktop / ALVR) is the simplest
  path to avoid the whole thing. A true in-headset fix needs Pico firmware.
