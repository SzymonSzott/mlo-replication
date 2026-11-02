#!/bin/bash
set -u
NS3=~/ns-3.46.1/build/scratch/ns3.46.1-fig21_22_23_legacy_coexistence-default
OUT=~/ns3runs/4_3b
LOG=~/ns3runs/4_3b_attempts.log
mkdir -p "$OUT"
SIM=10; GUARD=5; STAS=50; WANT=5; MAXSEED=40; PAR=2

point() {
  e=$1; pct=$2
  have=$(ls "$OUT"/e${e}_p${pct}_r*.csv 2>/dev/null | wc -l)
  for r in $(seq 1 $MAXSEED); do
    [ "$have" -ge "$WANT" ] && break
    f="$OUT/e${e}_p${pct}_r${r}.csv"
    [ -s "$f" ] && continue
    frac=$(awk "BEGIN{print $pct/100}")
    o=$("$NS3" --caseId=B --legacyFraction=$frac --totalStas=$STAS --simTime=$SIM \
        --emlsr=$e --startupGuard=$GUARD --RngRun=$r 2>&1)
    at=$(echo "$o" | sed -n 's/^AggThr_Total: *\([0-9.eE+-]*\).*/\1/p')
    if [ -n "$at" ]; then
      al=$(echo "$o" | sed -n 's/^AggThr_Legacy: *\([0-9.eE+-]*\).*/\1/p')
      am=$(echo "$o" | sed -n 's/^AggThr_MLDs: *\([0-9.eE+-]*\).*/\1/p')
      vl=$(echo "$o" | sed -n 's/^AvgThr_Legacy: *\([0-9.eE+-]*\).*/\1/p')
      vm=$(echo "$o" | sed -n 's/^AvgThr_MLDs: *\([0-9.eE+-]*\).*/\1/p')
      jf=$(echo "$o" | sed -n 's/^JFI_Total: *\([0-9.eE+-]*\).*/\1/p')
      echo "$e,$pct,$al,$am,$at,$vl,$vm,$jf,$r" > "$f"
      have=$((have+1))
      echo "OK    e=$e pct=$pct seed=$r -> $have/$WANT" >> "$LOG"
    else
      echo "CRASH e=$e pct=$pct seed=$r" >> "$LOG"
    fi
  done
  [ "$have" -lt "$WANT" ] && echo "NIEPELNE e=$e pct=$pct ($have/$WANT)" >> "$LOG"
  return 0
}
export -f point; export NS3 OUT LOG SIM GUARD STAS WANT MAXSEED

: > "$LOG"
: > /tmp/points43.txt
for e in false true; do for pct in 10 20 30 40 50 60 70 80 90; do echo "$e $pct" >> /tmp/points43.txt; done; done
echo "punktow: $(wc -l < /tmp/points43.txt)"
xargs -P "$PAR" -n 2 bash -c 'point "$@"' _ < /tmp/points43.txt
echo "wynikow: $(ls "$OUT"/*.csv 2>/dev/null | wc -l)"
cat "$OUT"/*.csv 2>/dev/null | sort -t, -k1,1 -k2,2n > ~/ns3runs/4_3b_all.csv
echo "udane: $(grep -c ^OK "$LOG")  awarie: $(grep -c ^CRASH "$LOG")  niepelne: $(grep -c ^NIEPELNE "$LOG")"
