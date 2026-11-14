# Rosettus for Catalina release notes

Rosettus for Catalina is the macOS 10.15 Catalina port of Rosettus (Rosettus for Mojave runs on
macOS 10.14). It shares the shim library, the Snow Leopard export and the patches with Rosettus for
Mojave; see that project's release notes for the history of those parts.

## Rosettus for Catalina 1.0

**November 9, 2026 - the first release.**

Rosettus for Catalina runs PowerPC Mac applications on macOS 10.15 Catalina, which cannot run
32-bit code at all, with the Rosetta translator from your own Mac OS X 10.6 Snow Leopard
installation. No Apple software is included.

### What's included

* **rhost**, a 64-bit host process that runs Rosetta's 32-bit translator on Catalina: it loads
  Mojave's 32-bit Intel libraries and shared cache into the low 4 GB of a 64-bit process, runs the
  translator in 32-bit mode and carries out its system calls, Mach traps and messages, threads,
  signals and exceptions, the libdispatch workqueue and the 32-bit commpage.
* **The shim library** (`liboah_r2.dylib`), the Catalina build of Rosettus for Mojave's shim: the
  PowerPC system frameworks reach Catalina's window server, events, menu bar, audio, OpenGL,
  clipboard, Launch Services and Apple Events through native 64-bit code in rhost
  (`libr2native.dylib`).
* **Rosettus Launcher** (in `/Applications`): imports the two exports, finds PowerPC apps, patches
  them or makes launchers, and opens them.
* **Installer package** `Rosettus for Catalina 1.0.pkg`: installs for every account. A setup kit
  goes to `/Library/Rosettus/Setup`, and four folders to `/Applications/Rosettus`:
  * *Snow Leopard Export Kit*: copy it to a Snow Leopard Mac to make the Snow Leopard export.
  * *Mojave Export Kit* (new): copy it to a Mojave Mac and double-click
    `export_from_mojave.command` to make the Mojave export (Mojave's 32-bit libraries, which
    Catalina does not have).
  * *Snow Leopard Install DVD Import*: makes the Snow Leopard export from an install DVD and
    Apple's 10.6.8 updates, on a Mac with Mojave.
  * *PowerPC Java Import*: builds PowerPC Java 1.3.1, 1.4.2 and 5.0 from Apple's own Java updates.
* Command-line tools: PowerPC command-line programs run with `/Library/Rosettus/rosettus TOOL ARGS`
  (arguments, standard input and output, exit status and Ctrl-C as for a native tool).

### Tested

On macOS 10.15.7 with exports from Snow Leopard 10.6.8 and Mojave 10.14.6: the full test run
(`tests/behave_all.sh`: launch, typing, copy and paste, hide and show, quit and reports for every
app) is at the level of Rosettus for Mojave for all apps; see
[docs/COMPATIBILITY.md](docs/COMPATIBILITY.md) for the results per app. PowerPC Java 1.3.1, 1.4.2 and
5 compile and run programs and show Swing windows.

REALbasic apps (built with REALbasic 5.5 to 2010, such as Mactracker): Rosettus corrects a bug in
the REALbasic runtime that only shows when an app's memory lies above 2 GB, as it does under
Rosettus on Catalina. Without the correction Mactracker listed a single model; it now lists all
532 with their icons.

The Mojave export made from the "Install macOS Mojave" app (`from_installer.sh`, tested with a
10.14.6 installer on the Catalina Mac: 7 minutes) runs the same apps as one made on a Mojave Mac,
PowerPC Java included.

