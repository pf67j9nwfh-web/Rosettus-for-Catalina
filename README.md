# Rosettus for Catalina

Rosettus for Catalina runs PowerPC Mac applications on **macOS 10.15 Catalina**, using the Rosetta
1.0 from Mac OS X Snow Leopard.

Catalina removed 32-bit code altogether: the kernel no longer runs it, and the 32-bit Intel system
libraries are also gone. Rosetta's translator is itself a 32-bit Intel program, so Rosettus for
Catalina brings its own 64-bit host process, **rhost**, that loads and runs the translator, Mojave's 32-bit
Intel libraries and the shim library inside a 64-bit process, and stands in for everything the
kernel used to do for them (32-bit system calls, Mach traps and messages, threads, signals,
exceptions, memory layout, the commpage). On top of that it is the same project as
**Rosettus for Mojave**: the shim library (`liboah`) connects the PowerPC system frameworks to
Catalina's window server, events, audio, OpenGL, pasteboard, Launch Services and Apple Events.

Apps confirmed working on Catalina include AppleWorks 6, Microsoft Office 2004 (Word, Excel,
PowerPoint, Entourage), iWork '09 (Keynote, Pages, Numbers, through their PowerPC code), Adobe CS2
(Photoshop, Illustrator, InDesign, GoLive, Acrobat 7), SimCity 4, Bejeweled 2, Build-a-lot 2,
Internet Explorer 5.2, Kid Pix Deluxe 4, Blender 2.63, TextWrangler, Hex Fiend, Transmission,
Mactracker and PowerPC Java 1.3.1, 1.4.2 and 5. See [docs/COMPATIBILITY.md](docs/COMPATIBILITY.md).

> **No Apple software is included in this repository or in the installer package.** Rosettus for
> Catalina needs files from two of your own Macs: Rosetta and the PowerPC system from **Snow
> Leopard** (or a Snow Leopard install disc), and the 32-bit Intel libraries from **Mojave**.
> Scripts copy exactly the files needed. The repository holds only this project's own code, lists
> of the files to copy (names and checksums), and small binary patches containing only the bytes
> this project changes.

**Rosettus for Catalina 1.0** is the current release: see [RELEASE_NOTES.md](RELEASE_NOTES.md).
For macOS 10.14 Mojave, use **Rosettus for Mojave**, which runs the translator directly.

## Requirements

| | |
|---|---|
| Catalina Mac | macOS 10.15 on an Intel Mac (tested: 10.15.7), about 6 GB free |
| Snow Leopard Mac | Mac OS X 10.6.8 with Rosetta (any Intel Mac that runs 10.6, or a virtual machine on Apple hardware), **or** a Mac OS X 10.6 install DVD or disc image (see [Without a Snow Leopard Mac](#without-a-snow-leopard-mac-install-dvd)) |
| Mojave | the 32-bit libraries of macOS 10.14 (tested: 10.14.6), copied once: from a Mac running Mojave (another Mac, a second partition or disk, or a virtual machine), **or** from Apple's *Install macOS Mojave* app |

## Installing

1. **On Snow Leopard**, copy the *Snow Leopard Export Kit* (`import/sl` in this repository, or
   `/Applications/Rosettus/Snow Leopard Export Kit` after installing the package) and run:

   ```
   ./export_from_sl.sh
   ```

   If Rosetta is not installed, the script opens **Install Rosetta.app**, which makes Snow Leopard
   offer its Rosetta download; accept it and run the script again. The result is
   `~/Desktop/Rosettus-SL-export.tar.gz` (5-15 minutes).

2. **On Mojave**, copy the *Mojave Export Kit* (`import/mojave`, or
   `/Applications/Rosettus/Mojave Export Kit`) and double-click **export_from_mojave.command**
   (or run `./export_from_mojave.py`). It copies the 32-bit (i386) dyld shared cache, the i386 code
   of about 840 libraries and plug-ins and the 32-bit commpage into
   `~/Desktop/Rosettus-Mojave-export.tar.gz` (about 510 MB, a few minutes). Nothing is changed on
   the Mojave Mac, and no developer tools are needed.

   **No Mojave Mac?** The same export can be made from Apple's **Install macOS Mojave** app (about
   6 GB, downloadable from Apple), on any Mac including the Catalina one:
   `./from_installer.sh "Install macOS Mojave.app"` in the Mojave Export Kit (a .zip of the app
   works too). It reads the 32-bit libraries out of the installer's system package and builds the
   32-bit shared cache with the installer's own tools, as installing Mojave would (10-20 minutes,
   about 11 GB of free space while it runs, no administrator password). See
   `import/mojave/README.txt`.

