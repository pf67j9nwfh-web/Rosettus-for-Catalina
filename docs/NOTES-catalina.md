# Rosettus for Catalina (working name Rosettus 2) - working notes

> **Name (decided 2026-11-08):** this port is **Rosettus for Catalina** (first release: Rosettus for
> Catalina 1.0). The Mojave project (formerly Rosettus 1.0) is **Rosettus for Mojave** (1.1 next).
> "Rosettus 2" / "R2" in these notes and in code identifiers (OAH_R2, R2_* variables, rhost) is the
> internal working name; user-facing names (package, README, launcher) use the new name.

## Layout
- Sources: SL `~/rosettus-work/cat/r2` (edit here) -> `pushr2.sh` -> Catalina `~/rosettus-work/r2`, `bash build.sh` there.
- Helpers (SL `~/rosettus-work/cat`): `ct.sh` (ssh to Catalina via Mojave), `cbg.sh NAME 'cmd'` / `cbg.sh -r NAME`
  (background job on Catalina, log in `~/rosettus-work/logs/NAME.log`), `push.sh FILE`, `where.sh`,
  `msym.sh ADDR...` (on Mojave: symbolize addresses in Mojave's i386 shared cache), `mjclosure.sh`,
  `mjfull.sh` (copy Mojave i386 slices not in the cache).
- Catalina trees:
  - `~/r2/Rosettus`: 1.0 install (+ Snow Leopard i386 slices merged, for the SL-userland mode).
  - `~/r2/mjroot`: Mojave's i386 userland: `var/db/dyld/dyld_shared_cache_i386` (+ .map), `usr/lib/dyld`,
    and every i386 Mach-O not in the cache (/usr/lib, frameworks, Components, Extensions, ...).
- Launchers (made by `mkrun.sh`): `~/r2/Rosettus/rosettus2` (SL i386 userland, r2 liboah build) and
  `~/r2/Rosettus/rosettus3` (Mojave i386 userland + shared cache, the unmodified 1.0 liboah:
  `liboah_mojave.dylib.1.0`; OAH_NO_GLWARM=1). rosettus3 is the main line now.
- Tests: `~/r2tests/tests` (repo's tests/ppc + difftest.sh): `ROSETTUS=~/r2/Rosettus/rosettus3 bash ~/r2tests/tests/difftest.sh`.
- Debug: `R2_FLAGS=-t|-tt`, `R2_WATCHDOG=secs` (dump all 32-bit threads, then exit), `R2_XPCTRACE=1`.

## State (Nov 6)
- rosettus3: 6/9 difftests pass (cf_basics cr_eh files_rsrc isa_edge sys_misc vec_edge).
  au_default_output: HAL works (default device, sample rate) but AudioComponentFindNext finds nothing.
  draw_qd_cg, intl_kbd: crash in guest QD / CF (native bridges reach i386 SkyLight / LS).
- Mojave i386 userland on Catalina works for: libSystem, dispatch (emulated workqueue + kevent workqueue),
  CF, Foundation, XPC to most daemons, CoreAudio HAL (coreaudiod).
- Does NOT work: WindowServer (Mojave SkyLight's MIG routines get MIG_BAD_ID from Catalina's server),
  LaunchServices (lsd drops / never answers Mojave's client; the lookup is now denied so it fails fast).
  => the native side of liboah's window server / LS / GL bridges must run in the 64-bit host
     (planned: hostcall thunks, i386 stubs + host implementations, generated from a signature list).

## Things learned (newest first)
- Word 2004 (Nov 6 afternoon): splash screen, then spins in ShMem (thread_switch loop) at startup;
  open. Fixed on the way there (each was a real bug):
  * Mach OOL receive: the host buffer was deallocated with its size rounded up but its start not
    rounded down, so the page after it went too: the first page of the host's small-malloc region
    (heap "corruption", crashes in malloc much later). Guarded port descriptors (type 4) were also taken
    for OOL memory (context copied and vm_deallocate'd); now converted {context, name, flags}.
  * pipe() returned only the read end (passthrough set eax only; edx = write end).
  * GetProcessInformation (Office checks processFreeMem: "not enough memory or disk space") is a
    host call converting the record (hostcall.c r2h_gpi).
  * map_path: folders this Mac has are this Mac's (the i386 root is sparse: Office built
    ApplicationServices.framework/Frameworks/QD.framework inside the root's copy).
  * launchers (mkrun.sh) carry absolute paths: Office's helper child runs with an empty environment.
  * liboah also unlinks Office's Mso11{Pref,OffG,Priv,Nstb,OleS}<asid> objects at the first start.
  * CGDisplayIsMain/IsActive/IsCaptured, CGShieldingWindowID, CGSSetWindowResolution bridged.
- Test aids added: R2_HEAPCHECK=N (+R2_HEAPCHECK_PAGES) heap checks with an operation ring,
  R2_GMALLOC=1 (Guard Malloc via re-exec), R2_SHMTRACE, R2_SPAWNTRACE, R2_TRACE_CHILD, R2_OOLTRACE,
  fault-address region in host fault dumps; __GUARD segment (no access) at 4 GB..4.25 GB, rhost image
  at 0x110000000; host signal stacks mmap'd with a guard page.
- GUI apps on Catalina (Nov 6, 04:15): Hex Fiend (Cocoa), TextWrangler (Carbon) and AppleWorks 6 (CFM)
  open their windows and draw correctly. Launch them through a bundle with `open` (LaunchServices sends
  oapp); started from a shell, a Cocoa app gets no oapp and opens no window (as on Mojave).
  Test aids: r2/mkl.sh APP [ENV] makes ~/r2launch/<App>.app; R2_SNAP_DIR (snap.c) writes PNGs of the
  process's own windows (no Screen Recording permission needed); wins2.py lists windows by pid.
- Quarantined apps: a file mapping asked for execute makes Catalina ask syspolicyd (Gatekeeper prompt,
  mmap blocks). The translator's file mappings are not executable (PPC code is never run natively), and
  file mappings no longer get the implicit i386 execute bit (sys.c sys_mmap).
- CGWindowListCreate returns an array of plain IDs: bridge type W (hc_ids_out / r2_ids_in).
- The i386 Process Manager registers the app with LaunchServices and aborts on Catalina
  (_RegisterApplication): IsProcessVisible / SameProcess / SetFrontProcessWithOptions go to the host;
  GetProcessBundleLocation etc. are denied.
- Menu bar: the i386 CoreText cannot draw into a host context (a handle): titles are not redrawn
  natively under R2 (the guest's pixels are used). TODO: bridge CTLine drawing for native-looking titles.
- vm_read (3805) emulated: OOL reply in the 32-bit layout {address, size, flags}.
- Word 2004: starts (CFM, resource forks fine) but crashes early: Microsoft Component Plugin calls
  through a NULL entry of a function table in its own data (TOC+0x45b4, near McpInitCertUI; the
  crash is at the CFM glue, r12 = 0). Not a CFM import of CGDisplayIsMain/CGShieldingWindowID etc.
  (now bridged). On Mojave the next log lines are the Dock client callback and
  CGSGetDockRectWithReason. pefimp.py (in rosettus-work) decodes PEF imports; it found no import
  bound at that offset with TOC = data base, so the table is probably filled at run time
  (CFBundleGetFunctionPointerForName): find which lookup returns NULL under R2.
- Mactracker (REALbasic) says "must be running Mac OS X v10.4 or later" (Gestalt sysv answers 0x1068
  in a test tool): open.
- difftest 9/9 (Nov 6, 03:40) with rosettus4 in the GUI context.
- Audio: Catalina's AudioComponentRegistrar lists only 64-bit units; liboah (OAH_R2) dlopens the
  root's Mojave CoreAudio.component and AudioComponentRegister()s its output units (oah_au.h r2_au_find).
- thread_get_special_reply_port (trap -50): one per kernel thread, shared by host and guest libsystem;
  the guest's request replaced the host's and host SkyLight's TCC xpc reply failed with
  MACH_RCV_INVALID_NOTIFY (libdispatch crash). The guest gets mach_reply_port(), its
  SYNC_WAIT/SYNC_PEEK/SYNC_OVERRIDE options are dropped (mach.c; R2_SPECIAL_REPLY=1 = old way).
- Volume group (one volume for "/"): only in i386-program processes. In a PowerPC process the
  translator's 10.6 File Manager handles the two volumes, and with one volume fontd could not
  revive its file tokens (FODBReviveFileTokens) and its registry listed no system fonts (CG text
  missing). Mixed views in one process broke FSNewAlias (-35). R2_VOLGROUP_PPC=1 to try again.
- The PowerPC fontd expects registry content version 14; the 10.6 read-only DB is 15 (written by the
  Intel fontd), so it always regenerates the DB from the font folders (normal, as on Mojave).
- Native Mojave CarbonCore's LogBacktrace crashed on backtrace() returning 0 frames on a translator
  stack: liboah interposes backtrace and walks the frame chain (oah_r2.h).
- /Network does not exist on Catalina: the root's empty /Network stands in (map_path).
- *at syscalls (openat, fstatat, ...) and getattrlistat need the same path mapping as the plain ones.
- Catalina's system volume has 64-bit object IDs: 32-bit aliases (0xF0000000+n) only for the
  translator's calls; /.vol/<dev>/<alias> paths map back (vol_alias_map).
- fork/exec/posix_spawn (spawn.c): i386 children run as rhost again; DYLD_ variables of rhost
  must not leak into them (conv_env).
- Workloops (kevent_id) must be emulated (wq.c), else 32-bit libdispatch reaches the host kernel's
  workloops; WORKQ features 0x400000df.
- Mojave i386 shared cache: dyld maps it privately (R2_DYLD_SHARED_REGION=private); dyld uses a cached
  library only if the file's inode/mtime match the cache: stat reports the cache's values (sys.c).
  Catalina still ships i386 slices of libSystem/libgcc_s: never let them in (map_path refuses host Mach-O files).
- OOL descriptors: 32-bit layout is {address, size, flags}; 64-bit {address(8), flags, size}.
- Mach trap 43 is map_fd on SL but mach_generate_activity_id on 10.7+.
- psynch_* take 64-bit arguments (two words): sysconv.c wide_call + sys8 (7th/8th args on the user stack).
- __ulock_wait/wake 64-bit value; kevent_qos data_available is size_t*; MACH_RCV_LARGE retry needs
  the message kept (mach.c stash).
- 10.9+ libpthread: bsdthread_register returns feature bits and gives tsd_offset / mach_thread_self
  offset; the kernel stores the new thread's port in its TSD (pthread_join waits on it) and sets %gs.
- libdispatch needs WORKQ_FEATURE_KEVENT: wq.c emulates the workqueue and the workqueue kqueue
  (host kqueue + pump thread; Mach port events receive the message for the 32-bit world).
- vm_map with a memory entry: keep the entry's protections (no PROT_EXEC added).
- mach_vm_deallocate of a thread's own stack below the stack pointer: keep 16 KB (trap frames live there).
- (SL mode) vm_region 32-bit reply has no RetCode; map_fd emulated; guest messages are big-endian NDR
  (find the NDR record after the descriptors); readable 32-bit memory executable; liboah getenv guard;
  liboah maps the PPC commpage itself; exception ports kept in exc.c; signals delivered by sig.c.

## Next
- GUI apps on Catalina (TextWrangler, AppleWorks, Hex Fiend, Word 2004, Pages): window server bridge.
- Upcalls (host -> 32-bit) for CGEventTap, CoreDrag, _LSScheduleNotificationFunction.
- Bridge GL/CGL for OpenGL apps.
- Move sources into the repository (src/rhost, bridge) when stable.

## sysctl KERN_PROC: 32-bit kinfo_proc for the guest (2026-11-07)
Project Gallery Launcher -> Word crashed ~9 s after start (gs-fault loop in a lock routine, then a
NULL deref in Word). Cause: rhost passed sysctl CTL_KERN.KERN_PROC straight through, so the i386
guest got the kernel's 64-bit kinfo_proc (648 bytes/process) where it expects 492. liboah's
oah_office_running_other() walked garbage pids, saw no other Office 2004 process, and
oah_office_shmem_fix() unlinked Project Gallery's live ShMem*/Mso11*/PT_* objects; the new Word then
disagreed with PGL's shared state. Fix: sys.c converts KERN_PROC answers to xnu's user32_kinfo_proc
(kinfo32.h; layout checked field by field against the i386 header: size 492). R2_KPROC64=1 restores
the pass-through. PGL 5/5 x2, Word 10/10. R2_SHMTRACE=1 logs shm_open/sem_open/shared mmaps.

## Skitch, Wireshark: not translator bugs (2026-11-07)
- Skitch on this Mac is an i386-only build (no PowerPC slice). Rosettus started the PowerPC dyld on
  it, which read the "main executable" header at 0x38001000 and crashed (SIGBUS 0x3800101c). Running
  it as i386 under rhost (Mojave i386 root) gets past dyld (mjroot now also has libssl/libcrypto
  0.9.7/0.9.8, Cocoa, WebKit closure) but Mojave's i386 AppKit cannot use Catalina's WindowServer
  (MIG_BAD_ID, see State) and needs dlopen'd private frameworks: 32-bit Intel app support is out of
  scope. The launcher (mkrun.sh) now refuses executables without PowerPC code with a message,
  exit 86 (OAH_ALLOW_NOPPC=1 skips the check).
- Wireshark (PowerPC GTK/X11 build) needs an X server: XQuartz is not installed on the test Mac
  (installing it means accepting its licence: left to the user). The launcher now says so.

## Mactracker: CF's Mach port queue stuck (open, 2026-11-07)
- Every launch on Catalina: the "All Models" list shows 1 model (Mojave: 532 at launch); choosing the
  category again shows all 532. Not the update check (same with the update window skipped).
- Separately (most launches, within ~25 s): CoreFoundation's "CFMachPort" dispatch queue
  (__portQueue, CF+0x183638 -> queue; libdispatch-84 layout) has items queued but
  do_suspend_cnt = 0xfffffffe, so it is never drained. Blocked behind it: the main thread
  (__CFMachPortPerform -> dispatch_sync), CFNetwork's host lookup (_CFMachPortCreateWithPort2),
  and __CFMachPortDeallocate's cleanup block on the global LOW queue (waits for the source's
  cancel handler, which runs on __portQueue). The app then answers clicks only intermittently.
- -2 is an extra resume/lock release without the matching suspend. A real over-resume would trap
  (checked: overres.c dies with SIGTRAP under R2 as on Snow Leopard). Ruled out with PowerPC
  tests: atomics in hot and cold code (dqstress.c, gencold.py/coldatom.c), dispatch_sync/async
  stress, suspend+abort+get_state+resume of threads blocked in semaphore_wait (suspwait.c) - all
  pass under R2.
- Tools: R2_MEMWATCH=ADDR+OFF (hex guest addresses; R2_MEMWATCH_LABEL=text checks the object, e.g.
  CFMachPort at +0x3c) - a host thread watches a word, keeps its value history, and when it turns
  negative stops all threads, logs where each one is and asks for a hang dump. The IMAGE:OFF form
  (base from the image's first executable mmap) does not work for guest frameworks (they are not
  mapped through sys_mmap); CF's load address changes between runs, so the plain form needs the base
  from a hang dump of the same run. psym.py (SL) symbolizes guest addresses with a dump's image list.
- R2_MEMWATCH=low24=XXXXXX+OFF / low12=XXX+OFF with R2_MEMWATCH_LABEL: find the object by its label
  at that low address in any 16 MB block / any page. The CFMachPort queue was at ??e37cf0 in 3 runs,
  then elsewhere; with the watcher spinning on a core the corruption did not show in 1 watched run.
  Next idea: catch the moment with a guest-side hook on dispatch_resume/_dispatch_queue_invoke for
  that queue, or compare with a Mojave run's dispatch call pattern.

## Night of Nov 7-8
- blender functional test (tests/blender/run.sh): 28/28 (Mojave 26/26). It died with translate
  fatal "code pool lookup" at 0x4592000: three threads asked for the callppc return stub at once;
  the first built it in CarbonCore's load-command padding, the others found the padding non-zero,
  built private mmap pages and each made its page THE stub - stray pages the translator drops later.
  Fix (liboah_mojave.c oah_callppc_n): a mutex around the stub set-up, and a stub already in the
  padding is reused. The same race exists in the Mojave liboah (not changed there).
- GetProcessInformation (procnames.c test): processAppSpec was the current folder ("Rosettus") for
  every process: the 64-bit record has no FSSpec and FSpMakeFSRef on an empty spec names the cwd.
  Now from the pid (proc_pidpath; for another rhost, its guest program = last argument), and a
  translated tool's own name is its program's, not "rhost". 
- difftest.sh: 9/9 PASS.
- R2_MMAPLOG=LO-HI: log mappings landing in a range with an i386 frame chain (who mapped it).
- Launcher X11 note: only when the load commands name libX11/libgtk-x11 (Blender's text mentions X11).
- Thread churn (tsdtest.c: short threads created and joined in a loop): the 4,096th 32-bit thread of
  a process hung (and every later thread start with it). rhost's thread table (4,096 slots) kept the
  records of ended threads as tombstones for good, with their 8 MB host stacks, and the insert loop
  never found a free slot; each thread also kept its own LDT entry for %gs. Now a tombstone ended more
  than 0.1 s ago is reused with its host stack (a full table waits for one), and ended threads' LDT
  entries are reused (new ones allocated with a spare after them: the table's last entry is the one
  whose %gs faults for good). tsdtest: 25,224 threads in 10 s, 0 TSD mismatches (Mojave 18,050).
  R2_THR_NOREUSE / R2_LDT_NOREUSE / R2_LDT_NOSPARE switch the parts off.
- Watchdog (R2_WATCHDOG=s) now prints each thread's run state / suspend count, the Mach message it
  is in, and registers + frames of threads running outside a trap. R2_MEMWATCH_HW=1: hardware write
  watchpoint (DR0) on the watched word in all threads, each write logged with the guest backtrace
  ([wp] lines). OAH_CFQ_WATCH=183638 (liboah) + R2_MEMWATCH=file: watch CF's CFMachPort queue.
- Mactracker CFMachPort queue: caught 3 times. The over-resume comes from a thread's second
  dispatch_sync on __portQueue returning at once (its per-thread semaphore already signalled). In one
  capture it was an NSThread exiting: TSD destructors -> Security ThreadNotifier -> ~NotificationPort
  -> CFMachPortInvalidate (sync, fast path) -> CFRelease -> __CFMachPortDeallocate (sync, slow path:
  no wait) -> dispatch_resume with the queue locked by a worker -> -1, then the worker's unlock -> -2.
- cfmpstress (CFMachPorts made and released from short threads; passes on SL and Mojave) crashed on
  R2 in about half the runs, each time somewhere else (malloc's tiny region gone, a new thread's
  stack gone, translator pages). R2_MMAPLOG (now also frees, vm_deallocate with the guest backtrace,
  OOL sends/receives, vm_allocate, bsdthread_create/terminate) and R2_MAPWATCH=ADDR found it:
  pthread_join -> _pthread_reap_thread -> vm_deallocate(random address, 0x100800). 0x100800 is the
  stack size 0x81000 byte-swapped. vm_routine's byte-order flag (g_be: the PowerPC side's MIG
  requests are big-endian) was one variable for the whole process; a translator (little-endian)
  request on another thread at the same moment made a PowerPC request be read the wrong way round,
  and the wrong range was freed. g_be is per thread now: cfmpstress 10/10 (was ~5/10). This can hit
  any app that has PowerPC and translator threads asking for VM routines at once.
- A second cfmpstress fault (rhost itself, in vm_routine's thread_get_state): another thread's
  trap frame (it lies on that thread's 32-bit stack) was read after the thread had freed its stack
  in bsdthread_terminate. The ending thread now withdraws its trap frame first, and the frame is
  copied with mach_vm_read_overwrite (cannot fault).
- Tests that crash write crash reports that behave's "reports" step counts for whatever app is
  being tested: do not run crashing CLI tests during a behave run.
- Full behave run (full3, after the g_be fix): 34 of 48 apps at their Mojave best or better (all
  of Office, Mactracker 9/9, AppleWorks, Keynote, Numbers, blender 26/26, the G4 demos, TextWrangler,
  StuffIt, Transmission, xACT ...). Below Mojave: the four Adobe CS2 apps, iWeb and IE (the app
  loses the front), Pages (template chooser stayed), Hex Fiend (the quit question stayed).
- Illustrator once died in rhost: a window flush (CGContextFlush -> SkyLight
  SLSCopyRegionSerializedBytes) read a region's bytes from a guest page that had been unmapped.
  Out-of-line memory the guest sends with deallocate was taken out by the kernel, leaving a hole in
  the 32-bit reservation until the fix-up after the send (seconds, for a send-receive), and host
  allocations could land in it. Now such memory is sent as a copy (deallocate off) and freed by
  rhost after the send: no hole at any time (R2_OOL_KERNELDEALLOC=1: the old way).
- The front loss: switching from Illustrator to another app (open -a Finder, the copy check's
  "activate Finder") takes 20 s on Catalina (switchtest.sh: +20.2 s, twice), then lands in the
  middle of a later step (Save As, the preferences close). IE switches away at once. The
  deactivation event reaches the app only at the 20 s mark. OAH_MAINPUMP=1 (run the native main run
  loop from the event path) handles sources but does not change the 20 s.
- The 20 s switch delay is not steady: 6 of 6 switch tests between 03:50 and 04:07 took 20 s
  (Illustrator x5, AppleWorks x1; Word, iWeb, IE fast), every test after 04:08 was fast (Illustrator
  x5 with and without its welcome dialog, AppleWorks). The Finder (the app being activated) logged
  its SetFrontProcess only at +20 s and its main thread was idle in its event wait meanwhile.
  Tools: cat/switchtest.sh APP WAIT TRACE (ESC=1 closes the start dialog first, POKE=s sends a
  mouse move, SAMPLE=1 samples the app), frontlog.sh, behshots.py (behave with screenshots after a
  step), cfetch.sh (copy a file from the Catalina Mac).
- Peripheral tests (tests/periph/periph.sh) on Catalina, first run: usb_list, serial, threads, midi,
  audio_out, qt_movie pass as on Mojave. Fixed:
  * hid / hidold: crashed reading element values (IOHIDDeviceGetValue). IOHIDLib maps the values
    with the 32-bit IOConnectMapMemory (io_connect_map_memory, MIG 2819): for a 64-bit task the
    kernel maps above 4 GB and hands back the low 32 bits. rhost now sends it as the 64-bit routine
    (2863), maps the memory again below 4 GB (mach_vm_remap, shared) and writes the 32-bit reply;
    the unmap (2820 -> 2864) frees both. 2863 itself (IOConnectMapMemory64) gets the same remap.
    hid 5/5, hidold 3/3 now.
  * qt_play, and the G4 demos' start-up crash (searchForComponents + 787, ~1 in 5 launches): the
    root's (Mojave) CarbonCore read this Mac's Component Manager cache
    (/System/Library/Caches/com.apple.Components2.SystemCache.Components, Catalina's layout) and took
    the text "jscr" for a size. The com.apple.Components2.* caches now come from the root for the
    32-bit world (absent there: the component folders are scanned). R2_HOST_COMPCACHE=1: the old way.
  * qt_play also crashed in liboah's oah_gt_install (a hook written into a page another thread had
    just made read-only again, prot r-x after a successful vm_protect): the hook is now written with
    vm_write and retried ("hook written after 1 retry" seen once in 4 runs). The crash report's
    region line now has the protection.
  Still failing: services fsevents (0 events; liboah's FSEvents hooks are installed but never
  called), diskarbitration reports 0 disks (native 16).
- racetest (5 x 10 apps): 49/50, the one failure the G4 component-cache crash (fixed since:
  G4 demos 32/32 after). smoke.sh: every wrapper up after 30 s except Skitch and Wireshark (known).
- PowerPC Java on Catalina (tests/java/javatest.sh with ~/r2/Rosettus/rjava): 1.3.1_16 and
  1.4.2_21: -version, javac, JTest ("JTest ok 1.4.2_21 threads=5999995 ...") and the Swing window
  work. 1.5.0_19: -version works; JTest and javac crash at once in translate's interpreter
  (b80c5d10: a load "lwz rX,-1(0)" - it runs data) on a new thread (bsdthread_create flags
  0x90008ff: custom stack + SETSCHED); not the SETSCHED (masked: same), not tonight's changes (the
  20261107-0050 rhost: same), -Xshare:off: same. Open. On Mojave 1.5.0_19 passes.
- "[vm] kernel routine 3810 not handled yet" (vm_msync) is printed by Java; harmless so far.
- services: fsevents gets 0 events: liboah's native stream is created and started (FSELOG), the
  fseventsd registration (MIG 66000) answers, but no event message arrives. diskarbitration: 0 disks
  (native 16). Both open.
- soak.py (patched to find ~/Applications/Rosettus on the Catalina Mac): TextWrangler 12 min: 126
  cycles, 0 stalls, 126/126 copies, rss +9.5 MB / 100 cycles. Word 10 min (--start-esc for its
  Project Gallery): 103 cycles, 0 stalls, 103/103 copies, rss flat.
- launchdoc / opendoc: TextWrangler, Word (launch), Hex Fiend open their documents; Word's opendoc
  while its Project Gallery (modal) is up: not opened (Word holds the odoc while the gallery shows).
- Adobe Installer: no wrapper on the Catalina Mac (Mojave behave 2/4).
- vm_msync (MIG 3810) is now answered (mach_vm_msync); PowerPC Java called it.
- Front loss in behave (Illustrator, Photoshop, GoLive, InDesign, iWeb, IE, every run): the copy
  check's `osascript -e 'tell application "Finder" to activate'` takes effect only 20 s later
  (unified log: osascript 07:01:02.3, Finder "SetFrontProcess options=0" 07:01:22.4), in the middle of
  the next step. The Finder's threads are idle in that time (sampled twice), the pasteboard answers at
  once (all types, 0.00 s). An LS/AppleEvent-side wait of 20 s on the translated front app is the
  likely cause; not found yet. Apps that pass (Office, Keynote, TextWrangler ...) get the Finder at
  once.
- Idle CPU (Nov 8 morning). Illustrator idle on Catalina used 27% of a core and never "settled"
  for behave (< 0.1 s CPU per 0.5 s). R2_PROF: 19500 traps/s, 7700/s of them CGEventSourceKeyState:
  liboah's CGSGetKeys (HIToolbox GetKeys, which Illustrator polls 60 times a second) asked the 128
  keys one hostcall each. Now one hostcall (r2_getkeys, hostcall.c) fills the KeyMap; liboah's
  oah_getkeys16() uses it (and the input poller too). Idle: 18%, start-up settles in 10 s (was 30).
  Still ~60 iterations/s of Illustrator's own idle loop (insert_member, a timed receive,
  extract_member, two vm_allocate/deallocate, FindWindow, cursor, front process).
- commpage time thread: Catalina leaves the gettimeofday block's generation at 0 for good; the copy
  loop waited for it with 100 usleep(10) every 20 ms in every translated process (7% of that
  thread's samples). Now it gives up at once (the guest's gettimeofday makes the system call).
- TCC: every translated app is the same rhost binary, so a privacy prompt names whichever app TCC
  saw first ("Microsoft Word would like to access files in your Desktop folder" during an
  Illustrator test), and a rebuilt (unsigned) rhost asks again. A pending prompt sat on screen
  during behave runs; answered (OK). Not the cause of the 20 s switch (still there without it).
- TextInputMenuAgent logged "CFMessagePort: dropping corrupt request Mach message (0b100010)" (bad
  magic, wrong size) each time a translated app came to the front: the guest's PowerPC CF (10.4/10.5)
  sends CFMessagePort messages in the old layout (msgh_id = conversation id, {0xF1F2F3F4, msgid,
  byteslen, bytes}); today's CF wants room for an OOL descriptor and {0xF0F2F4F8, msgid, convid,
  byteslen}. rhost now sends the old layout in today's (msg_to64, "[cfmp]" logged once) and turns
  today's layout back into the old one on receive (msg_to32), so replies and messages between two
  translated processes still read. The TextInputMenuAgent warnings are gone.
- The 20 s switch, more data (R2_MSGLOG=1: every message sent/received with wall time):
  * the Finder receives the 'rapp' (open -a) at once and calls SetFrontProcess 20.1 s later while
    its main thread is idle; Illustrator receives nothing at +0, in slow and fast runs alike;
  * intermittent: open -a Finder 1 slow in 3, then 3 fast in 4; Cmd-Tab 3 of 3 fast; open -a
    TextEdit fast. Only activations another process asks for wait.
- The 20 s switch is Catalina's, not Rosettus': a native TextEdit in front for 5 min, then
  `open -a Finder`: Finder in front after 20.17 s (natsw.sh). Every batch had its first run slow and
  the next ones fast: an activation another process asks for waits 20 s when the Finder has been in
  the background for a few minutes; Cmd-Tab (a user's switch) is always fast. behave.py's copy check
  now switches away with Cmd-Tab (osascript's Finder activate only if that did not change the front).
  Mojave's behave never hit this.
- Full behave run (full4, Nov 8 11:44-13:00) after the Cmd-Tab copy check: all apps at or above
  Mojave except Photoshop 9/12 (its New Document never came up this time; 12/12 forty minutes
  before), Keynote 8/11 (Theme Chooser left up; flaky in Mojave's history too) and SimCity 6/9
  (Mojave's 4/4 is from the older 4-step test). Acrobat 3/4 (Mojave 2/4), blender 26/26.
- FSEvents fixed: the i386 FSEvents client registers with this Mac's fseventsd and never gets an
  event, so streams now run on the host (hostcall.c r2h_fse_start/poll/stop) and liboah's thread
  moves their events into its queues every 0.1 s (oah_fse.h, OAH_R2_NATIVE_FSE=1: the old way).
  services fsevents: PASS 4 of 4 (was 0 events).
- DiskArbitration: disk-appeared callbacks now fire: when a session has one and is scheduled (run
  loop or dispatch queue), each /dev/diskN and diskNsM is reported once (oah_gtrace.h
  oah_da_deliver; the match dictionary is not applied; OAH_NO_DAAPPEAR=1: none, as before).
  services diskarb: 15 disks reported (/dev has 15), was 0 (on Mojave too).
- Photoshop 9/12 (no New Document: "Could not create a new document because the ICC profile is no
  longer installed", every New of the session): a test-sequence effect, also in Mojave's history.
  Photoshop's New takes size and color profile from a picture on the clipboard (Illustrator's copy),
  remembers the profile, and its next session refuses every New (reproduced: Photoshop 12/12,
  Illustrator 11/11, Photoshop 12/12, Photoshop 9/12). behave.py now starts every app with a
  plain-text pasteboard: Illustrator, Photoshop, Photoshop all full. (The bad prefs file was moved
  to the Catalina Mac's ~/rosettus-work/backup/ps-prefs-1350.)
- Keynote 8/11 in full4 was the Theme Chooser flake: 11/11 twice right after.
- SimCity's APlayer (a helper app in its bundle) stayed running after behave killed the game and
  made behave_all skip the next SimCity run; behave_all now kills anything inside the guest bundle.
  SimCity 6/9: the generic copy / hide / quit steps do not fit the full-screen game (Mojave only
  ran the older 4-step test).
- PowerPC Java, state on Nov 8 afternoon:
  * thread_get_state(x86_THREAD_STATE32) of a thread created but not yet in 32-bit code (Java creates
    its threads suspended and reads their state at once) now answers its initial registers, as the
    kernel would (thread.c g_pend, mach.c 3603); it was KERN_FAILURE. 1.5's crash is unchanged by it.
  * 1.3.1 and 1.5 look for a class-data-sharing archive (classes.jsa) that the Rosettus install does
    not have and run update_sharing (/System/.../Versions/1.3/Commands/update_sharing, which on the
    installed tree is the jikes-only 1.3 folder): "update_sharing command failed to execute", then
    exit(2) (JTest, javac). With sharing off (-Xshare:off, or a jsaInProgress marker in Libraries)
    both crash in compiled-code paths: 1.5 at b80c5d10 (null context in the translator), 1.3.1 by
    jumping into the JIT's code area (eip 0x59xxxxxx, sig 10). 1.4.2 says "HotSpot not at correct
    virtual address. Sharing disabled." and passes JTest. In the 06:05 run 1.3.1 passed: an archive
    may have existed then. vm_msync on or off (R2_NO_MSYNC=1) makes no difference. Open.
- behave_all.sh kills everything inside the tested guest bundle afterwards (SimCity's APlayer).
- Java 1.3.1 JTest (Nov 8 evening): crashes every run (sig 10, eip in a freed 26 MB region the JVM
  had vm_allocate'd and vm_deallocate'd itself: [mmaplog] deallocate trap now logs the 32-bit and
  PowerPC caller chains), but passes under R2_TRACE=1 or R2_MMAPLOG (timing). The child of its
  fork (update_sharing) failing is harmless. R2_THR_NOREUSE / R2_LDT_NOREUSE: no change. Next: who
  jumps into that region (x86 eip = a guest data address) - the translator's dispatch to a stale
  target after the JVM unmaps and remaps memory (code cache?), a race only seen without tracing.
- Mactracker intermittent (6/9 about 1 run in 2): its Sparkle "Software Update" panel stays up and
  the app takes no keys at all in those runs (Esc x3, typing, Cmd-H all ignored; the copy finds the
  old pasteboard). Not a test timing issue (Esc retried 3 times over 4.5 s: no change).
  OAH_EVTRACE: in those runs the guest stops pulling events (17 CGEventCreateNextEvent calls vs
  300+). Guest-state dumps (behave.py BX_HANGDUMP=1 sends SIGUSR2 when the panel survives Esc;
  OAH_HANGDUMP=600 OAH_HANGNAMES=1 in the wrapper's rosetta.env) show the main thread 20+ s in a
  nested CFRunLoop run by CommerceCore (+0xd28c0, Mactracker's update / registration framework,
  called from an HIToolbox event handler), while CommerceCore threads (one in WebCore) and a
  CFNetwork thread wait: it is waiting for the update panel's network load (release notes), so the
  app takes no input until that finishes. Depends on the update server / network; whether a lost
  CFNetwork/socket callback under Rosettus 2 is involved is not known yet. Open.

## cfnet first-fetch "TIMEOUT": stock Rosetta loses f30/f31 (2026-11-08)
- cfnet's first fetch timed out at once with t0 = 0.000. t0 sat in f31; CFReadStreamOpen came back
  with f31 = 0. tools/fpsave.c and tools/fphost.c narrow it to CFHostStartInfoResolution (sync host
  lookup): afterwards f31 = 0 and f30 = 9000, f29 kept. Thread create/join, run loops, getaddrinfo,
  gethostbyname, kqueue, sockets and SCDynamicStore keep f14-f31.
- The same binary fails the same way under stock Rosetta on Snow Leopard and under Rosettus on Mojave,
  so it is the translator's behaviour, not Rosettus 2's (parity). During the lookup the translator
  sends itself SIGTRAP ~40 times (__pthread_kill(self, 5), handler in translate), most handlers not
  ending in sigreturn - a likely place for the lost FPRs. Not chased further.
- cfnet keeps t0 in memory now: 9/9 fetches ok on Catalina (example.com and the Mactracker update
  server, 2 callbacks each). The async CFReadStream path works; the Mactracker panel stall is elsewhere.
- Side find (not the cause): exc.c ts_load points the float state at the short mcontext layout even
  when the trap's ucontext is the full one (sig.c host_fpu handles both). Fixed to use the same check.

## Mactracker stall = CF's port queue over-resumed; liboah repair (2026-11-08)
- The "CommerceCore +0xd28c0" frames in earlier dumps were misnamed: CommerceCore's PowerPC text is
  28 KB; the addresses are liboah's hook stubs in the gap after it. The main thread was in
  HIToolbox -> CFRunLoopRunSpecific (hook) -> __CFRunLoopRun -> __CFMachPortPerform ->
  dispatch_barrier_sync_f_slow, i.e. the CFMachPort queue problem above, not a network wait.
- The hang dump now prints the event pump: "[hang] events: port .. msgs 0 pending 1 queued 10 (in 15
  out 5 ...)": the wake-up was taken, the guest pulled 5 events and then blocked on the queue.
- 6 runs with dumps: 2/6 at 9/9; all 4 failures the same (main thread in __CFMachPortPerform's
  dispatch_sync, CFNetwork in _CFMachPortCreateWithPort2, a worker in __CFMachPortDeallocate's block).
- liboah (oah_hang.h oah_cfq_repair, watcher thread, every second): CF+0x183638 -> __portQueue
  (checked by its "CFMa..." label); do_suspend_cnt (queue+0x10, big-endian) negative for two checks
  -> CAS to 0, then the next bridge call of any guest thread runs dispatch_async_f(queue, NULL, free)
  to wake it (oah_cfq_wake_now). A first version called dispatch_async_f from the native watcher
  thread and crashed in pthread_getspecific (no guest TSD there). OAH_NO_CFQFIX=1 disables.
  The root cause (a dispatch thread semaphore already signalled) is still open.
- exc.c: thread_get/set_state's float state now follows the full-mcontext layout (as sig.c).
- Result with the repair: Mactracker 7/8 runs at 9/9, no crashes (before: 2/6). In the 8th the
  repair fired and woke the queue, but the app stayed unresponsive (busy 30 s at start-up, Software
  Update panel stayed): not every case is recovered; needs a dump of such a run.

## Java 1.3.1 JTest crash: a translator code-cache flush under a running thread (2026-11-08, open)
- Crashes 20/20 now (sig 10, x86 eip in an unmapped 26 MB region, always ~0x69ec00 into it).
- R2_MMAPLOG=50000000-60000000: the translator keeps its code in 26 MB chunks (plus 2 MB ones).
  Whenever the JVM patches compiled code (guest r4 = 0x48000000, a branch, at r3 = its code heap),
  the translator frees the whole current chunk (vm_deallocate from translate 0xb814e258) and starts a
  new one: 3-5 flushes per run. The crashing thread is one that was running translated code in the
  chunk at that moment, typically a thread started just before (it may have allocated that chunk).
- Not signals: the translator's 100+ __pthread_kill calls per second are all a thread signalling
  itself with SIGTRAP; every one is delivered (R2_SIGLOG=1). No thread_suspend/abort/set_state is
  sent around a flush (R2_TSLOG=1). So the translator relies on the other threads being stopped
  by the JVM when it patches; on Mojave and Snow Leopard they are.
- Lead: the JVM creates threads suspended, reads their state (thread_get_state PPC), sets a
  special port and resumes them. For those threads rhost answers STATE32 with the initial
  registers (thread_pending_state) and liboah's PPC answer finds no guest register file
  ("[gc] ... no guest state found"), so the JVM gets an error there. Next: what the JVM does with
  that (thread treated as not a Java thread -> not stopped at the safepoint?), compare with Mojave.
- New switches: R2_TSLOG=1 (thread routines 36xx with caller and time; get/set state results),
  R2_SIGLOG=1 (kill/__pthread_kill, signals pended in host code, deliveries, sigreturns).
- mach_msg: the host wrapper retried *_INTERRUPTED by itself, so a guest signal for a thread
  blocked in a receive waited for a message. Now the interruption goes back to the guest when one
  of its signals is deliverable (the guest's handler runs, its mach_msg wrapper retries), as the
  kernel does. Not the Java crash (never hit there). R2_NO_MSGINTR=1: the old way.
- Regression after the mach_msg interrupt change + CF queue repair: Keynote 11/11, Hex Fiend,
  Transmission, TextWrangler, AppleWorks 6 9/9 (no change). Mactracker 8 of 11 runs at 9/9 since
  the repair; in the failures the repair fired ("queue woken") but the app stayed stuck: waking the
  queue is not enough there - another dump of such a run is needed.

## Java: translator code-chunk quarantine (2026-11-08 night)
- mach.c code_quarantine(): a mach_vm_deallocate of 1-64 MB made by the translator itself (a return
  address in translate's text 0xb8000000-0xb8200000 on the 32-bit frame chain) is kept mapped (and
  reserved) for 3 s, then freed by a reaper thread. Threads still running the old translated code
  finish it and return to the dispatcher instead of jumping into unmapped memory.
  First version had no upper limit and caught translate's start-up release of 0x1000-0x80000000:
  every launch failed for ~3 minutes (some of full5's first apps may show it). Now <= 64 MB.
  R2_NO_CODEQUAR=1 / R2_CODEQUAR_SECS=n.
- Java 1.3 JTest: 10/10 ok (was 0/20). Java 1.4: 5/5. Java 1.5: still 5/5 crash, now in the
  translator (b80c5d10, null context): the JVM reads the PowerPC state of threads that have not run
  yet; liboah finds no register file and passes the call on. liboah (oah_gtrace.h, OAH_R2) now answers
  zero registers for such a thread (x86 eip set, ebp 0, nothing noted). OAH_NO_NEWTHRSTATE=1: old way.

## Night of Nov 8-9: full5 and follow-ups
- Rosettus for Mojave 1.1 released (Mojave repo 71a7f18): the menu-bar re-check and no-shadow fixes
  from here. Mojave's own full run on 1.1 matched 1.0 everywhere.
- full5 (Catalina, all of today's fixes; the code quarantine went live during it): at or above
  Mojave everywhere except Keynote 5/9 and Numbers 8/11 (both 11/11 twice right after: transient)
  and SimCity (behave 6/9 vs Mojave's sc4test 4/4: different tests; sc4test run separately).
  Photoshop 12/12 and Mactracker 9/9 now. Skitch is an Intel-only app (Rosettus refuses it,
  correctly); Wireshark needs XQuartz (not installed; its EULA is not ours to accept).
- Code quarantine threshold: 1 MB (to cover the translator's 2-4 MB tables) broke Java 1.3 again
  (crash in a freed 26 MB chunk); back to 16-64 MB: 1.3 5/5, 1.4 5/5.
- exc.c ts_load: a thread created and not started (or created suspended) now answers every flavor
  with the registers it will start with (and a reset FPU state); thread_set_state on it changes
  them (thread.c thread_pending_update; host_thread enters with the recorded values). The
  liboah zero-register answer is opt-in now (OAH_NEWTHRSTATE_ZERO=1): Mojave passes these calls
  to the translator too.
- Java 1.5 JTest still crashes (5/5): the translator's interpreter executes lwz r10,0(r10) with
  r10 = 0 (b80c5d10, a guest null read - HotSpot's implicit null check style), and the fault ends
  as a crash. Mojave's JTest never reaches it. Open: find which value is 0 here and not on Mojave
  (the polling page / a thread's state read at creation).
- Java 1.5 crash, traced (R2_EXCLOG=1, R2_MSGLOG=1, R2_SIGLOG=1 now log exception ports/raises,
  messages and SIGSEGV/SIGBUS/SIGTRAP sigactions without full tracing; the liboah crash report now
  prints an interpreter state found in esi/edi/ebx/eax: guest pc, code, registers):
  * guest code at 0xafe5770c (JVM-generated): `li r10,-1; lwz r10,0(r10)` - a deliberate fault
    (HotSpot's planned trap style). The translator's interpreter faults at b80c5d10.
  * rhost sends EXC_BAD_ACCESS to the translator's task exception port (mask 0xe, state-identity,
    flavor 1); the translator's handler thread then forwards a 2403 message to port 0 (send ->
    MACH_SEND_INVALID_DEST) and replies failure; rhost falls back to SIGSEGV, whose only 32-bit
    handler is liboah's crash handler (the translator never installs a SIGSEGV handler; no
    "kept behind" either) -> CRASH.
  * Port 0: rhost's task/thread_get/swap_exception_ports report no earlier handlers. On Snow Leopard
    there is one (the crash reporter's); what the translator does after that forward is not known.
  * Mojave 1.0's known issue "Java dereferencing null crashes the JVM" is likely the same path;
    Mojave's JTest simply never reaches such code. Not fixed.
- full6 (all of tonight's changes): at Mojave's level everywhere except Keynote 8/11 and
  Mactracker 6/9 (SimCity: sc4test 4/4 as Mojave). Re-runs: Keynote 11/11 x2, Mactracker 9/9 then
  6/9. Both are the intermittent "app stops taking input" stall: Mactracker's with the CF port-queue
  repair firing ("woken") but the app staying stuck; Keynote's without it (Theme Chooser up,
  typing / Cmd-H ignored, "busy" 10-20 s) - a different cause, not found yet; only seen in long
  full runs so far. Next: BX_HANGDUMP=1 + OAH_HANGDUMP on a looped Keynote run to catch one.
- mach.c: mach_vm_purgable_control (4818) passed to the kernel quietly like 3830 (Keynote logged
  "kernel routine 4818 not handled yet"; it was passed on anyway, the layout is the same).

## Command-line tools (2026-11-09)
- Both versions run PowerPC command-line tools with `rosettus TOOL ARGS` (Mojave: /Library/Rosettus or
  ~/Library/Rosettus; Catalina test tree: ~/r2/Rosettus/rosettus). tools/clitest(.c) checks arguments
  with spaces, environment, cwd, relative path, piped stdin, a terminal (script -q), stdout/stderr,
  exit status (passed through) and SIGINT (-2 at once). All pass on both.
- Fixed on both (liboah, Mojave commit b2e06fa, unreleased there): with stderr redirected to a file,
  Rosettus's [pathmap] / [exit] lines went into it; a regular file now counts as the user's stderr
  unless it is a Rosettus log (~/Library/Logs/Rosettus/...), and the lines go to tools.log.
  My own tests that capture a tool's stderr in a file need OAH_TOOL_STDERR=1 now (j13loop.sh has it).
- rhost: [codequar] logs only with R2_TRACE / R2_MMAPLOG now (it reached tools' stderr).
- Documented: Mojave README "Command-line tools"; README.md here (new: Rosettus for Catalina status,
  test-tree layout, apps, command-line tools, Java, diagnostics).
- Java 1.5: the two 32-bit signal handlers in the process are liboah's (segv_handler, SIGTRAP
  bridge_trap_handler); the translator uses only its Mach exception port. Its handler forwards this
  fault (inside its interpreter) to the "earlier" task exception port - none, as rhost reports - and
  gives up. Same flaw as Mojave's known Java-null issue: left for a later, deliberate dig into
  translate's exception path.
- rosetta.env is sourced by the launcher: a line needs `export NAME=value`. Plain `NAME=value` lines
  (as I added for OAH_HANGDUMP / OAH_HANGNAMES in the Mactracker and Keynote loops) never reached
  the app; SIGUSR2 dumps worked anyway (the handler is always installed).
- Keynote loop without any diagnostics active (knhd, 8 runs): 6 stalls (type+copy, hide/show, quit
  fail: the app ignores keys and Cmd-H). With a dump every 15 s: no stall (timing). OAH_EVSTAT=1
  (liboah, oah_hang.h): event pump state once a second, no thread stopped.

## Keynote / Mactracker stall: semaphore hooks off on Catalina (2026-11-09)
- Keynote stall (6 of 8 behave runs: type+copy, hide/show, quit fail) traced to liboah's
  semaphore-trap hooks (oah_gtrace.h, made for GoLive's Save deadlock). A/B: hooks off
  (OAH_NO_SEMWAITFIX=1) 6/6; OAH_SEMW_NOPLAIN=1 0/4; OAH_SEMW_NATIVE=1 3/4.
- OAH_SEMWTRACE=1: no callback-lock release at all in Keynote (guest threads call without the lock
  since Nov 7, so the hooks only ever pass through). The hang dump: main thread in pthread_mutex_lock
  +0x204 (semaphore_wait on mutex+0x28 with the spin lock held), two woken waiters spinning on that
  spin lock (+0x268), mutex waiters 2-3; the token on +0x28 never came.
- OAH_SEMRING=1 (+ OAH_SEMRING_GUEST=1, or with OAH_SEMW_NATIVE): ring of the last 4096 semaphore
  waits (thread, kind, semaphores, caller, result), printed by the hang dump. A caught stall showed
  a waiter at +0x268 with no matching wait in the ring and a semaphore (12403) in use as both a
  condition's and a mutex's at once; the exact way the hooks break it is not pinned down.
- Fix (OAH_R2 only): the hooks are off unless OAH_SEMWAITFIX=1 or OAH_CB_GUESTLOCK=1 (the case they
  were made for). Rosettus for Mojave keeps them (its guest threads still take the lock; GoLive needs
  them there).
- Results with the fix: Keynote 11/11 x6, Mactracker 9/9 x4, GoLive 12/12 x2 (Save), InDesign 11/11
  x2, Numbers 11/11, TextWrangler 9/9: all clean.

## Java 1.5 fixed (2026-11-09)
- Cause: PowerPC Java 1.5 maps its safepoint polling page with mmap(0, 4096, PROT_READ, MAP_ANON)
  (neither MAP_PRIVATE nor MAP_SHARED). Old Darwin took that as private; Catalina's kernel answers
  EINVAL. The JVM kept the -1, and C1 compiled every poll as "li r10,-1; lwz r10,0(r10)" (Mojave:
  "lis r10,0x01df; ori r10,r10,0xf000; lwz r10,0(r10)"): the first compiled loop
  (java.io.UnixFileSystem.normalize) faulted at 0xffffffff in the translator's interpreter - not a
  null check the JVM handles - and the JVM crashed. -Xint died of a SIGTRAP instead (same page).
  The exception-port forwarding to port 0 seen before was a consequence, not the cause.
- Found by comparing the compiled code: OAH_SCANPAT=w1,w2,.. (liboah, both versions; big-endian
  words) prints the matches in readable memory with 40 words around them, at a guest exit, a crash
  or a hang dump (SIGUSR2); OAH_SCANPAT_MS=N also after N ms.
- Fix (rhost sys.c): MAP_ANON without MAP_PRIVATE/MAP_SHARED gets MAP_PRIVATE (R2_NO_ANONPRIV=1:
  the old behaviour). Java 1.3 / 1.4 / 1.5: -version, javac, JTest, SwingTest all pass.

## Mactracker listed one model: REALbasic strings above 2 GB (2026-11-09)
- Symptom: All Models showed one row (Mojave 532). The data loaded; RuntimeListAddRow ran 17 times
  (Mojave 1260) and a NilObjectException (RaiseNilObjectException at 0x6aad64) ended the fill.
- MainWindow.GetModelPreviewIcon: f = BundleResourceFolderMBS(App).Child(":Icons:" + name); if
  f.Exists, OpenAsIconFamilyMBS(f), whose nil result raised. On Mojave f.Exists is true too and
  ReadIconFile reads Resources/Icons/<name>.icns; on Catalina ReadIconFile got the FSSpec of the
  Resources folder itself and failed (-43).
- HXRuntime's FolderItemImpMac::GetChildByName calls FSMakeFSSpec(vRef, dirID, name) with the name
  converted to the system encoding as a Pascal string; on Catalina the length byte was 0 (an empty
  name gives the folder itself). The conversion (TECConvertText) was right.
- Cause: string::AllocateBuffer puts the text at (block + 0x14) + ((block + 0x15) & 1), the halving
  done signed (srawi; addze). For a malloc block above 2 GB that gives -1 instead of 1: the length
  byte lands at block + 0x13, which the string's encoding word (block + 0x10, written later) then
  overwrites. Under rhost the guest heap is above 2 GB (the translator's mmap hints are free there;
  on Mojave and on real Macs the heap was always low). The same broke "System" in
  GetEncodingFromFont (UpgradeScriptInfoToTextEncoding -50, so the system encoding fell back to
  MacRoman variant 0 instead of 0x20000).
- Fix (liboah, oah_gtrace.h oah_rbstr_patch): in images named HXRuntime / rbframework, the
  srawi/addze pair after "addi rD,rX,0x15" in string::AllocateBuffer becomes srwi/nop (the result it
  always had below 2 GB). OAH_NO_RBSTRFIX=1 disables. Mactracker: 532 models with icons.
- New tracing aid: OAH_GTRACE="Lib:+OFFSET" hooks an unexported function at an offset from the
  library's header (libraries slide between launches, so abs: addresses only fit the main program).

## X11: shmat below 4 GB (Wireshark, 2026-11-09)
- With XQuartz 2.8.5 installed, GTK Wireshark crashed as its window came up: memmove in
  libgdk-x11 (gdk_x11 image code) to 0x10191000, an unmapped address. GDK's MIT-SHM images: shmat
  went through to the kernel unchanged, which attached the segment above 4 GB, and the 32-bit side
  kept the low half of the address.
- Fix (rhost sys.c): shmat attaches in the 32-bit space (low_alloc, or the asked address if free;
  the reservation's PROT_NONE pages are unmapped for it), shmdt gives the pages back to the
  reservation, shmctl(IPC_STAT) converts struct shmid_ds to the i386 layout. R2_SHMTRACE=1 logs
  the attaches.
- Result: Wireshark 1.9.2 opens a capture (packet list, details, bytes); XQuartz started by
  Rosettus. tshark / capinfos (XYLD_FALLBACK_LIBRARY_PATH=Wireshark.app/Contents/Resources/lib)
  read the same file.

## Mojave export from the installer, tested for real (2026-11-09)
- With "Install macOS Mojave.zip" (10.14.6 18G103, 6 GB) on the Catalina Mac, the first version
  (pkgutil --expand-full of Core.pkg) stopped after 3 minutes: "Could not unarchive ... Core.pkg
  (Permission denied)". Without root, pkgutil cannot finish the payload's folders that have no
  write or search permission for the owner (private/var/db/timezone, dslocal/nodes/Default), and
  its leftovers needed chmod -R u+rwX to delete.
- Now: xar -xf Core.pkg Payload (4.9 GB), then payload_extract.py reads the pbzx stream (xz chunks
  decoded with /usr/lib/libcompression, COMPRESSION_LZMA, from the stock Python 2.7 through ctypes:
  no tools to install) and the odc cpio archive in it, and writes only the listed files and the
  symbolic links below System/Library and usr (1188 files, 5689 links, 1.4 GB).
- Whole run 6.5 minutes after the unzip (unzip about 3); the cache built by the installer's
  update_dyld_shared_cache: 567 dylibs (as before from the simulated root). Export 607 MB; the
  exporter reports 2 files that differ from the tested 18G103 checksums (informational only).
- With R2_ROOT32 pointing at that export: Mactracker (532 models), TextWrangler, Keynote (theme
  chooser, OpenGL), Java 1.4 / 1.5 -version and JTest all as with the Mojave-made export.

## Installed package vs test tree: a stale Snow Leopard export (2026-11-09)
- The behave run on the installed package (full8, full9) aborted GoLive, Illustrator, InDesign
  ("the test app was not in front": lsappinfo front = Finder right after typing and pasting) and
  Photoshop's copy came back empty; the same apps passed from the test tree at the same time
  (bis1: Photoshop 12/12, InDesign 11/11).
- /Library/Rosettus had been installed from an old Rosettus-SL-export.tar.gz (from an earlier
  exporter): 10453 files of the test tree were not there (usr/X11, QuickTime codec components,
  AppleSpell.service, Java, kexts). A fresh export from the Snow Leopard Mac with this release's kit
  (122628 entries, 0 missing, 1.25 GB) and a reinstall (uninstall.sh --system, then the package
  with both exports next to it): GoLive 12/12, Illustrator 11/11, InDesign 11/11, Photoshop 12/12 (bis2). Most
  likely cause: the spelling service missing (the apps check spelling as one types).
- install.sh now counts the manifest's files missing from an export and warns (0 for a current
  export and for the test tree).
- Also from this: the package's postinstall updates an existing install in place (install.sh
  --update); a new Snow Leopard export needs uninstall.sh --system first (or install.sh with both
  exports).

## SimCity froze on the region view: Apple Event replies taken by the receive thread (2026-11-09)

Reported by hand: SimCity 4 showed the region view but nothing moved and clicks did nothing.
The hang dump (SIGUSR2) had the main thread in `AESendMessage` (via `oah_aeout_call`, send mode
kAEWaitReply), waiting in its run loop for the reply to 'APlr'/'PIsP', an event to the game's
sound helper APlayer. APlayer had answered, but the AE trace showed the 'aevt'/'ansr' reply
arriving at liboah's own receive thread (`oah_aen_thread`), which listens on the same port to
deliver incoming events to the guest; it handed the reply to the guest as an ordinary event
(-1708) and the main thread never saw it. On Mojave timing seems to have hidden this.

Fix (`src/liboah/oah_aen.h`): a synchronous send (mode & 3 == kAEWaitReply) "parks" the receive
thread: the sender raises `g_aeo_wait`, waits until the receive thread is out of `mach_msg`, and
lowers it after the reply came back. The receive thread now waits in `mach_msg` with a 25 ms
timeout and does not start a new receive while a sender is waiting. `OAH_NO_AEOPARK=1` turns it
off. Checked: the region view animates (frames differ when idle), hover works, and a click on the
"Getting Started Tutorial" city opens its information bubble.

## Five behavior-test aborts on the installed package: the harness activated the wrong launcher (2026-11-09)

Photoshop, Entourage, PowerPoint, Word and Equation Editor stopped with "the test app was not in
front" in the full run on the installed package, after all their functional steps had passed. The
test Mac has two sets of launchers with the same bundle ids (the test tree in ~/Applications/Rosettus
and the installed one in /Applications/Rosettus), and behave.py's fallback activation,
`tell application id "<bundle id>" to activate`, could pick the other one, so the running app never
came back in front after the copy check had brought the Finder forward. Native apps were not
affected, and `tell application "<launcher path>" to activate` brings the running app back at once.

behave.py now activates by the launcher's path, and its front checks wait up to 2 s for the test
app to come back (logged when that happens). Rerun on the installed package: Word 10/10,
Photoshop 12/12, Entourage 9/9, PowerPoint 10/10, Equation Editor 9/9. A user's Mac has one set
of launchers, so this concerned only the test setup.

## G4 Quasar played no music: AudioUnit output never reached the speakers (2026-11-09)

Reported by hand: no music in G4 Quasar on Catalina (it plays on Mojave). Its chiptune is an
NSSound (22050 Hz mono), which 10.6's AppKit plays through the default output AudioUnit, bridged
by liboah (`oah_au.h`). On Catalina that bridge handed the samples to Mojave's i386 output unit
(CoreAudio.component, registered in-process), which initializes and starts without an error but
finds no output device and never asks for a frame (OAH_AUTRACE=1: `renders=0`). The Sound
Manager had met the same wall earlier and plays through this Mac's own 64-bit output unit
(`r2_audio_ring`, a 44100 Hz host ring); AudioUnit output had not been moved over.

Fix: on Rosettus for Catalina, running guest output units are mixed into the Sound Manager's
host ring (`g_snd_au_mix` in `oah_snd.h`), and a unit fed at another rate is resampled to
44100 Hz (linear interpolation in `au_take`). OAH_AU_NATIVE=1 restores the old path. Checked:
Quasar's music plays (confirmed by ear), the ring stays filled (a handful of underruns at
start-up), and Bejeweled 2, Kid Pix and Mactracker still pass their behavior tests.

## Build-a-lot 2 crashed on "Play level", slow page transitions: a mode switch QuickDraw never saw (2026-11-09)

Reported by hand: Play Campaign, the tutorial, then "Play level" crashed, and the title screen's
page transitions were very slow. The crash was in the game's CDirectSurf::ConvertCopyPixels,
reading past a 1024x768 RGB background (CxImage PNG decode) while filling a surface 1792 pixels
wide. The game switches the display to 1024x768 and sizes its screen surfaces from QuickDraw's main
GDevice (DIRECTDRAW2::GetDisplayMode: GetMainDevice + GetPixBounds). 10.6's QuickDraw updates the
GDevice from a display reconfiguration callback that this Mac's WindowServer never delivers to the
guest, so it kept the desktop's 1792x1120: every frame was drawn at 2.5 times the pixels, and the
level background was copied into a surface wider than the picture.

On top of that the built-in Retina display gets another display ID in a mode without HiDPI
(0x7bd7eef9 became 0x04281106 at 1024x768), a moment after the switch returns, and the old ID still
answers with the old mode.

Fix (`liboah_mojave.c`, `oah_cg_switch_mode`): after a successful switch, wait until this Mac's
main display is in the new mode (and has its new ID, or kept its ID for half a second), set the
guest's main GDevice (gdRect, PixMap bounds and rowBytes) to the new size, and pass the old
display ID on as the new one in later CGDisplay... / CGS...Display... calls. OAH_NO_QDMODE=1 and
OAH_NO_DISPIDFIX=1 turn the two parts off. Checked: the game's surfaces, viewport and textures are
1024x768, and the campaign's first level plays (confirmed by hand).

## Feature parity with Rosettus for Mojave (2026-11-09)

The features the release notes listed as "not tested on Catalina yet" were tested and fixed.

**PowerPC screen savers.** Catalina runs third-party savers in `legacyScreenSaver`, a sandboxed
process. Several problems, in order:
* The shim spawned the host from inside the sandbox, so the host was sandboxed too and could not run
  the translator. The shim now posts a distributed notification (`com.rosettus.saver.start` /
  `.stop`), and a launch agent, `savers/saverhelper.py` (`com.rosettus.ppc.saverhelper`), starts the
  host outside the sandbox. It only ever starts Rosettus's own runner and saver host, and only for an
  `Original.saver` inside an installed "(PowerPC).saver" shim. A 2 s watchdog stops hosts when no
  `ScreenSaverEngine` runs.
* The shim ran `ROOT/rosetta`, which fresh installs do not have; it now runs `ROOT/rosettus`.
* `legacyScreenSaver` reports `isPreview` YES for the full-screen view, so preview means "small"
  (under 600x400). `startAnimation` is not always called, so the first `drawRect` starts the host.
* Hosts started by the agent are not started through Launch Services, so `CGGetActiveDisplayList`
  returned no displays and the window went to Y=-1120. `oah_cg_displays()` (oah_cgs.h) falls back to
  `CGGetOnlineDisplayList` (`OAH_NO_ONLINEDISPLAYS=1` disables).
* The saver was black: Catalina shows the screen saver in a space of its own, above every app's
  windows, whatever their level (even the shielding level). A native test window became visible only
  in a SkyLight space at absolute level 300 or above. liboah now puts the process's windows into such
  a space when `OAH_WINDOW_SPACE_LEVEL` is set (new hostcall `r2_space_join` -> `r2h_space_join`:
  `SLSSpaceCreate`, `SLSSpaceSetAbsoluteLevel`, `SLSShowSpaces`,
  `SLSSpaceAddWindowsAndRemoveFromSpaces`). The agent sets 300 for the host.
* It ran slowly: the agent was a `ProcessType` Background job, and the host inherited darwinbg
  (priority 4, coalesced timers). The agent is now Interactive.
* A preview started from System Preferences could not be dismissed: the host made itself the front
  process (`SetFrontProcess` in `oah_make_foreground`), so the engine never got the mouse and key
  events that end a preview. `OAH_NO_SETFRONT=1` (set by the agent) skips it.
* The agent bootstrap could race its own bootout during an upgrade (fontd was left unloaded once):
  install.sh waits for the bootout and retries the bootstrap (both repositories).
Confirmed on the test Mac by the user: the saver shows full screen, runs smoothly, and both the idle
saver and the System Preferences preview end on a mouse move.

**Flash (QuickTime component and the built-in renderer: Kid Pix stickers).** The built-in renderer
passed a host-handle colorspace to the i386 `CGBitmapContextCreate`: the `L()` lookups in oah_swf.h
found the libr2native bridge stubs of the same names. `swf_cgsym()` reads CoreGraphics's own symbol
table, and a dladdr check refuses any function that is not CoreGraphics's. A Kid Pix sticker .swf
draws through it (an acorn, in the qtflash test program; Kid Pix itself: see 2026-11-10 below). The QuickTime 6 component path is the same as on Mojave; `install_qtflash.sh`
was not run end to end (no QuickTime 6 installer on the test Mac), but Rez/DeRez are present.

**Adobe Version Cue CS2.** Three Catalina-only problems:
* `install_versioncue.py` used `install_name_tool`, which on Catalina cannot read PowerPC Mach-O.
  `change_dylib()` now rewrites the load commands in Python.
* Every PowerPC program run as root crashed in CarbonCore at its first file system call
  (`_SCConstructMappedUniverse` storing to 0x10192034). For a root client, Snow Leopard's
  coreservicesd (translated, own rhost) shares the session universe through `vm_map` on the client's
  task port (the liboah task broker); the kernel put it above 4 GB (0x110192000) and the client got
  the low 32 bits. rhost now answers a fault on a free page of the 32-bit space whose address + 4 GB
  lies in memory mapped in from outside by mapping it there too (`foreign_alias_fix` in trap.c:
  shared, not copied; never host malloc, stack or dyld regions; `R2_NO_ALIASFIX=1` disables,
  `R2_VMLOG=1` logs). `rjava -version` and PowerPC tools run as root work.
* Then the server, started by its controller, still said "Unable to start database": MySQL's
  launches died at once. The children went through `ROOT/native/bash` (scripts of PowerPC programs,
  a copy of /bin/bash outside SIP, so dyld honours `DYLD_*`), and they carried the guest's
  `DYLD_LIBRARY_PATH` (the PowerPC root, Snow Leopard's libSystem first): bash died in dyld,
  "Symbol not found: _environ" (crash reports in /Library/Logs/DiagnosticReports). The translator
  passes the launcher's `XYLD_*` settings on as `DYLD_*` at every exec. rhost's `conv_env` now treats
  a guest `DYLD_X` equal to rhost's `XYLD_X` as the translator's own and drops it for native
  programs, and liboah's shell fix drops all `DYLD_*` except its own insert. It worked when started
  by hand because /bin/bash (SIP) had pruned the variables on the way.
Result: the LaunchDaemon starts the server, MySQL runs as nobody, and the log says "Version Cue CS2
up and running". A stale `JRs-MBP-3.pid` from a manual MySQL test was moved aside first.
Debug aids: `R2_SPAWNTRACE=2` logs each exec's environment and arguments; `R2_TRACE_CHILD=1` traces a
forked child.

**Preference panes.** The package now installs Rosettus Preference Pane Viewer in /Applications
(as install.sh does), with RosettusRoot set and the System Preferences icon copied in (Catalina keeps
System Preferences in /System/Applications; the first package looked in /Applications only).
The Viewer opened `/Library/PreferencePanes/Version Cue CS2.prefPane` on the test Mac: Adobe's own
pane, with "Version Cue CS2: On" (the server from the LaunchDaemon above).

**Sharp text on Retina displays.** With File > Sharp Text on Retina Displays in Rosettus Launcher
(`com.rosettus Retina`), TextWrangler's window text and menu titles drew at 2x (screenshot crops
checked at full resolution). No change was needed.

**Drag and drop.**
* Files dragged onto a PowerPC window: liboah's handler used the i386 (Mojave) CoreDrag, which on
  Catalina was not even found ("[dnd] CoreDrag not found") and could not reach this Mac's drag server
  anyway. CoreDrag now runs on the host (hostcalls `r2_dnd_start`, `r2_dnd_window`, `r2_dnd_poll` in
  hostcall.c): a host thread registers the connection as a CoreDrag client, the handlers on each
  window accept drags carrying file URLs (copy) and queue the drop's paths, and liboah polls the queue
  on its 0.25 s timer and sends the files as 'odoc', as on Mojave. Tested: a file dragged from a
  Finder window onto TextWrangler opened as a document (`OAH_DNDLOG=1` logs).
* Dragging selected text inside a PowerPC app ended a moment after it started (the text was dropped
  where the pointer was then, or slid back). Two causes in 10.6 CoreDrag's in-app loop
  (`DragInApplication`):
  * `GetDragTarget` asks `CGSFindWindowAndOwner(cid, dragWindow, -1, 1, ...)` for the window below
    its drag image; answered as a plain hit test, it returned the drag image itself. That window is
    now skipped.
  * `SampleMouseAndKeyboard` ends the drag when `CGEventSourceButtonState(0, 0..2)` says no button
    is down. The guest CoreGraphics answers that from the window server's scoreboard page, which a
    translated client does not have: always "up". A gtrace hook (`oah_fix_evbtnstate`) answers it
    with this Mac's `CGEventSourceButtonState` (`OAH_NO_EVBTNFIX=1` disables).
  Tested: a selected word dragged to the end of the line in TextWrangler moves there. (The
  "DraggingOwner" connection property it also reads is optional: unset, the window's owner is the
  target.)
* `Button`, `StillDown`, `WaitMouseUp`: every app runs in guest-toolbox mode on Catalina, so these
  are HIToolbox's own code; the HIToolbox bridges for them exist only outside that mode and are never
  reached. A first attempt that answered those bridges was taken out again. Checked with the hook
  on and off (`OAH_NO_EVBTNFIX=1`): Kid Pix's pencil draws a full stroke and its GO button works
  either way, Equation Editor's template palettes stay open while the button is held, and the
  behaviour tests of TextWrangler, Kid Pix, AppleWorks, Word and Keynote pass with the hook.

## Flash in QuickTime, tested; Kid Pix sticker palettes (2026-11-10)

* The QuickTime 6.0.3 page the user pointed to (macintoshrepository.org) has Mac OS 9 and Windows
  installers only (the hybrid CD image holds an HFS volume with the Classic network installer and
  a Windows installer). The QuickTime 6.5.2 for Mac OS X installer from the same site
  (`QuickTime_Installer_6-5-2.dmg`, SHA-1 f887c5f16d48050da2ba580b42d09a4aec01a371, as the site
  lists it) was used instead. The site lets a guest download once per 10 minutes.
* `tools/install_qtflash.sh ~/rosettus-work/qt6/QuickTime_Installer_6-5-2.dmg`, run as the user,
  found `QuickTime652.pkg` in the image and installed
  `/Library/Rosettus/System/Library/QuickTime/RosettusFlash.component`. Apps log
  "[qt] Flash component registered".
* `qtflash` (a PowerPC test program: component lookup, OpenMovieFile, NewMovieFromFile, draw
  frames to BMP) on a Kid Pix sticker carved from its .klb (`swfcarve/kp00_v3.swf`): the 'flsh'
  media handler and the 'SWF '/'SWFL' importers are found and open, the movie has one Flash track
  (146x85, 50/600 s), and the frames draw the sticker ("COOL" with snow).
* Kid Pix Deluxe 4's sticker palettes (Animals, Characters, Celebrate, Action Scenes...) show empty
  boxes, and a chosen sticker draws nothing on the canvas, with the component and without it (the
  built-in renderer). Kid Pix opens the .klb, but no OpenMovieFile, NewMovieFromFile,
  NewMovieFromDataRef or NewMovieFromHandle is called and no temporary .swf is written
  (`OAH_GTRACE` on QuickTime and CarbonCore file calls, fs_usage). The stamp palettes ("Ad Lib",
  the butterflies) do show. (The "an acorn was checked" of the entry above was the
  qtflash test program with the built-in renderer, not Kid Pix itself.)
* Cause: the guest heap above 2 GB. Kid Pix draws each sticker with its own Flash renderer
  (Rembrandt::FlashMovie) into a 16-bit GWorld: QTNewGWorld, EraseRect, LockPixels, then its own
  code writes the pixels and DisposeGWorld. Mojave makes exactly the same calls (OAH_GTRACE on
  QTNewGWorld, NewGWorld, LockPixels, DisposeGWorld, CopyBits... side by side); the only
  difference is where the memory is: the pixels at 0xb2000000 here, 0x0b000000 on Mojave. The
  renderer goes wrong above 2 GB (signed pointer arithmetic, as REALbasic's string allocator did:
  Mactracker, 2026-11-09).
* Why the heap was high: lowmem.c placed large requests (over 64 KB) next fit from a hint that
  only moves up until 1.5 GB were in use, and malloc's regions (2 MB mapped, trimmed to 1 MB
  aligned) come in that way, so after the launch they sat at 0xb0000000 and above while 1.1 GB
  was free lower down (R2_LOWDUMP). Mojave's kernel (and a real Mac's) places first fit.
* Fix (rhost lowmem.c): first fit for every request from the start (R2_LOW_FFMB default 0, was
  1536; aligned large requests also start from the lowest free page). With R2_LOW_FFMB=0 set by
  hand first, then with the new default: the Animals thumbnails draw, and the armadillo dragged
  from the palette lands on the canvas, as on Mojave (screenshots compared). The word-at-a-time
  first-fit scan from the InDesign fix (2026-11-07) keeps large requests fast.
* Regression run with first fit (cat_full11, 47 apps): no app worse than cat_full10 once rerun;
  Photoshop CS2 12/12, Equation Editor 9/9, Entourage 9/9, PowerPoint 10/10 and SimCity 4 (7/9)
  better. Excel (3/4) and Microsoft Graph (2/3) failed in the run, the same with the previous rhost
  put back: Office's FontCacheTool rebuilt "Office Font Cache (11)" at every launch and came to the
  front. The cache was left incomplete (1.1 / 1.4 MB instead of 3.5 MB) whenever a test ended the
  app while the tool was still writing, so the next Office app built it again. After one complete
  build: Excel 10/10, Graph 3/3, Word 10/10 with the new rhost.

## Menu bar apps (status items), Mojave and Catalina (2026-11-11)

* Test apps: `tests/ppc/menubar` (main.m, build.sh: `gcc-4.0 -arch ppc`, 10.5 SDK, built on Snow
  Leopard). "PPC Menu Dock" (regular app, window, status item), "PPC Menu Bar Only" (LSUIElement),
  "PPC Menu Dynamic" (LSUIElement; title "Dyn N" every 2 s, menu rebuilt in menuNeedsUpdate:, Show
  in Dock / Hide from Dock with TransformProcessType 1 / 4). Every event goes to
  ~/Library/Logs/PPCMenuTest.log with the item window's frame and isVisible.
* Before: no item showed on either system. The status item window stayed at x = -width above the
  screen. Snow Leopard's AppKit registers it with CGSRegisterWindowWithSystemStatusBar(cid, wid,
  priority), calls CGSAdjustSystemStatusBarWindows(cid), reads the place back (CGSGetWindowBounds)
  and sets its frame; the 10.6 window server placed and ordered the window. All of these were stubs.
* Mojave's server has SLSSystemStatusBarRegisterWindow(cid, wid, priority, spaceID, displayUUID,
  flags) (Mojave's AppKit: flags 1, the item's display identifier) and only computes a place: its
  AppKit moves and orders the window itself (a native lab test with raw SkyLight calls from Python:
  a registered window is never moved). Without the display UUID nothing at all is placed.
* Fix (liboah, oah_cgs.h): the status bar calls register with the server and lay our items out
  ourselves, right to left at the left of the other apps' items and menu extras (on-screen level-25
  windows in the menu bar), in registration order; a reshape keeps an item's right edge (a growing
  title grows to the left). CGSServerOperationState(16) answers 1, which makes AppKit 10.6 order its
  status windows in itself (its check: IsMenuBarVisible, that state, CGDisplayIsCaptured), so
  AppKit's own frame and isVisible are right; without that, clicks reached NSWindow's sendEvent:
  but never the item's button (its window was not visible to AppKit). Mouse events in the menu bar
  over one of our items go to its window, not to our menu bar window. CGSSetPreventsActivation is
  passed on. Hiding the app (Cmd-H, or the hide request that comes with a transform to UI element)
  leaves the items, and a hidden app's item menu still opens (levels 25 and up are not held back).
* TransformProcessType from the guest answered -600 (the guest's Process Manager); for this process
  it now goes to the host's (oah_gtrace.h fix). Dock icon appears and goes again (lsappinfo
  Foreground / UIElement).
* Dark Mode (the Catalina test Mac): 10.6 draws the titles black, nearly invisible on the dark bar.
  The pixels sent for a status item window have their dark grey made light (only the copy that is
  blitted; white text on the blue highlight stays).
* Results, Mojave and Catalina alike: each app alone (launch, menu, Say Hello, Open Window, Quit:
  the item goes away), the dynamic one through Show in Dock, Hide from Dock, menu after the hide,
  Quit; all three at once side by side (Mojave 890-1089, Catalina 1238-1437), each menu opening.
* Gap closing (second round): each app lays out only its own items, so a quitting app's gap stayed.
  A watch thread (every 0.5 s) now takes the right edge of our rightmost item from the nearest other
  item to its right (default: screen width - 120) and lays our items out again when it changed; it
  also adopts a move the server made on its own (Mojave's server moved an item after a resize to a
  colour icon) and tells AppKit with a window-moved event, so clicks still land. On Unregister the
  next item takes over the right edge at once.
* Menus after Show in Dock: HIToolbox caches the process kind (GetProcessState: 1 foreground,
  2 background only, 3 UI element) on first use and never looks again, so after the transform
  SetMenuBarObscured(false) returned at once and the menu bar was never registered or drawn. The
  cache byte is found from that function's code and set to 1 after TransformProcessType(1) (put
  back for 4). The guest's 1440x22 menu bar window is adopted, the activation is handed over (the
  app that was front before, else the Finder, then this one) and HIToolbox's SetMenuBarObscured
  1/0 is called in the guest, which registers and draws the bar: the app's own menus show and open.
* Picture items: template glyph (black, white alternate image while the menu is open), colour icon
  and icon with title all show and resize correctly on both systems.
* Textured windows did not move (Pages 09 document, Keynote / Pages choosers; also on Mojave, and
  with the library from before the menu bar work): AppKit 10.6 sends CGSAddDragRegion(cid, wid,
  region) - the window minus its controls, window coordinates - and leaves the move to the window
  server; those calls were no-ops. liboah keeps a guest copy of each window's region (union of the
  adds, released on CGSClearDragRegion); a left mouse-down inside it (CGSPointInRegion, no Command
  key, normal-level window) starts a move that follows the drag events (CGSMoveWindowWithGroup,
  title kept below the menu bar) and ends with a window-moved event for AppKit. The events still
  go to the app. Standard title bars were never affected: AppKit moves those itself.
* iWork '09 charts (also on Mojave): inserting one gave "An unexpected error has occurred", 3D
  charts were blank. SFCharts renders 3D charts into a CGL pbuffer or an offscreen window
  (backing type 0) with a multisampled context, and shows them in the canvas through the private
  surface texture (CGLSetParameter 999) from a context in another share group; the source context
  only calls glFlush. Native CGLCreatePBuffer always fails (10005) and CGSNewWindow refuses type 0.
  liboah emulates pbuffers with an FBO per context (colour texture, depth/stencil when the pixel
  format has them; CGLTexImagePBuffer reads back and uploads), retries type 0 windows as buffered,
  drops multisampling when no format has it (Intel GPU), refreshes surface textures after a guest
  glFlush / glFinish of a front-buffer context, resolves multisampled surfaces first, and copies
  through memory when the two contexts are in different share groups (contexts are mapped to their
  share root at CGLCreateContext; unknown contexts keep the GPU copy, as Build-a-lot 2 needs).
* Regression run with these changes (full12, 50 wrappers incl. the three test apps): every app as in
  full10/full11 except Excel 3/4 and Graph 2/3 (both pass alone: Excel 10/10, Graph 3/3, Word
  10/10). That is the Office font cache again, and now clearer: on Catalina Excel builds a 1.1 MB
  "Office Font Cache (11)" and Graph a 3.5 MB one, each rebuilds the other's at launch, and the
  FontCacheTool window takes the front (pre-existing; the same with the previous rhost and liboah;
  on Mojave both use one cache). The menu bar test apps score 5/9-7/9 there: the generic test
  expects a document window to type in. To look at: why Graph sees more fonts than Excel here.

## System-call gate; the spell server could not start (2026-11-13)

* Gate: loader.c's patch_sysenter writes `jmp rel32` to gate_tramp32() (a page below 4 GB:
  `popl %edx; movl %esp,%ecx; ljmp *far` -> 64-bit `movabs r11, r2_gate64; jmp r11`). r2_gate64
  saves eax..gs into a block under the 32-bit esp, finds the r2thr_t by thread_selfid, switches to
  the host stack when depth was 0, sets the host gs base (0x3000003), xsave64s, calls r2_gate_c
  (fake ucontext + mcontext64_full so bsd_syscall/ts_set/signal delivery work unchanged), then
  xrstor64, sets the tsd32 gs base and returns with iretq built on the 32-bit stack. on_signal_c
  keeps the 32-bit gs if a signal lands inside the exit window (_r2_gate_exit_lo..hi); the 32-bit
  sigmask lives in t_gate_mask between calls. gatetest (FP, signals, mask, fork) passes; fpsave /
  fphost fail the same with and without the gate (older issue).
* Red zone: the first gate wrote its register block and iretq frame in the 128 bytes just below
  the 32-bit %esp. The signal path never touched them (the kernel builds a signal frame below
  rsp - 128), and Rosetta's translator keeps data there across a system call: G4 Quasar crashed at
  start-up about one time in ten (its 2.7 MB __DATA zero-fill was never allocated, other memory
  landed in it, SIGBUS at 0x298000). tests/catalina/redzone.c: 18 words changed with the first
  gate, none now or with R2_GATE=0. The registers to return with also go to the host stack now
  (a 32-bit signal frame built on the way out may cover the block). sigsys.c / sigsys2.c: system
  calls under a fast timer (integer registers, xmm7 across a handler that changes it).
* The Keynote failure seen while testing the gate (type+copy empty, Cmd-H ignored, two documents)
  was not the gate: it failed the same with R2_GATE=0, the previous rhost and liboah. Keynote.log
  showed `[spell] starting Snow Leopard's spell server` over and over and
  /tmp/oah_child_AppleSpell.service.log "AppleSpell has no PowerPC code": the launcher's no-PPC check
  (added for 1.0) read /System/Library/Services/AppleSpell.service from Catalina (x86_64 only), not
  $ROOT's Snow Leopard copy (x86_64 i386 ppc7400) that translate opens. The translated server from
  an earlier session had kept things working until it was stopped. Fixed in scripts/rosettus
  (guest /System and /usr paths are checked under $ROOT); Keynote 11/11 again.

## Mach traps and the gate; Finder-front stops; fpsave (2026-11-13)

* Mach traps already use the gate: libsystem_kernel's kernel_trap stubs are `movl $-N,%eax; call
  __sysenter_trap`, and bsd_syscall hands negative numbers to mach_trap. The `int 0x81` left in
  R2_PROF (mach 32 = mach_msg_overwrite, 37-39 semaphore waits) comes from translate_mojave's own
  code: 1-3 thousand in 10 s of a Pages launch, against 270 000 mach_msg and 115 000 proc_info
  through the gate. Not worth patching the translator for. vmbench.c: the gate costs ~1.1 us over
  a native call (thread_selfid + two gs-base syscalls + xsave/xrstor).
* Finder-front stops in the behaviour tests, two causes. (1) Office 2004's FontCacheTool (a bare
  Mach-O tool in Support Files, run by Word / Excel / PowerPoint at launch) was made a foreground
  app by oah_make_foreground when it connected to the window server and took the front from the
  Office app (lsappinfo showed it as "rhost" in front). Tools now check in as background-only
  (oah_background_kind; OAH_TOOL_FOREGROUND=1). (2) The test's copy check activates the Finder;
  on this Mac the Finder (no window open, building a Recents window) came forward 18 s later, in
  the middle of a later step (Save As in Illustrator / InDesign, Cmd-H in TextWrangler, iWeb).
  behave.py now waits until the Finder is in front, and on a stop logs what is in front and saves
  abort.png. The 10 apps that stopped this way: all pass.
* fpsave / fphost: CFHostStartInfoResolution's lost f30/f31 (stock Rosetta too) are put back by a
  gtrace fix + fixret pair (oah_gtrace.h, oah_fix_fpkeep: f14-f31 at R + 0x1e8 + 14*8, a per-thread
  stack of 8). fpsave, fphost and cfnet pass on Catalina and Mojave.

## iWork selection handles: glInterleavedArrays (2026-11-13)

* Reported: no knobs (the white squares around a selected object) in Keynote / Pages / Numbers.
  Mojave 1.2 had the same (the GL bridge is shared). SFDGLDecoration queues each knob;
  -[SFDCanvas drawQueuedDecorations] draws them all with glBindTexture (shared tile texture) +
  glInterleavedArrays(T2F_V3F) + glDrawArrays. oah_gl.h only emulated the separate
  gl*Pointer calls: glInterleavedArrays went to the native GL with the big-endian floats, and
  oah_gl_setup_arrays turned the arrays it had enabled off again at the draw. Now OP_glInterleavedArrays
  splits the format (all 14 of the spec's table) into the vertex / colour / normal / texcoord
  records, converted at draw time; edge flag, index, fog and secondary colour arrays are disabled
  as the spec says. Knobs show on Catalina and Mojave (title box, after a move).
