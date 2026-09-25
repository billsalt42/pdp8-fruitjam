#!/bin/sh
# Rebuild the OS/8 RK05 images in sdcard/ from DEC's distribution DECtapes,
# using the PiDP-8/I project's os8-run tooling (which drives SIMH).
# Needs: git, a C compiler, python3 with pexpect, perl.
set -e
HERE=$(cd "$(dirname "$0")/.." && pwd)
WORK=${WORK:-/tmp/pidp8i-build}
rm -rf "$WORK"
git clone --depth 1 https://github.com/tangentsoft/pidp8i "$WORK"
cd "$WORK"
# Fruit Jam boot banner
sed -i '2s/.*/Adafruit Fruit Jam PDP-8\/E - OS\/8 V3D - KBM V3Q - CCL V1F/; 3s/.*/Configured on @BUILDTS@/' media/os8/init.tx.in
sed -i '2s/^PiDP-8\/I @VERSION@/Adafruit Fruit Jam PDP-8\/E/; 3s/by @BUILDUSER@ //' media/os8/ock-init.tx.in
# the configure script refuses to run as root; harmless for building images
sed -i 's/^if {$instusr == "root"} {/if {0} {/' auto.def
USER=${USER:-builder} ./configure --disable-os8-src --disable-cc8-cross
USER=${USER:-builder} make -j4
cp bin/v3d.rk05 "$HERE/sdcard/os8.rk05"
cp bin/ock.rk05 "$HERE/sdcard/ock.rk05"
# pad to the full RK05 size (203 cyl x 2 heads x 16 sectors x 512 bytes)
truncate -s 3325952 "$HERE/sdcard/os8.rk05" "$HERE/sdcard/ock.rk05"
echo "Images written to $HERE/sdcard/"
