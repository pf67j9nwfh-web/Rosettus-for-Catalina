# Efficiency

Measured on a 2013 MacBook Pro with Mojave. The Snow Leopard column is the same app under the real
Rosetta on a 2007 MacBook Pro with Snow Leopard. Approximate figures.

## Idle cost (Hex Fiend, Cocoa, left idle)

| | Rosettus | Snow Leopard |
|---|---|---|
| Idle wakeups per second | 13-19 | ~0 |
| Context switches per second | 44-99 | - |
| Idle CPU | 0.9-1.1 % | 0.0 % |
| Helper daemons when no PowerPC app runs | exit after 30 s | (system daemons) |

* In the background an app settles at about 0.5 % CPU (0.01 s every 2 s). Switching away from it
  costs a short burst of about 0.5-1.5 s of CPU, for the inactive redraw and the clipboard check.
* **Event pump:** the window server is polled every 8 ms only while events arrive, and every 50 ms
  after a second of quiet (`OAH_EV_IDLE_MS`). In the background (another app in front) and after 2 s of quiet,
  every 200 ms (`OAH_EV_BG_IDLE_MS`); apps with menu bar items keep 50 ms. Idle CPU in the
  background: Hex Fiend 13.7 -> 11.3 ms/s (Mojave), 14.8 -> 12.2 ms/s (Catalina); TextWrangler
  24.5 -> 22.8 ms/s (Catalina).
* **Sound output:** once an app had played a sound, the output unit (Mojave) or the thread mixing
  for the host (Catalina, 250 wake-ups a second) ran until the app quit. It now stops after 10 s of
  silence with nothing queued, and the next Sound Manager call starts it again
  (`OAH_SND_IDLE_SECS`). Apps that stream continuously (Kid Pix's drawing screen) keep it running.
* The test-input file is checked once a second instead of four times.
* **Default buttons:** HIToolbox and AppKit 10.6 pulse the default button of the front window
  at 30 frames a second. Carbon default buttons are no longer animated; Cocoa ones draw one
  heartbeat frame every 2 s (that frame is what makes them blue). Idle CPU with the dialog open:
  Word 2004's Project Gallery 120 -> 10 ms/s (Mojave), TextWrangler's Open panel 152 -> 55 ms/s.
  `OAH_BUTTON_PULSE=1` keeps the pulse.
* **Launch (Catalina):** the translator asks for `vm_region` some 10000 times a second while
  an app starts; rhost answered from its 32-bit page map by testing one page at a time, which
  took 8 s of CPU in a Pages launch. It now skips 8 and 64 pages at a time. Launch to the first
  window: Keynote 22-24 s -> 12 s, Pages 23-26 s -> 10-19 s, Word 7.8 s -> 5.2 s.
* **Memory (Catalina):** the resident size (850 MB for Pages) counts the Mojave i386 libraries,
  which are mapped from files, clean and shared. The footprint is about 330 MB (Mojave: 187 MB).
  The i386 libraries already come from Mojave's i386 shared cache, prebound (the i386 dyld maps it
  itself when rhost has no shared region to offer): 31 MB of the 32-bit world's 305 MB of dirty
  memory is in the cache, 90 MB in all file mappings, and 214 MB is anonymous (the translator's
  code and data, the guest heaps); where that differs from Mojave is not measured yet.
* **Still open (Catalina):** every system call and Mach trap of the 32-bit world reaches rhost
  as a signal; in a launch that delivery is about a third of rhost's own CPU time.
* **Helper agents:** the pasteboard, font server, font worker and coreservicesd start on demand and
  quit 30 s after the last PowerPC app.
* **No Terminal:** apps start directly from Rosettus Launcher, from patched bundles or from launcher
  apps. Nothing stays open after the app quits.

## Memory and launch time

