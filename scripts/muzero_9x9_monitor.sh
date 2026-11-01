#!/usr/bin/env bash
# Watch a MuZero 9x9 training run and check it keeps improving.
#
# Every new snapshot in <data-dir>/snapshots is played against a fixed
# reference model with muzero_9x9_arena (256 games, 64 simulations by
# default), because the trainer's own 128-game, 32-simulation matches are too
# noisy to judge progress snapshot to snapshot. Each result is printed next to
# the snapshot's in-run Elo and appended to <data-dir>/eval/arena.csv. A
# warning is printed when the last two snapshots both fall well below the best
# one so far. When the trainer exits, the final model is matched against the
# reference and the best snapshot (512 games each) and the monitor exits.
#
#   scripts/muzero_9x9_monitor.sh                       # follow the current run
#   scripts/muzero_9x9_monitor.sh --once                # check pending snapshots, then exit
#   scripts/muzero_9x9_monitor.sh --since-step 100000   # also (re)check older snapshots
#   scripts/muzero_9x9_monitor.sh --reference data/muzero_9x9/snapshots/step_00013673.pt
#
# Options: --data-dir DIR, --reference FILE (default: the snapshot the run
# resumed from), --since-step N (default: the run's start step), --games N,
# --simulations N, --regress-elo N (warning threshold, default 60),
# --interval SECONDS (polling, default 60), --log FILE, --once, --no-final.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ARENA="$ROOT/build/bin/muzero_9x9_arena"
DATA_DIR="data/muzero_9x9"
REFERENCE=""
SINCE=""
GAMES=256
SIMS=64
REGRESS=60
INTERVAL=60
LOG=""
ONCE=0
FINAL=1

usage() { sed -n '2,21p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; }

while [[ $# -gt 0 ]]; do
  case "$1" in
    --data-dir)    DATA_DIR="$2"; shift 2 ;;
    --reference)   REFERENCE="$2"; shift 2 ;;
    --since-step)  SINCE="$2"; shift 2 ;;
    --games)       GAMES="$2"; shift 2 ;;
    --simulations) SIMS="$2"; shift 2 ;;
    --regress-elo) REGRESS="$2"; shift 2 ;;
    --interval)    INTERVAL="$2"; shift 2 ;;
    --log)         LOG="$2"; shift 2 ;;
    --once)        ONCE=1; shift ;;
    --no-final)    FINAL=0; shift ;;
    -h|--help)     usage; exit 0 ;;
    *) echo "unknown option: $1" >&2; usage >&2; exit 2 ;;
  esac
done

cd "$ROOT"
[[ -x "$ARENA" ]] || { echo "missing $ARENA; build it first: make build" >&2; exit 1; }
SNAP_DIR="$DATA_DIR/snapshots"
LOG="${LOG:-$DATA_DIR/eval/arena.csv}"
mkdir -p "$(dirname "$LOG")"
[[ -f "$LOG" ]] || echo "time,snapshot,reference,games,simulations,wins,draws,losses,score,elo_diff,inrun_elo" > "$LOG"

