#!/bin/bash
set -u
B="$HOME/ns-3.46.1/build/scratch/ns3.46.1-fig25_vr-default"
O="$HOME/mlo/results/VRns3"
run_one() {
  m=$1; w=$2; mcs=$3; r=$4
  f="$O/${m}_W${w}_M${mcs}_R${r}.csv"
  [ -s "$f" ] && return 0
  out=$("$B" --mode=$m --channelWidth=$w --mcs=$mcs --simTime=6 --dlBurstMs=0.05 --RngRun=$r 2>&1)
  dl=$(echo "$out" | grep "VR DL p75" | grep -oE "[0-9.]+ ms" | grep -oE "[0-9.]+")
  ul=$(echo "$out" | grep "VR UL p90" | grep -oE "[0-9.]+ ms" | grep -oE "[0-9.]+")
  if [ -n "$dl" ] && [ -n "$ul" ]; then echo "$m,$w,$mcs,$r,$dl,$ul" > "$f"; fi
}
export -f run_one; export B O
for pass in 1 2 3; do
  : > /tmp/todo_vrns3.txt
  for m in SLO MLO; do for w in 20 40 80 160 320; do for mcs in $(seq 0 13); do for r in 1 2 3; do
    f="$O/${m}_W${w}_M${mcs}_R${r}.csv"
    [ -s "$f" ] || echo "$m $w $mcs $r" >> /tmp/todo_vrns3.txt
  done; done; done; done
  left=$(wc -l < /tmp/todo_vrns3.txt)
  echo "[$(date +%H:%M:%S)] pass $pass: $left to run"
  [ "$left" -eq 0 ] && break
  xargs -P 16 -n 4 bash -c "run_one \$0 \$1 \$2 \$3" < /tmp/todo_vrns3.txt
done
echo DONE