3. **On Catalina**, put both archives in your Downloads folder and either open
   **Rosettus for Catalina 1.0.pkg**, or run from the repository folder:

   ```
   ./install.sh ~/Downloads/Rosettus-SL-export.tar.gz ~/Downloads/Rosettus-Mojave-export.tar.gz
   ```

   This unpacks the files into `~/Library/Rosettus` (the package: `/Library/Rosettus`, for every
   account), checks every Snow Leopard file against its checksum and applies the patches. It also
   installs **Rosettus Launcher**. The script needs no administrator password; the package asks for
   one as any installer does.

4. Open **Rosettus Launcher**, click **Search This Mac**, then **Patch All…**. From then on your
   PowerPC apps open like any other app.

### Without a Snow Leopard Mac (install DVD)

The Snow Leopard export can also be made from a **Mac OS X 10.6 Snow Leopard install DVD** (the disc,
or an `.iso`/`.dmg` image) and Apple's free Snow Leopard updates, which the tool downloads from
Apple's servers: **Applications › Rosettus › Snow Leopard Install DVD Import ›
Import from Install DVD.command**, or `import/dvd/import_from_dvd.sh`. It runs on a Mac with
Mojave (it starts the DVD's 32-bit translator once to decode it), so do it on a Mojave Mac (the
one of step 2) and copy both archives to Catalina; it cannot run on Catalina itself. See
`import/dvd/README.txt`.

### The installer package

`tools/mkpkg.sh 1.0` builds **`dist/Rosettus for Catalina 1.0.pkg`**, a standard installer
package that installs for all users of the Mac:

* **Rosettus Launcher** goes to `/Applications`, a setup kit (scripts, patches, checksums; no
  Apple files) to `/Library/Rosettus/Setup`, and the export kits to `/Applications/Rosettus`.
* If `Rosettus-SL-export.tar.gz` and `Rosettus-Mojave-export.tar.gz` are next to the package, or
  in the Downloads or Desktop folder of the user running it, the package imports them into
  **`/Library/Rosettus`**. Otherwise Rosettus Launcher asks for them the first time it opens
  (choose both files; an administrator password is asked for in the system dialog).
* The helper agents are installed in `/Library/LaunchAgents`, so they run for whoever logs in.

The same from Terminal: `sudo ./install.sh --system Rosettus-SL-export.tar.gz Rosettus-Mojave-export.tar.gz`;
remove it with `sudo ./uninstall.sh --system`. `./install.sh --update` re-installs this
repository's files into an existing install (add a Mojave export to replace the Mojave files).

The package is not signed with a Developer ID, so Gatekeeper may refuse to open it with a double
click: Control-click it, choose **Open**, and confirm.

## Using it

Rosettus Launcher, patched apps, launchers, documents, the clipboard, helper programs and scripts
work as in Rosettus for Mojave; the Rosettus for Mojave README and the documents in [docs/](docs/)
describe them in detail. In short:

* **Rosettus Launcher** lists the PowerPC programs in `/Applications`, `~/Applications`,
  `~/Downloads`. **Open** runs one, **Patch** changes an app so a normal double-click (Finder,
  Dock, Spotlight, documents) runs it through Rosettus (**Unpatch** restores it exactly),
  **Launcher** makes a separate launcher app in `~/Applications/Rosettus` and leaves the app
  untouched. **File › Patch New PowerPC Apps Automatically** patches new PowerPC apps as they
  appear.
* A PowerPC app has its own Dock icon and name, appears in Cmd-Tab and Force Quit, receives
  documents from Finder, answers Apple Events, and shows its menus in Catalina's menu bar.
  Copy and paste work in both directions.
* The first time a PowerPC app reads your Documents, Desktop or Downloads folder, Catalina asks
  whether *<App>* may access it, as for any app: choose **OK** (the answer is remembered for that
  launcher or patched app; *System Preferences › Security & Privacy › Privacy › Files and Folders*
  changes it later).
* Every run writes `~/Library/Logs/Rosettus/<App>.log`; crashes and hangs are reported in
  `~/Library/Logs/Rosettus/Reports/`.

