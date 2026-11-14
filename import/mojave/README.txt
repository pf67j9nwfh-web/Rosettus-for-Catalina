Mojave Export Kit (Rosettus for Catalina)
==========================================

Catalina cannot run 32-bit code and has no 32-bit Intel libraries. Rosettus for Catalina runs
Rosetta's translator (a 32-bit Intel program) on Mojave's 32-bit libraries, which you copy once from
your own copy of macOS 10.14 Mojave. Two ways:

1. From a Mac running Mojave (best; another Mac, a second disk or partition, or a virtual machine):
   copy this folder there and double-click export_from_mojave.command (or run
   ./export_from_mojave.py). It writes ~/Desktop/Rosettus-Mojave-export.tar.gz (about 510 MB, a few
   minutes): the i386 dyld shared cache, the i386 code of about 840 libraries and plug-ins, and the
   32-bit commpage. Nothing is changed on that Mac; no developer tools are needed.

2. Without a Mojave Mac, from Apple's "Install macOS Mojave" app (about 6 GB; it can be downloaded
   from Apple: support article HT211683), on any Mac
   with macOS 10.13 or later, Catalina included:

       ./from_installer.sh "/Applications/Install macOS Mojave.app"
       ./from_installer.sh "Install macOS Mojave.zip"          (a zip of the app works too)

   It mounts the installer's disk image read-only, reads the libraries out of its system package
   (about 11 GB of free space needed while it runs, 10-20 minutes; no administrator password),
   copies their i386 code, and builds the i386 shared cache from them with the installer's own
   update_dyld_shared_cache, as installing Mojave would. The result is the same
   ~/Desktop/Rosettus-Mojave-export.tar.gz (somewhat larger). The commpage is left empty: Rosettus
   fills it from Catalina's own at run time.

Then give the archive to the Rosettus for Catalina installer together with the Snow Leopard export
(see the README). The archive holds Apple software from your own Mojave: for your own Macs only.

Files: export_from_mojave.py (the exporter), from_installer.sh and payload_extract.py (the
installer route), mojave_i386_files.txt and mojave_i386_cache_files.txt
(the files it copies), mojave_i386_expected.sha1 (checksums of the Mojave build Rosettus for Catalina
was tested with, 10.14.6 18G103: a different build is reported, not refused), commdump (dumps the
32-bit commpage; source in commdump.c).
