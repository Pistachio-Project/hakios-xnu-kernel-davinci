#!/bin/sh
# The firmware of a phone's modem and Wi-Fi where board/sm7150_mpss.c reads it (/hakios/fw/ of the small image, which
# darwin/userland/root26-darwin.sh makes from firmware/BOARD/fw/): fw/modem/ (the modem's image), fw/tftp/ (what the
# modem fetches from the host: wlanmdsp.mbn, modem_pr/) and fw/wlan/ (board data: bdwlan.bNN by the chip's board id,
# bdwlan.bin). From the phone's modem partition, pulled to firmware/BOARD/modem/. Nothing here is committed.
# usage: tools/stage-wcn3990-fw.sh BOARD [SOURCE_DIR]
set -e
board=${1:?usage: stage-wcn3990-fw.sh BOARD [SOURCE_DIR]}
root=$(cd "$(dirname "$0")/.." && pwd)
src=${2:-$root/firmware/$board/modem}
dst=$root/firmware/$board/fw
[ -f "$src/modem.mdt" ] && [ -f "$src/wlanmdsp.mbn" ] || { echo "$src: no modem.mdt and wlanmdsp.mbn (the phone's modem partition's image/)" >&2; exit 1; }
rm -rf "$dst/modem" "$dst/tftp" "$dst/wlan"
mkdir -p "$dst/modem" "$dst/tftp" "$dst/wlan"
cp "$src"/modem.mdt "$src"/modem.b[0-9][0-9] "$dst/modem/"
cp "$src/wlanmdsp.mbn" "$dst/tftp/"
[ ! -d "$src/modem_pr" ] || cp -r "$src/modem_pr" "$dst/tftp/"
cp "$src"/bdwlan.* "$dst/wlan/"
echo "stage-wcn3990-fw: $(find "$dst/modem" "$dst/tftp" "$dst/wlan" -type f | wc -l) files in $dst (modem, tftp, wlan)"