From Terminal (`R` is `/Library/Rosettus` for the package's shared install, or `~/Library/Rosettus`
for a per-user install):

```
$R/rosettus /Applications/SomeApp.app                    # run an app
python $R/rosettus_patch.py /Applications/SomeApp.app     # patch
python $R/rosettus_patch.py --unpatch /Applications/SomeApp.app
```

### Command-line tools

PowerPC command-line programs (Unix tools, compilers, helpers) run through the same `rosettus`
command. Catalina cannot start them itself: typed directly, a PowerPC tool fails with
*Bad CPU type in executable*.

```
/Library/Rosettus/rosettus /path/to/ppc-tool arg1 "arg 2"        # shared install
~/Library/Rosettus/rosettus ./ppc-tool --help                      # per-user install, relative path
some-command | /Library/Rosettus/rosettus /path/to/ppc-filter > out.txt
```

The tool gets its arguments, environment and current folder, reads standard input (a pipe, a file,
or the terminal for interactive tools), writes standard output and error, and `rosettus` exits with
the tool's exit status, so it works in scripts and pipelines; Ctrl-C stops it. Rosettus's own
messages go to `~/Library/Logs/Rosettus/tools.log`, not into the tool's output
(`OAH_TOOL_STDERR=1` keeps them on stderr). To type just a tool's name, put `rosettus` on your
`PATH` (`sudo ln -s /Library/Rosettus/rosettus /usr/local/bin/rosettus`) or wrap the tool in a
two-line script:

```
#!/bin/sh
exec /Library/Rosettus/rosettus /path/to/ppc-tool "$@"
```

Tools that a PowerPC app starts itself (helper commands, shell scripts and the PowerPC programs they
run) need none of this. `tests/catalina/clitest.c` checks all of the above.

### PowerPC Java