The apps were opened through their launchers, as from the Finder. "Footprint" is the memory macOS
charges to the app (Activity Monitor's Memory column), 20-25 s after launch.

| App | Footprint | First window |
|---|---|---|
| Hex Fiend | 72 MB | 4.1 s |
| AppleWorks 6 | 101 MB | 3.5 s |
| TextWrangler | 150 MB | 9.0 s |
| Microsoft Word 2004 | 124 MB | - |
| Pages '09 | 224 MB | 14.1 s |
| A command-line tool | ~5 MB | |

The shared install (`/Library/Rosettus`) costs nothing over a per-user one.

Most of each app's footprint is made up of:

* the PowerPC frameworks' own data after binding: dyld fix-ups, 30-60 MB in Cocoa apps;
* the guest heap;
* Rosettus' window stores: one desktop-sized bitmap per window, about 5.5 MB each on a 1440x900
  screen.

File-backed pages, such as the PowerPC libraries' code, are shared between apps by the kernel and do
not count toward the footprint.

liboah finds PowerPC Mach-O images by reading guest memory. Images are always mapped from files, so
it reads only file-backed regions. Reading anonymous memory would make the kernel back every page it
touched: about 55 MB in an idle process and 150-230 MB in a GUI app. `OAH_SCAN_ANON=1` scans anonymous
memory too, for comparison.

Other savings:

* native libm, through the Rosetta interposers;
* a fast path for bridge calls;
* a coalescer for CGL flushes;
* skipping window pixels that did not change;
* lazy Gestalt registration.

## System calls (Catalina)

The 32-bit translator makes its system calls through the `sysenter` stubs of Mojave's 32-bit
libsystem_kernel. Catalina has no 32-bit system-call entry, so rhost rewrites each stub. It used to
rewrite them to `int 0x80`, which macOS turns into a SIGILL; rhost then carried the call out in its
signal handler and returned through sigreturn: three kernel crossings and a full signal frame per
call. The stubs now jump to a small trampoline below 4 GB that makes a far call into 64-bit code
(`r2_gate64`, src/rhost/gate.s). The gate saves the 32-bit registers and the FPU/vector state,
switches to the thread's host stack and host TLS, runs the same system-call code as the signal path,
and returns to 32-bit mode with one `iretq`. Signals that arrive meanwhile and the 32-bit signal
mask are handled as in the signal path.

| | Signal path | Gate |
|---|---|---|
| 1.8 million calls (tests: gatetest) | 7.6 s | 1.3 s |
| Pages '09, first window | 10.4 s | 9.2 s |

About 98 % of the traps of a Pages launch now go through the gate, Mach traps included: Mojave's
libsystem_kernel makes them through the same `sysenter` stub with a negative number
(mach_msg_trap is -31). What still takes the signal path is small: `int 0x81` from the translator's
own code (mach_msg_overwrite for guest messages and semaphore waits, about 300 a second during a
launch), shim host calls (`int 0x82`, about 100-300 a second) and a few direct `int 0x80` calls.
Moving those to the gate would save about 1 ms a second. `R2_GATE=0` turns the gate off.

Cost per call (tests/catalina/vmbench.c, i386; the translator's hot calls are vm_read_overwrite and
vm_region on its own task, about 12 000 and 10 000 a second while an iWork app starts):

| | getppid | vm_read_overwrite | vm_region |
|---|---|---|---|
| Mojave, native | 0.27 us | 1.34 us | 1.33 us |
| Catalina, gate | 1.39 us | 2.43 us | 2.41 us |
| Catalina, signal path | 3.73 us | 5.36 us | 5.08 us |

The gate's own cost, about 1.1 us a call, is three system calls (the thread id and the %gs base
twice) and saving the vector state; during a launch that is about 5 % of one core. The rest of a
launch is the translator translating and binding PowerPC code.

## Open work

* **Launch-time binding.** Snow Leopard runs PowerPC apps against `dyld_shared_cache_rosetta`. In that
  cache, frameworks are prebound and Objective-C selectors are pre-uniqued. On Mojave every framework
  is bound and fixed up at launch, which runs and translates far more guest code.

  The 10.6 PowerPC cache cannot be reused as it is. It is linked to fixed addresses (0x90000000 for
  text, 0xA0000000 for data) and cannot slide. In a 32-bit Intel process on Mojave, that range holds
  Mojave's own i386 shared cache. Two ways forward: a relocated cache built at a free address, or
  less binding work in the guest at launch.
* **Event port.** `OAH_EV_RECV=1` receives window-server events on the native event port, at about
  13 wakeups a second when idle. It could replace polling once it has been tested more.
* `OAH_MEMREPORT` and the helpers in `tests/` make these measurements repeatable.