step_of() { local b; b=$(basename "$1" .pt); echo $((10#${b#step_})); }

START=$(cat "$DATA_DIR/run_start_step.txt" 2>/dev/null || cat "$DATA_DIR/checkpoint/step.txt" 2>/dev/null || echo 0)
SINCE="${SINCE:-$START}"

# default reference: the newest snapshot at or before the run's start step
if [[ -z "$REFERENCE" ]]; then
  for f in $(ls "$SNAP_DIR"/step_*.pt 2>/dev/null | sort); do
    (( $(step_of "$f") <= START )) && REFERENCE="$f"
  done
fi
[[ -n "$REFERENCE" && -f "$REFERENCE" ]] || { echo "no reference model; pass --reference FILE" >&2; exit 1; }

trainer_running() { pgrep -f "^([^ ]*/)?muzero_9x9_selfplay( |$)" >/dev/null; }

inrun_elo() {   # the trainer's own rating of a snapshot, once it has evaluated it
  awk -F, -v n="$1" '$1 == n { print $2 }' "$DATA_DIR/eval/ratings.csv" 2>/dev/null
}

# match a against b; prints "wins draws losses score elo"
match() {
  local out
  out=$("$ARENA" -g "$3" -s "$SIMS" "$1" "$2" 2>&1) || { echo "arena failed: $out" >&2; return 1; }
  sed -E 's/.*: ([0-9]+)-([0-9]+)-([0-9]+) \(score ([0-9.]+), elo diff ([-+0-9]+)\).*/\1 \2 \3 \4 \5/' <<< "$out"
}

declare -A DONE=()
BEST_ELO=""
BEST_SNAP=""
RECENT=()       # elo diffs of the snapshots checked by this monitor, in order

# results already in the log for this reference and settings count as done
while IFS=, read -r _ snap ref games sims _ _ _ _ elo _; do
  [[ "$ref" == "$REFERENCE" && "$games" == "$GAMES" && "$sims" == "$SIMS" ]] || continue
  DONE["$snap"]=1
  if [[ -z "$BEST_ELO" ]] || (( elo > BEST_ELO )); then BEST_ELO=$elo; BEST_SNAP=$snap; fi
done < <(tail -n +2 "$LOG")

echo "reference: $REFERENCE | checking snapshots after step $SINCE | $GAMES games, $SIMS simulations | log: $LOG"

check_snapshot() {
  local f="$1" name r w d l score elo inrun flag=""
  name=$(basename "$f" .pt)
  r=$(match "$f" "$REFERENCE" "$GAMES") || return 0
  read -r w d l score elo <<< "$r"
  inrun=$(inrun_elo "$name")
  echo "$(date -Is),$f,$REFERENCE,$GAMES,$SIMS,$w,$d,$l,$score,$elo,$inrun" >> "$LOG"
  DONE["$f"]=1
  RECENT+=("$elo")
  if [[ -z "$BEST_ELO" ]] || (( elo > BEST_ELO )); then BEST_ELO=$elo; BEST_SNAP=$f; flag=" (new best)"; fi
  printf '%s  %-20s vs reference %s-%s  elo %+4d%s | in-run elo %s\n' \
    "$(date +%H:%M)" "$name" "$w" "$l" "$elo" "$flag" "${inrun:-pending}"
  local n=${#RECENT[@]}
  if (( n >= 2 )) && (( RECENT[n-1] < BEST_ELO - REGRESS && RECENT[n-2] < BEST_ELO - REGRESS )); then
    echo "WARNING: the last two snapshots are more than $REGRESS Elo below the best ($BEST_SNAP, $BEST_ELO)."
    echo "         Check directly: $ARENA -g 512 -s $SIMS $f $BEST_SNAP"
  fi
}

# snapshots newer than SINCE not checked yet, oldest first; a file still being
# written (modified in the last 10 s) waits for the next pass
pending() {
  local f now; now=$(date +%s)
  for f in $(ls "$SNAP_DIR"/step_*.pt 2>/dev/null | sort); do
    (( $(step_of "$f") > SINCE )) || continue
    [[ -n "${DONE[$f]:-}" || "$f" == "$REFERENCE" ]] && continue
    (( now - $(stat -c %Y "$f") >= 10 )) || continue
    echo "$f"
  done
}

while true; do
  # read this before checking, so a snapshot saved on exit is not missed
  running=0; trainer_running && running=1
  for f in $(pending); do check_snapshot "$f"; done
  (( ONCE )) && break
  (( running )) || break
  sleep "$INTERVAL"
done

if (( FINAL )) && ! trainer_running && [[ -f "$DATA_DIR/final_model.pt" ]] && (( ! ONCE )); then
  echo "trainer stopped; final model checks ($((GAMES * 2)) games):"
  read -r w d l score elo <<< "$(match "$DATA_DIR/final_model.pt" "$REFERENCE" $((GAMES * 2)))"
  printf '  final_model vs %s: %s-%s  elo %+d\n' "$(basename "$REFERENCE" .pt)" "$w" "$l" "$elo"
  if [[ -n "$BEST_SNAP" && "$BEST_SNAP" != "$REFERENCE" ]]; then
    read -r w d l score elo <<< "$(match "$DATA_DIR/final_model.pt" "$BEST_SNAP" $((GAMES * 2)))"
    printf '  final_model vs %s (best snapshot): %s-%s  elo %+d\n' "$(basename "$BEST_SNAP" .pt)" "$w" "$l" "$elo"
  fi
elif (( ! ONCE )); then
  echo "trainer is not running"
fi
