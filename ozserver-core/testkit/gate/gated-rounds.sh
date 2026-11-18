#!/bin/bash
# Load-gated ×3 quiet + ×3 concurrent test rounds for a server worktree.
# Gate: 1-min load < 8 for 2 min before each round. During each package run a
# sampler records the 1-min load every 5 s; the PEAK is printed per run.
# Usage: gated-rounds.sh <worktree> <outdir> > <outdir>/rounds.txt
# Verdict (counts RESULTS, not lines; GATE-VERDICT-1):
#   node ozserver-core/testkit/gate/verdict.js <outdir>/rounds.txt 15   (+3 per extra suite run appended)
WT=$1; OUT=$2; mkdir -p "$OUT"
la(){ sysctl -n vm.loadavg | awk '{print $2}'; }
gate(){ local w=0; while :; do local ok=1; for k in 1 2 3 4; do awk -v l=$(la) 'BEGIN{exit !(l<8)}' || { ok=0; break; }; sleep 30; done; [ $ok = 1 ] && return 0; w=$((w+1)); [ $w -gt 240 ] && { echo "load never < 8 for 2 min"; exit 2; }; sleep 30; done; }
# run <logfile> <dir>: runs npm test with a peak-load sampler; echoes "pass/fail start peak"
run(){ local log=$1 dir=$2 pk=$1.peak; echo $(la) > $pk
  ( while :; do l=$(la); awk -v l=$l -v p=$(cat $pk) 'BEGIN{exit !(l>p)}' && echo $l > $pk; sleep 5; done ) & local sp=$!
  local st=$(la); (cd "$WT/$dir" && npm test) > "$log" 2>&1; kill $sp 2>/dev/null; wait $sp 2>/dev/null
  echo "$(grep -E '^# (pass|fail)' "$log" | tr '\n' ' ') start $st peak $(cat $pk)"; }
cd "$WT" && echo "HEAD $(git rev-parse HEAD) diff $(git diff | shasum -a 256 | cut -c1-16)"
for i in 1 2 3; do gate; for d in ozserver-core ozpms/server ozlock/server; do
  echo "quiet$i $d $(run $OUT/q${i}_${d//\//_}.log $d)"; done; done
for i in 1 2 3; do gate
  run $OUT/c${i}_ozlock.log ozlock/server > $OUT/c${i}_ozlock.res & run $OUT/c${i}_ozpms.log ozpms/server > $OUT/c${i}_ozpms.res; wait
  echo "conc$i ozlock $(cat $OUT/c${i}_ozlock.res) | ozpms $(cat $OUT/c${i}_ozpms.res)"; done
cd "$WT" && echo "HEAD $(git rev-parse HEAD) diff $(git diff | shasum -a 256 | cut -c1-16)"