Snow Leopard has no PowerPC Java. **Applications › Rosettus › PowerPC Java Import ›
Import PowerPC Java.command** builds Apple's PowerPC Java 1.3.1, 1.4.2 and 5.0 from Apple's own
Tiger Java updates (downloaded from Apple's servers, about 130 MB) and installs them; see
`import/java/README.txt`. Run Java programs with `/Library/Rosettus/rjava`: `rjava -version`,
`rjava -jar App.jar`, `rjava javac Hello.java`, or `rjava -j 1.4 ...` for an older version. On
Catalina, Java 1.3.1, 1.4.2 and 5 compile and run programs and show Swing windows.

### More Rosettus for Mojave features

These work on Catalina as on Mojave (see the Rosettus for Mojave sections of
[docs/SAVERS_WIDGETS.md](docs/SAVERS_WIDGETS.md), [docs/PREFERENCE_PANES.md](docs/PREFERENCE_PANES.md)
and [docs/VERSION_CUE.md](docs/VERSION_CUE.md)):

* **PowerPC screen savers** run full screen, in the idle screen saver and in the System Preferences
  preview. Catalina runs third-party savers in a sandboxed process, so a small launch agent
  (`com.rosettus.ppc.saverhelper`) starts the PowerPC saver for it.
* **Preference panes**: Rosettus Preference Pane Viewer (in Applications) opens PowerPC panes, such
  as Adobe Version Cue CS2's.
* **Adobe Version Cue CS2**: its server runs as a background service (MySQL included) and its pane
  shows it **On**.
* **Flash in QuickTime**: `tools/install_qtflash.sh` adds the Flash of your own QuickTime 6 for
  Mac OS X (for example the QuickTime 6.5.2 installer) to the PowerPC QuickTime, which then opens
  and plays .swf movies.
* **Sharp window text on Retina displays**: File > Sharp Text on Retina Displays (Experimental) in
  Rosettus Launcher.
* **Drag and drop**: files dragged from Finder (or any native app) onto a PowerPC app's window open
  in that app, and dragging inside a PowerPC app works (moving selected text in TextWrangler). As on
  Mojave, drags from a PowerPC app to a native app are not carried over yet.
* **Menu bar apps**: a PowerPC app's status items (NSStatusItem) show in the menu bar and open
  their menus, for apps that are also in the Dock, for menu-bar-only apps (LSUIElement), and for
  apps that change their item's title or put themselves in and out of the Dock while running. In
  Dark Mode the items' titles are drawn light. When one of these apps quits, the other PowerPC items move over to close the gap. Items drawn as pictures (a template glyph, a colour icon, or an icon with a title) work as well as text ones, and an app that puts itself in the Dock gets its own menus in the menu bar.

Dashboard widgets with PowerPC plug-ins do not apply: Catalina has no Dashboard.

## How it works (short version)

* **rhost** (`src/rhost`) is a 64-bit program that reserves the low 4 GB of its address space, loads
  Mojave's 32-bit dyld, shared cache and libraries into it, and runs Snow Leopard's `translate`
  (Rosetta, with the same 38-byte patch as on Mojave) in 32-bit mode. Each system call, Mach trap
  and Mach message of the 32-bit code is handled by rhost: converted to the 64-bit form and passed
  to the kernel, or carried out by rhost itself (threads, signals, exception ports, memory, the
  workqueue for libdispatch, the 32-bit commpage).
* The translator translates PowerPC code to x86 and runs the **PowerPC** system frameworks from 10.6
  (Carbon, Cocoa, QuickDraw and others) in the same process, exactly as on Mojave.
* About 2,000 framework functions are **bridged** by the shim library, `liboah_r2.dylib`. Where the
  Mojave version calls Mojave's 32-bit frameworks, which cannot talk to Catalina's window server or
  Launch Services, the Catalina build calls them through **libr2native**: small i386 stubs that hand
  the call to rhost, which makes it in 64-bit code (`src/rhost/bridge`).
* Services that Catalina no longer provides in a form 10.6 understands (pasteboard, font server,
  coreservicesd) run as translated 10.6 daemons, under launchd agents named `com.rosettus.ppc.*`.

More detail: [docs/NOTES-catalina.md](docs/NOTES-catalina.md) (the development notes) and
[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) (the shared parts).

## Troubleshooting

* Every run writes `~/Library/Logs/Rosettus/<App>.log`; crash and hang reports, with PowerPC and
  native backtraces, go to `~/Library/Logs/Rosettus/Reports/`.
* `OAH_*` environment variables switch individual fixes off or turn on tracing
  ([docs/ENVIRONMENT.md](docs/ENVIRONMENT.md)); rhost has its own (`R2_TRACE=1`, `R2_SIGLOG=1`,
  `R2_EXCLOG=1`, `R2_MSGLOG=1` ...), explained in [docs/NOTES-catalina.md](docs/NOTES-catalina.md).
  Per-app settings go in a launcher's `Contents/Resources/rosetta.env`, one `export NAME=value`
  per line.
* [docs/TROUBLESHOOTING.md](docs/TROUBLESHOOTING.md) covers common problems.

## Building from source

The prebuilt binaries in `bin/` are built from `src/`:

| Component | Built on | Command |
|---|---|---|
| `rhost` (x86_64 host process) | Catalina + Command Line Tools | `src/rhost/build.sh` |
| `liboah_r2.dylib` (i386 shim) | Snow Leopard + Xcode 3.2.6 | `src/liboah/build.sh` |
| `libr2native.dylib` (i386 bridge stubs) | Snow Leopard + Xcode 3.2.6 | `src/rhost/bridge/build_i386.sh` |
| `Rosettus Launcher.app` | Catalina + Command Line Tools | `src/launcher/build.sh` |
| `rlaunch`, `libppcexec.dylib`, savers, prefpanes, Version Cue helpers | as for Rosettus for Mojave | `src/*/build.sh` |

`bin/SOURCES.sha1` records which sources each prebuilt file was built from. After rebuilding one and
copying it into `bin/`, run `tools/binstamp.sh --update NAME`; `tools/binstamp.sh --check` lists
files whose sources changed since, and `tools/mkpkg.sh` will not build a package while any are
listed. Tests: `tests/behave_all.sh` (`ROSETTUS=/Library/Rosettus/rosettus`,
`BEHAVE_WRAPDIR=/Applications/Rosettus`), `tests/difftest.sh`, and the Catalina-specific checks in
`tests/catalina/` (command-line tools, floating point, gettimeofday, CFNetwork).

## Legal

Rosettus is not affiliated with Apple. Rosetta, Mac OS X, macOS and the frameworks and libraries it
uses are Apple's software. They are copied only from installations that you own (Snow Leopard and
Mojave), onto your own Mac, for interoperability with your own PowerPC software. `cp_data.h` (in
`src/liboah` and `src/rhost`) is the PowerPC commpage from xnu (Apple Public Source License 2.0).
Everything else in this repository is under the MIT License ([LICENSE](LICENSE)).
