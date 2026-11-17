#!/bin/sh
# Replay a real Nasdaq TotalView-ITCH 5.0 day and check it two ways:
#  1. every frame of the day goes through the decoder, in 1.5 GB pieces
#     (the uncompressed day, about 8 GB, is on disk until each piece is read);
#  2. for each symbol, the top of book after every change is compared with an
#     independent Python reconstruction of the same messages.
#
#   scripts/verify_itch_day.sh 12302019.NASDAQ_ITCH50.gz AAPL,MSFT,AMZN,TSLA,QQQ [WORKDIR]
#
# Files come from Nasdaq's public server (see docs/MARKET_DATA.md section 7);
# they are several gigabytes and are not committed. Needs a release build:
#   cmake --preset release && cmake --build --preset release
set -eu
GZ=$1
SYMBOLS=$2
WORK=${3:-itch_work}
B=build/release
mkdir -p "$WORK/parts"

gzip -dc "$GZ" | $B/itch_extract --symbols "$SYMBOLS" --out "$WORK/day.bin" \
  --split-dir "$WORK/parts" --split-bytes 1500000000 > "$WORK/extract.txt"
cat "$WORK/extract.txt"

total=0
for p in "$WORK"/parts/part*.bin; do
  n=$($B/lobkit_replay --itch "$p" --parse-only | sed -n 's/^parse only: \([0-9]*\) messages.*final status End$/\1/p')
  [ -n "$n" ] || { echo "decoder did not reach the end of $p"; exit 1; }
  total=$((total + n))
  rm -f "$p"
done
echo "decoder: $total messages delivered, every piece ended cleanly"

for s in $(echo "$SYMBOLS" | tr ',' ' '); do
  echo "== $s"
  $B/lobkit_replay --itch "$WORK/day.bin" --symbol "$s" --top-csv "$WORK/top_$s.lobkit.csv" | grep -v '^grid'
  python3 scripts/itch_reference_top.py "$WORK/day.bin" "$s" "$WORK/top_$s.ref.csv"
  python3 scripts/compare_top.py "$WORK/top_$s.lobkit.csv" "$WORK/top_$s.ref.csv"
done
