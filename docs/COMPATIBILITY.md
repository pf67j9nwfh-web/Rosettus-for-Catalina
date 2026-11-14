# Compatibility (Rosettus for Catalina)

Tested on a 2019 MacBook Pro with macOS 10.15.7 (19H15), the Rosetta from Mac OS X 10.6.8 and the
32-bit libraries of macOS 10.14.6 (18G103). Rosettus for Catalina runs the same PowerPC code, shim
and patches as Rosettus for Mojave, so the detailed notes per app in
[COMPATIBILITY-mojave.md](COMPATIBILITY-mojave.md) (features used, known limits of each app) apply
here as well, except where this page says otherwise.

## Behavior test

`tests/behave_all.sh` launches each app through its launcher, makes a new document, types, copies
and pastes (checked on Catalina's clipboard), hides and shows the app, quits it, and checks that no
crash or hang report was written. Full run of November 9, 2026 (steps passed / steps):

| App | Result | | App | Result |
|---|---|---|---|---|
| AppleWorks 6 | 9/9 | | Microsoft Word 2004 | 10/10 |
| Microsoft Excel 2004 | 10/10 | | Microsoft PowerPoint 2004 | 10/10 |
| Microsoft Entourage 2004 | 9/9 | | Office 2004 helpers (Clip Gallery, Graph, Query, Organization Chart, Notifications, Database Daemon, Project Gallery, Equation Editor) | all steps |
| Pages '09 | 11/11 | | Keynote '09 | 11/11 |
| Numbers '09 | 11/11 | | iWeb '09 | 9/9 |
| Photoshop CS2 | 12/12 | | Illustrator CS2 | 11/11 |
| InDesign CS2 | 11/11 | | GoLive CS2 | 12/12 |
| Acrobat 7 Professional | 3/4 (1) | | Adobe Help Center | 7/9 (2) |
| Internet Explorer 5.2 | 9/9 | | Kid Pix Deluxe 4 | 9/9 (3) |
| Bejeweled 2 | 9/9 | | Build-a-lot 2 | 8/9 (2) |
| SimCity 4 Rush Hour | 4/4 in its own test (4) | | Logic Platinum 5.4 | 8/9 (2) |
| Hex Fiend | 9/9 | | TextWrangler 3 | 9/9 |
| Transmission 2.22 | 9/9 | | The Unarchiver 3.2 | 9/9 |
| StuffIt Expander, DropStuff, DropZip, DropTar | all steps | | xACT | 9/9 |
| Mactracker | 9/9 (7) | | Database Utility | 8/8 |
| G4 Quasar, Reactor, Supernova, Event Horizon, Fractal Explorer | all steps | | Blender 2.63 (render and save checks) | 26/26 |
| Skitch | not a PowerPC app (5) | | Wireshark 1.9.2 | works with XQuartz (6) |

1. Acrobat stops at personalization, as on Mojave (where the same test gives 2/4).
2. The test's generic typing and copy step has nothing to type into in these apps (a help window,
   a full-screen game board, a sequencer that stops at its copy-protection key check); the app
   itself runs. These three are not part of the Mojave test set.
3. 8/9 in the full run: the "no new reports" step counted a report from another test run at the
   same time; 9/9 on its own.
4. A full-screen game: the generic hide/show and copy steps do not fit it; its own test
   (`tests/sc4test.py`: region view, a city, the menus, quit) passes 4/4, as on Mojave. Checked by
   hand as well: the region view animates and a click on a city opens its information.
5. The test set's Skitch is an Intel-only build; Rosettus says so and stops (exit 86).
6. Not part of the behavior test (a GTK program in XQuartz). Checked by hand with XQuartz 2.8.5:
   Rosettus starts XQuartz, and Wireshark opens a capture file with its packet list, details and
   bytes; tshark and capinfos read the same file from Terminal.
7. All 532 models are listed, with their icons, as on Mojave. This needs the shim's correction of
   REALbasic's string buffers (see NOTES-catalina.md); `OAH_NO_RBSTRFIX=1` turns it off.

## Command-line tools and Java

* `tests/catalina/clitest.c`: arguments with spaces, environment, current folder, relative paths,
  piped and terminal standard input, standard output and error, exit status and Ctrl-C: all pass.
* PowerPC Java 1.3.1_16, 1.4.2_21 and 1.5.0_19 (from Apple's Tiger Java updates): `-version`, `javac`,
  a threads and collections test (JTest) and a Swing window (SwingTest) pass, 3 runs each.

## Rosettus for Mojave features

* PowerPC screen savers (PPC Test Saver): full screen and smooth in the idle screen saver and the
  System Preferences preview; a mouse move ends both.
* Preference panes: Rosettus Preference Pane Viewer opens Adobe Version Cue CS2's pane.
* Adobe Version Cue CS2: the background service starts the server and MySQL ("up and running"); the
  pane shows On.
* Flash in QuickTime: `install_qtflash.sh` on the QuickTime 6.5.2 installer disk image builds and
  installs RosettusFlash.component; a PowerPC test program (QuickTime 7 with it) finds the Flash
  media handler and importers, opens a Kid Pix sticker .swf as a movie with a Flash track and draws
  its frames.
* Kid Pix Deluxe 4's animated sticker palettes: the Animals thumbnails draw, and a sticker dragged
  from the palette lands on the canvas, as on Mojave.
* Sharp text on Retina displays: TextWrangler's text and menu titles draw at 2x.
* Drag and drop: a file dragged from Finder onto TextWrangler opens, and a selected word dragged to
  the end of the line in TextWrangler moves there.
* Menu bar apps (four PowerPC test apps built on Snow Leopard, `tests/ppc/menubar`): an app with
  a Dock icon and a status item, a menu-bar-only app (LSUIElement) and one that counts in its
  item's title every 2 s, rebuilds its menu each time it opens and moves itself into and out of
  the Dock. All three items show side by side in the menu bar; each menu opens under its item, and
  Say Hello, Open Window and Quit work. Show in Dock gives a Dock icon (Launch Services type
  Foreground), Hide from Dock removes it again (UIElement) and the item stays. Light in Dark Mode.
  When one of these apps quits (from its menu, or killed), the other items move right to close the gap and their menus still open under them; after Show in Dock the app's own menus take over the menu bar and open. A fourth test app has a picture item: a black template glyph with a white highlighted version, a colour icon, and icon with title; each shows, highlights while its menu is open and resizes correctly, also in Dark Mode.
* Textured windows (Pages, Keynote, Numbers: the template / theme chooser and documents) move when dragged by the title bar or toolbar background; a drag that starts on a control does not move them.
* iWork '09 charts: 2D and 3D charts are inserted and drawn in Pages and Keynote (tested: 2D column, 3D column, 3D pie).
* Dashboard widgets do not apply: Catalina has no Dashboard.
