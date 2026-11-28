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
- **Where the tear check lives (SDE 5.x).** On SDE 5.x (davinci hw rev 0x50020000) the tear check is in the
  **INTF** block, not the ping-pong block. Programming PP0's tear check was why TE "never arrived".
  - On davinci: INTF1 at 0xae6b800, tear check at +0x280 (VSYNC_SEL 0x280, CHECK_EN 0x284,
    SYNC_CONFIG_VSYNC 0x288 with the TE-connect bit 20, HEIGHT 0x28C, LINE_COUNT 0x2B0,
    AUTOREFRESH_CONFIG 0x2B4).
  - The RD_PTR interrupt is in the MDP interrupt block at mdp+0x6e800, bit 2 (SPI 83).
  - Result: 301 interrupts in 5 s, period 16.65 ms.
- **Zero-copy scanout** (the panel fetches the window server's surface directly). Copy Qualcomm's per-frame
  kickoff exactly (sde_encoder_phys_cmd); guessing it gave missed flips and a glitchy screen. The sequence:
  1. Disconnect TE (clear bit 20 of SYNC_CONFIG_VSYNC).
  2. Turn autorefresh off.
  3. Poll LINE_COUNT until it is outside (0, vdisplay).
  4. Reconnect TE and write CTL_FLUSH.
  5. Turn autorefresh back on. No CTL_START.

  Result: 0 missed flips, 2.6 ms from flip to the panel taking it.
  - Release the old surface only once the flush is taken.
  - Drop a frame if the previous flip hasn't settled.
  - Fall back to the copy ring after repeated misses.
- **Display fetch QoS.** The bootloader leaves the splash pipe's QoS settings, sized for an unscaled splash.
  Program them yourself, as `sde_plane.c` does, from the SoC's `-sde.dtsi`:
  - creq LUT, danger/safe LUTs, QOS_CTRL and CDP for the scanout SSPP;
  - the VBIF RT remap levels.

  davinci uses VIG0 at mdp+0x5000 and VBIF at mdp+0xb0000 (adreno.c `scanout_qos_program`, `fb_qos=0`
  turns it off). Untested on the phone at the time of writing.
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

### CPU, caches (SM7150 / OSM SoCs)

- **Check the cache code runs on your SoC.** Our L3 and LLCC code was written for SC7280 (EPSS) and silently
  did nothing on SM7150 (OSM). If a feature is gated on the SoC, log a line when it is skipped.
- **The L3 clock on OSM SoCs.** It has its own OSM block 0x2000 before the cores' (davinci 0x18321000;
  `osm_l3_base` in `<soc>.dtsi`), with the same layout as the core groups: table at 0x110, 32 bytes a row,
  the wanted row at 0x920. `l3=1 l3_probe=1` raises it and measures a 2 MB pointer chase before and after.
  davinci: the bootloader leaves row 7 (1190 MHz). The top row (1459 MHz) cut the chase from 97.1 to 89.8 ns,
  so davinci raises it by default (`l3=0` leaves it alone).
- **LLCC slice IDs per SoC.** Take them from `llcc-<soc>.c`. sdmmagpie: cpuss 1 (512 KB), gpuhtw 11, gpu 12.
  - On the GPU side, A618 takes kgsl's CX_MISC path: SYSTEM_CACHE_CNTL_1 gets five 5-bit GPU SCIDs, with the
    HTW SCID at bit 25, and SYSTEM_CACHE_CNTL_0 = 3.
  - Off by default (`llcc=2` turns it on). On SM7325 the CPU slice made the pointer chase faster with no UI change.
    davinci (LLCC v1.1.2, 2 banks): 2 MB chase 93.5 -> 82.2 ns, nothing measurable from 3 MB up. Left off.
- **Governor (davinci).** `pm_gov=1` turns it on; it is OFF by default:
  - a 250 ms top-speed boost on touch;
  - otherwise 1.5x headroom over load, fast up and slow down;
  - a soft thermal cap from 60 C down to 75% of top (big cores) at the 70 C trip, combined with
    thermal.c's limiter by MIN.

  Measured with ui_bench, two pairs in both orders: on 36.7 / 38.7 fps with 101 / 79 frames over 100 ms;
  off 41.1 / 42.6 fps with 71 / 63; peak 66-69 C either way. It drops the small cores when the load falls,
  and the next animation starts on slow cores, which feels like lag. The heat is held by thermal.c's
  limiter, not by running slower. A load-only governor needs a frame signal (a boost on the start of
  an animation or a render-server wake) before it can match a fixed top frequency.

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
- **Read the pauses, not just the fps.** The ui_bench fps figure leaves out every gap of 250 ms or more.
  Two variants that looked best (8-bit render targets, GPU 610 MHz) had 11-13 s freezes hidden from the
  average. Always report the "gaps of 250 ms or more" total and the longest gap next to the fps.
- **Visual checks for "free" GPU savings.** 8-bit stand-ins for 16-bit float render targets (`metal_rt8=2`)
  and a coarser glass blur level (`metal_glass_lod=4`) together gave Liquid Glass a black backdrop. The
  benchmark can't see that: have a person look before you make any rendering change the default.
- **HITCH verdicts.** The Metal plug-in's "HITCH breakdown" lines label each long frame. Most of davinci's are
  BLOCKED: in a 200 ms frame backboardd used about 10 ms of CPU, so it is waiting, not working.
