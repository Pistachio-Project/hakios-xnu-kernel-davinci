# Porting notes: lessons from the davinci (Mi 9T, SM7150) bring-up

What we learned bringing iOS 27 (Darwin 27) up on the Xiaomi Mi 9T
(davinci, Snapdragon 730 / SM7150, Adreno 618).

They are written to be reused on the next device. Each item says what went wrong, how it showed up,
and what to check first. Device-specific values are marked as davinci values.
Look the equivalents up for your SoC; don't copy them.

## Process and safety

- **AMOLED burn-in.** Never leave the phone on a static screen: fastboot, the loader, or a frozen UI.
  - Every run script must check that the phone shows up in `fastboot devices` within 30 s of
    `adb reboot bootloader`. If it doesn't, stop and get the phone back to Android at once.
  - Every run must end with the phone confirmed back in Android (`adb devices` shows `device`).
  - See `davinci-data/run-safe.sh` for the pattern: fastboot watchdog, loader check, back-in-Android check.
- **Pin kernels per run.** The shared build output gets overwritten by other worktrees and branches.
  - Copy each build to `kernels/main-<hash>.kernel` and pass it explicitly (`BENCH_KERNEL`).
  - Check it is the right board: `strings K | grep -c "<board name>"` must print 1.
- **One kernel build at a time.**
  - Check with `ps -eo args | grep -E "^(/bin/)?bash kernel/build.sh"`, not `pgrep -f`, which matches itself.
- **Boot-arg line limit.** On davinci the loader takes at most 607 characters. Board defaults eat most of it.
  Keep diagnostics short and drop the ones a run doesn't need.
- **Every run should test a concrete fix.** Use a fixed scripted benchmark (`ui_bench=2`) to compare.
  Hand-driven fps numbers depend on what was on screen and can't be compared run to run.
- **Read the whole log before concluding.** A `grep | head` hid the windows where touch interrupts did
  fire, and "0 interrupts" got misreported as a regression. Count over the full run.

## Logging pitfalls

- **printf vs IOLog.** In some kexts (e.g. MiniIOMobileFramebuffer) `printf` never reaches the run log; only
  `IOLog` does. If an expected line never appears, check this before concluding the code didn't run.
- **Periodic stats from a timer.** If a stats line hangs off a code path that only runs when a client
  registers for something, it may never print. Example: vsync stats that only started once a type-5 port
  was registered. Drive diagnostics from a timer (thread_call) of their own.
- **Daemon os_log.** RunningBoard/FrontBoard reasons (e.g. why an app got `0x8badf00d`) are in os_log only.
  Name the job in `OSLOG="com.apple.runningboardd"` (optionally `OSLOG_LEVEL=debug`) for `mkjobs.py`.
  The jobs live in the flash system image, so this needs an image rebuild.
- **Symbolication.** The shipped dyld cache is stripped. Stack dumps (`stacks=` boot-arg) give raw addresses
  plus the slide, so keep an unstripped cache copy or a symbolicator around before you need one.

## Qualcomm SoC specifics

### TLMM GPIO interrupts (touch IRQ)

- **INTR_CFG target field.** It routes a GPIO's interrupt to the apps processor (KPSS), and the value differs
  per SoC. sdmmagpie / SM7150 uses `intr_target_kpss_val = 3` at bit 5, where later SoCs use 4.
  - With the wrong value, edges still latch in INTR_STATUS (raw status works) but no interrupt is
    ever delivered.
  - Take the exact fields from the vendor kernel's `pinctrl-<soc>.c` PINGROUP macro: target bit/value,
    enable, polarity, detection bits, raw status bit, dir_conn.
- **Summary interrupt.** It is level-triggered. In the handler, only mask, count and wake the thread.
  The thread clears the sticky latch before each read and unmasks before it sleeps; that way no edge is
  lost in the window between reading and clearing.
- **Keep a latch-polling fallback.** If an interrupt doesn't come, read anyway, so touch can never be lost.
- **Latency.** Once interrupts work, latency is dominated by the I2C read: about 0.56 ms for a GT9889
  report at 400 kHz. To go lower, read fewer bytes.

### Display (command-mode DSI panel, MDP/DPU)

- **Tear check math.** `vsync_count = vsync_clk / (fps * vtotal)` is clocks **per line**. On davinci that is
  19.2 MHz / (60 × 2488) = 128.
