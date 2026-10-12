#!/bin/sh
# Downloads a free LOBSTER sample and unpacks it into data/lobster/ (git-ignored).
#
#   scripts/get_lobster_sample.sh [URL]
#
# The URL below is the pattern the LOBSTER site used for its sample files when I
# last knew it, and it has NOT been verified by me: the host is blocked in the
# environment this script was written in. If the download fails, do it by hand:
#
#   1. Open https://lobsterdata.com/info/DataSamples.php in a browser.
#   2. Download one sample, for example AAPL 2012-06-21 with 10 levels
#      (a zip with two csv files).
#   3. Unzip it into data/lobster/ in this repository. The files are named like
#         AAPL_2012-06-21_34200000_57600000_message_10.csv
#         AAPL_2012-06-21_34200000_57600000_orderbook_10.csv
#      (names as documented by LOBSTER; check what the zip actually contains).
#
# Then run the independent check:
#   build/release/lobkit_replay --lobster data/lobster/<message file> \
#       --book data/lobster/<orderbook file> --verify
set -eu
URL="${1:-https://lobsterdata.com/info/sample/LOBSTER_SampleFile_AAPL_2012-06-21_10.zip}"
DEST="$(cd "$(dirname "$0")/.." && pwd)/data/lobster"
mkdir -p "$DEST"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
echo "downloading $URL"
if ! curl -fsSL --retry 2 -o "$TMP/sample.zip" "$URL"; then
  echo "download failed. If your network policy blocks lobsterdata.com, follow the manual steps in this script." >&2
  exit 1
fi
unzip -q -o "$TMP/sample.zip" -d "$DEST"
echo "unpacked into $DEST:"
ls -l "$DEST"