- **Off-CPU profiling with symbols.** Run `waitprof=<process>[,<process>]`. From the start of the ui_bench
  window each long wait gets a short `wpw` line in the profiler's own buffer (640 KB, appended to the returned
  log as "--- wait profile ---"), with the trap, the message id, 4 user return addresses, the kernel frames and
  the destination process; a `wpc` line gives the slides. `tools/waitprof.py LOG --cache DIR --kernel FILE`
  groups the waits by thread and place and names the addresses. (Before 2026-11-28 the lines went to the log's
  ring only and had no line ends: nothing of the benchmark window survived a run.)
  - Subtract the slide and find the image from the cache header's image list (offset 0x1c0).
  - Local names are `<redacted>` in the main cache. Read them from `dyld_shared_cache_<arch>.symbols`: the
    local symbols info at header 0x48, its entries keyed by the image's offset from 0x180000000.
  - davinci finding: CA's render server (`CA::Render::Server::server_thread`) waits long only right after
    unlock. During the benchmark's pauses it is idle, waiting for app commits, so look at the clients.
- **The Edge scheduler's edges are never set up (`amp=1`).** Apple's kernels call `sched_edge_cpu_init_completed()`
  through `ml_cpu_init_completed()` from AppleARMSMP; the processors here are registered by GenericARMPE, which
  doesn't. Every edge stays zero: nothing migrates from the P cluster to E, no idle E core steals, `amp_spill` is
  never read. waitprof showed it: user-initiated and default threads of SpringBoard and backboardd sat 100 ms to
  seconds on the A76 queue beside idle A55 cores (40-75 s of thread time per ui_bench window), and widget
  extensions were killed by chronod's 10 s watchdog having had no CPU (white widgets: `getTimelines` watchdogged,
  28-31 kills a boot). The edges are now set up at `amp=1` (only them: the rest of `sched_cpu_init_completed()`
  wants every core online; `amp_edges=0` for the old state). Two more things were needed for the frame rate: an
  idle E cluster is taken at wake before any edge weight is looked at, and an idle E core steals whenever more
  than two threads are runnable on P, so the frames were being made on A55 cores (31 fps for 37). `amp_keep`
  (default 7: fixed priority, FG, IN) places those buckets on P at wake and exempts them from the steal.
  davinci, ui_bench, two runs each: no edges 37.5 fps / 2610 frames / 17.3 s of pauses / 30 widget kills;
  edges + keep 7: 37.6 / 2836 / 12.6 s / 14; edges + keep 3: 34.5 / 2690 / 8.8 s / 20.
- **A one-time job that never gets saved runs on every boot.** After a new system image (new image id) the
  LaunchServices store is rebuilt and BackgroundShortcutRunner rebuilds the Shortcuts database: 2 min 10 s of
  CPU on up to two cores. Runs without `ufs_write=1` threw that away each time, and every benchmark of that
  afternoon was 3-5 fps low. After a new image, do one run with `ufs_write=1` before measuring.
- **Report flash as flash (`DKIOCISSOLIDSTATE`).** This was the post-unlock freeze, and it was ours. A block
  driver that doesn't answer this ioctl makes spec_vnops.c treat the device as a spinning disk, so low-priority I/O
  is throttled with the slow-disk periods (40-200 ms, windows up to 500 ms) instead of the flash ones (5-25 ms).
  At boot the disk is always busy: geod (a `LowPriorityIO` job) spent about 40 s in 600 ms page faults inside
  `throttle_lowpri_io`. audiomxd waited on geod before checking in its media services, and SpringBoard waited
  20+ s on audiomxd. With the ioctl answered, the longest pause in the benchmark went from 9-13 s to 1.3-1.6 s,
  and fps from 37-40 to 37.6/43.9. **Answer it in every block driver of a new port.**
- **Finding a chain like that:** `waitprof=<process>` on the stuck client, then on each service it names. `t1`
  in a `wpw` line's destination (`d PID NAME t1 SVC`) means the port is in transit, queued on launchd's port for a service that hasn't
  checked in; `svc` names it. The kernel stack frames (`k:`) symbolize against the unstripped kernel (slide 0).
- **Unlock right after boot.** SpringBoard's threads wait in synchronous XPC calls
  (`_dispatch_mach_send_and_wait_for_reply`) for up to 47 s. The callees are runningboardd, backboardd, biomed,
  iconservicesagent, MTLCompilerService and misd, plus one port no process owned at that moment (probably a service
  still being started on demand). That is the 10-20 s freeze at the start of the benchmark: every service starting
  cold at once. `wpm` lines name the receiving process (`to PID NAME`).
- **Old frames flashing in zero-copy mode.** When a frame can't be shown zero-copy, the panel must not go back to the
  copy framebuffer before the new frame has been copied into it. That buffer still holds whatever was copied
  before zero-copy began. Fixed by deferring the switch (`fb_restore_late`).
- **Every-boot permission alerts block scripted runs.** Example: Wallet's "allow notifications" alert. Answer it
  once with `/var` saved (`var_store=1`) so it isn't asked again.
- **Wrong earlier hypotheses, so you don't retry them:**
  - memory exhaustion;
  - the 56k uber shader;
  - synchronous swap;
  - "0 half registers" for fp16 (a stats artifact);
  - a memory cap (it regressed fps from 31 to 26);
  - dropped writes to the GPU's ring write pointer (the watchdog that repairs them counted 0 in three runs);
  - a CPU governor holding the big cores low (both groups were at their top rows; check the cpufreq lines).