- **Before `set_tear_on`.** The tear-check line counter reads 0 until the panel has been sent `set_tear_on`
  (DCS 0x35), so don't judge TE before that.
- **TE pin.** It needs the `mdp_vsync` pin function. On davinci that is gpio10, function 1.
- **Status on davinci.** TE pulses still never arrive: the PP0 line counter stays at 0 after `set_tear_on`
  and the INTR_EN bit is set. Still open; the vsync runs from a 60 Hz timer.
- **iOS 27 backboardd and vsync.** It does not ask IOMobileFramebuffer for vsync (type 5). CoreAnimation
  runs its own display timer. backboardd registers notification ports for:
  - type 7: `IOMobileFramebufferRelbufInfo`, a display id and two mach timestamps, consumed by a
    condition-variable waiter in QuartzCore;
  - type 9: ReqDCPReset.

  The driver must accept type numbers up to at least 9. It used to refuse 9 with `kIOReturnBadArgument`.
  The Enable*Notifications type map from the iOS 27 IOMobileFramebuffer:
  HotPlug 0, Power 1, CRC 4, VSync 5, NeedSwap 6, ReqDCPReset 9, GenlockError 11.

### Wi-Fi (WCN3990), RMTFS

- **RMTFS memory.** It needs a low DMA carve-out (the loader carves 128 MB: `lowdma`, `lowdma_mb`).
  Without it, the remote filesystem panics.
- **VHT80 rates.** Set up VHT80 and the rates properly. A wrong setup gave about 50 Mbit/s instead of the
  expected ~140+.

## iOS userland on foreign hardware

- **EDR.** backboardd looks up `com.apple.libEDR.Notification`. If launchd holds the name but nobody
  serves it, CoreAnimation waits out libEDR's timeout (about 40 s) while holding the display lock.
  The result is one frame in 40 s. Either run a stub EDR server (`EDRSERVER=1`) or don't publish the name.
- **IOSurface memory.** Take surface memory only while a surface is in use and release it when unheld,
  or IOKit memory leaks steadily.
- **Flashlight.** Control Center needs `isAvailable` forced YES once the torch driver works.
- **Accessibility.** Reduce Transparency/Motion needed an AXS interpose and a local `notify_post`
  before they applied.
- **Touch input thread.** Real-time priority, about 62, plus a 2 ms poll while a finger is down.
  This made the biggest felt difference before interrupts worked.
- **Scheduling.** Use AMP with QoS-based placement (Edge-style `amp=1`). Pinning the render thread to the
  big cores was rejected; `amp_ui` was worse.
- **Settings white screen** (open, davinci). RunningBoard kills Preferences with `0x8badf00d`, and only
  Preferences. The main thread is idle in its runloop at the time, which suggests a missed scene or
  RunningBoard handshake. Still being diagnosed with runningboardd's os_log.
- **Icons rebuilt every boot** (open). The icon caches are saved and restored, yet the graphic icons are
  still re-rendered. The bursts of rendering from iconservicesagent line up with frame hitches.

## Performance: where the time went (davinci)

- **Not the bottlenecks** (all measured):
  - memory bandwidth (~11 GB/s);
  - swap: already asynchronous, with a two-slot queue and a presenter thread;
  - GPU clock knobs;
  - the uber shader size.
- **What raised the scripted benchmark:**
  - input-thread fixes: real-time priority, the 2 ms poll, then interrupt-driven touch;
  - removing the EDR stall;
  - the IOSurface leak fix.

  The scripted benchmark (`ui_bench=2`) went from about 27–32 fps to 40 fps.
- **Remaining stutter.** It is in long frames, not the average: the 1% low is about 6 fps.
  - The hitch durations cluster at exact multiples of 100 ms, both with and without the TE changes.
    That points at a 100 ms timed wait somewhere in CA or its clients.
  - No such wait exists in our kernel or Metal plug-in. The GPU fence waits are 2 ms or 200 µs; the swap
    queue wait is 700 ms.
  - First suspect: the type-7 RelbufInfo condition-variable waiter in QuartzCore.
- **Wrong earlier hypotheses, so you don't retry them:**
  - memory exhaustion;
  - the 56k uber shader;
  - synchronous swap;
  - "0 half registers" for fp16 (a stats artifact);
  - a memory cap (it regressed fps from 31 to 26).
