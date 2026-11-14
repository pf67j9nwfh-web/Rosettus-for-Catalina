#!/bin/bash
# Double-click on the Mojave Mac: makes Rosettus-Mojave-export.tar.gz on the Desktop.
cd "$(dirname "$0")" && /usr/bin/python export_from_mojave.py "$@"
echo; read -n 1 -s -r -p "Press any key to close this window."
