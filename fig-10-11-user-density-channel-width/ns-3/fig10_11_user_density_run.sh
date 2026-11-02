#!/bin/bash
set -u
B="$HOME/ns-3.46.1/build/scratch/4_2_1_e2e/ns3.46.1-fig10_11_user_density-default"
O="$HOME/mlo/results/NS3_421"
run_one() {
  nl=$1; cw=$2; ns=$3; rho=$4; rng=$5
  ri=$(awk "BEGIN{printf \"%d\", $rho*10}")
  f="$O/L${nl}_W${cw}_S${ns}_R${ri}_rng${rng}.csv"
  [ -s "$f" ] && return 0
  out=$("$B" --RngRun=$rng --simTime=5.0 --payloadSize=1500 --nMpdus=1024 \
        --numLinks=$nl --channelWidth=$cw --txPowerDbm=23.0 --nStas=$ns \
        --normalizedLoad=$rho 2>&1)
  lat=$(echo "$out" | grep "Mean DL Latency" | grep -oE "[0-9.]+ ms" | grep -oE "[0-9.]+")
  thr=$(echo "$out" | grep "Mean DL Throughput" | grep -oE "[0-9.]+ Mb" | grep -oE "[0-9.]+")
  sat=0; echo "$out" | grep -q "^SATURATED" && sat=1
  [ -n "$lat" ] && echo "$nl,$cw,$ns,$rho,$rng,$lat,$thr,$sat" > "$f"
}
export -f run_one; export B O
for pass in 1 2; do
  : > /tmp/todo_ns3421.txt
  for nl in 1 2; do for cw in 80 160; do for ns in 1 4 10; do
    for ri in 0 1 2 3 4 5 6 7 8 9 10; do
      rho=$(awk "BEGIN{printf \"%.1f\", $ri/10}")
      for rng in 1001 1002 1003 1004 1005 1006 1007 1008 1009 1010; do
        f="$O/L${nl}_W${cw}_S${ns}_R${ri}_rng${rng}.csv"
        [ -s "$f" ] || echo "$nl $cw $ns $rho $rng" >> /tmp/todo_ns3421.txt
      done; done; done; done; done
  left=$(wc -l < /tmp/todo_ns3421.txt)
  echo "[$(date +%H:%M:%S)] pass $pass: $left to run"
  [ "$left" -eq 0 ] && break
  xargs -P 4 -n 5 bash -c "run_one \$0 \$1 \$2 \$3 \$4" < /tmp/todo_ns3421.txt
done
echo DONE