X11 programs (Wireshark's GTK interface) run in XQuartz 2.8, which Rosettus starts when one opens.

Apps that wait for an Apple Event reply from a helper app (SimCity 4 asks its sound helper,
APlayer, before the region view starts): the reply now reaches the waiting app. Before the fix
SimCity's region view stood still and did not respond to clicks.

Sound played through an AudioUnit (Cocoa's NSSound, for example the music of G4 Quasar) now
reaches the speakers; before, only Sound Manager sound did.

Games that switch the screen resolution (Build-a-lot 2) now see the new size: Build-a-lot 2 no
longer crashes when a campaign level starts, and its menus are no longer slow.

The Rosettus for Mojave features work on Catalina too: PowerPC screen savers (full screen, smooth,
and both the screen saver and the System Preferences preview end on a mouse move), PowerPC
preference panes in Rosettus Preference Pane Viewer, Adobe Version Cue CS2 (its server and MySQL
run as a background service, and its pane shows On), Flash in QuickTime, sharp window text on
Retina displays, files dragged from Finder onto a PowerPC app's window, and drags inside a PowerPC
app (moving selected text in TextWrangler). PowerPC programs run
as root (Version Cue's server, installer helpers) no longer crash at their first file access.
The guest heap now stays below 2 GB, as on Mojave: Kid Pix Deluxe 4's animated sticker palettes
draw their thumbnails and stickers.

PowerPC menu bar apps work: status items show in the menu bar (side by side, at the left of the
system's), open their menus and run their commands, for apps that are also in the Dock,
menu-bar-only apps, and apps that change their item or move themselves into and out of the Dock
(TransformProcessType) while running. Hiding such an app leaves its item in the menu bar. In Dark
Mode the items' titles are drawn light. When one of these apps quits, the other PowerPC items move over to close the gap. Items drawn as pictures (a template glyph, a colour icon, or an icon with a title) work as well as text ones, and an app that puts itself in the Dock gets its own menus in the menu bar.

**Textured windows can be moved again.** The windows of Pages, Keynote, Numbers and other apps with textured or unified-toolbar windows did not move when dragged by the title bar or the toolbar background: Snow Leopard's window server moved those itself (AppKit only told it which parts of the window may be dragged), and that part was missing. Rosettus now moves them.

**Charts in Pages, Keynote and Numbers '09.** Inserting a chart showed "An unexpected error has occurred" and 3D charts stayed blank. iWork draws charts with OpenGL into offscreen buffers (pbuffers) that current macOS no longer creates; Rosettus now provides them itself, and copies the finished 3D chart into the document's canvas. 2D and 3D charts are drawn as on Snow Leopard.

**Less work in the background.** An app behind other apps checks for input less often (at most 0.2 s before the click that brings it forward is handled), and an app that has made a sound no longer keeps the sound output running: it stops after 10 s of silence and starts again with the next sound.

**Default buttons no longer pulse.** Snow Leopard animated the default button of every dialog at 30 frames a second, and redrawing it through the translated toolbox cost 10-15 % of a core for as long as a dialog was open (Word 2004's Project Gallery, an Open panel). Default buttons are now drawn blue without the pulse, as on current macOS. `OAH_BUTTON_PULSE=1` brings the pulse back.

**Faster launches on Catalina.** A memory lookup the translator makes thousands of times a second while an app starts scanned the 32-bit address map one page at a time. It now skips whole runs: Keynote '09 opens in about 12 s instead of 23 s, Pages '09 in 10-19 s instead of 23-26 s.

**System calls without a signal.** Every system call of the 32-bit world used to stop on a trap that macOS turned into a signal, and rhost carried the call out from its signal handler: three trips through the kernel per call. rhost now enters 64-bit mode directly through a call gate it places in the 32-bit system library (`sysenter`), and returns with one `iret`. A system call costs a fifth of the time it did (1.8 million calls: 7.6 s -> 1.3 s); Pages '09 opens about 1 s sooner. `R2_GATE=0` goes back to the signal path.

**Spell checking could stop Keynote and Pages.** Snow Leopard's spelling service, which Rosettus starts translated when an app first checks spelling, was turned away by the launcher's "no PowerPC code" check, which looked at Catalina's own AppleSpell at the same path instead of the Snow Leopard copy. Once a running copy had quit, every spelling check waited for a service that never came: typing in a Keynote slide showed nothing, and Cmd-H did not hide the app. The launcher now checks the Snow Leopard copy.

**Helper tools stay in the background.** A PowerPC program that is not an application (no .app bundle, no CFM application, no 'APPL' file type) no longer becomes the front app when it draws or connects to the window server, as on Snow Leopard. Office 2004's FontCacheTool ("Optimizing font menu performance", started by Word, Excel and PowerPoint) took the front from the Office app while it started, so its first keystrokes went nowhere. `OAH_TOOL_FOREGROUND=1` restores the old behaviour.

**Floating-point values survive a host-name lookup.** After `CFHostStartInfoResolution` (a synchronous host lookup, also used inside CFNetwork's socket and HTTP streams) the caller's floating-point registers f30 and f31 came back changed (stock Rosetta on Snow Leopard does the same). Rosettus now puts f14-f31 back, as the PowerPC calling convention requires: a first network fetch could time out at once when its start time was kept in one of them. `OAH_NO_FPKEEP=1` turns this off.

**Selection handles in Pages, Keynote and Numbers '09.** A selected text box, shape, picture or chart showed no handles: the small white squares at its corners and edges that show its outline and are dragged to resize it. iWork draws them with OpenGL from one interleaved vertex array (`glInterleavedArrays`), which Rosettus passed to the native OpenGL unconverted (PowerPC byte order) and then switched off again before drawing. It is now converted like the other vertex arrays, and the handles show as on Snow Leopard.

**Version Cue no longer sticks at "Stopping".** When the Version Cue CS2 server hung while shutting down (its database kept running), the Version Cue preference pane showed "Stopping Version Cue CS2..." with its controls disabled for good, and reinstalling Rosettus left the service switched off. The service now ends a shutdown that has not finished after 90 s (the server and its database), so Version Cue can be started again, and the installer always starts the service again, even when it could not replace it. `VC_STOP_TIMEOUT` sets the wait in seconds.

**Games keep running on a Mac that has been up for weeks.** Build-a-lot 2 stopped at the HipSoft splash screen on a Mac that had been running for more than 24.8 days without a restart. Like many games ported from Windows, it keeps time in milliseconds since startup through `AbsoluteToDuration(UpTime())`, and that call can count only up to 2^31 ms (24.8 days); after that it answers "forever" every time, so the game's clock stopped and its frame pacing waited for ever (Snow Leopard does the same). When the Mac has been up for 12 days or more, Rosettus now counts such readings from shortly before the app started, so the clock runs as after a restart. `OAH_NO_UPTIMEFIX=1` turns this off.

**File > Open Recent remembers documents.** In Pages, Keynote, Numbers and other Cocoa apps the Open Recent menu was empty after the app was quit and opened again. Snow Leopard's AppKit keeps that list in the app's own recent-documents list, which Rosettus did not provide; it now comes from macOS's list for that app, as for a native app. The shared lists (the Finder sidebar, Apple menu > Recent Items) are still not changed by PowerPC apps. `OAH_NO_APPRECENT=1` turns this off.

### Known issues

* As on Mojave, drags from a PowerPC app to a native app, and text or pictures dragged from native
  apps into a PowerPC app, are not carried over yet. Dashboard widgets do not apply (Catalina has no
  Dashboard).
* A PowerPC app takes a little longer to start than on Mojave (rhost sets up the 32-bit world first).
* The package is not signed with a Developer ID: open it with Control-click › Open.
* Make the Snow Leopard export with this release's Snow Leopard Export Kit. An export made by an
  earlier Rosettus lacks files added since (Snow Leopard's X11, QuickTime codec components, the
  spelling service); the installer says so, and with it typing in some Adobe CS2 apps lost the
  keyboard focus to the Finder in the tests.
