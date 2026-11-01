#!/usr/bin/env bash
# Continue (or stop) MuZero 9x9 self-play training in the background.
#
# Resumes from <data-dir>/checkpoint with the settings of the 8-hour run of
# 2026-11-01: peak lr 5e-4 cosine-decayed so it reaches its floor (5e-5) just
# before the time budget runs out, snapshots every 4000 steps, 128-game
# in-run evaluation matches. The trainer runs detached (survives closing the
# terminal) and its output is appended to <data-dir>/train_stdout.log.
#
#   scripts/muzero_9x9_train.sh                  # 8 hours
#   scripts/muzero_9x9_train.sh --minutes 120    # 2 hours
#   scripts/muzero_9x9_train.sh --dry-run        # print the command only
#   scripts/muzero_9x9_train.sh --status         # is it running, at which step
#   scripts/muzero_9x9_train.sh --stop           # save a checkpoint and stop
#
# Anything after "--" is passed to muzero_9x9_selfplay as is, e.g.
#   scripts/muzero_9x9_train.sh -- --simulations 64
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BIN="$ROOT/build/bin/muzero_9x9_selfplay"
DATA_DIR="data/muzero_9x9"
MINUTES=480
LR=5e-4
HORIZON=""          # empty: derived from the time budget
RATE=""             # learner steps per second; empty: measured from train_log.csv
ACTION=start
EXTRA=()

usage() { sed -n '2,19p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; }

while [[ $# -gt 0 ]]; do
  case "$1" in
    --minutes)    MINUTES="$2"; shift 2 ;;
    --lr)         LR="$2"; shift 2 ;;
    --lr-horizon) HORIZON="$2"; shift 2 ;;
    --rate)       RATE="$2"; shift 2 ;;
    --data-dir)   DATA_DIR="$2"; shift 2 ;;
    --dry-run)    ACTION=dry-run; shift ;;
    --status)     ACTION=status; shift ;;
    --stop)       ACTION=stop; shift ;;
    -h|--help)    usage; exit 0 ;;
    --)           shift; EXTRA=("$@"); break ;;
    *) echo "unknown option: $1" >&2; usage >&2; exit 2 ;;
  esac
done

cd "$ROOT"
PID_FILE="$DATA_DIR/train.pid"
LOG="$DATA_DIR/train_stdout.log"

# PIDs of every running trainer, whoever started it
running_pids() { pgrep -f "^([^ ]*/)?muzero_9x9_selfplay( |$)" || true; }

current_step() { cat "$DATA_DIR/checkpoint/step.txt" 2>/dev/null || echo 0; }

case "$ACTION" in
  status)
    pids=$(running_pids)
    if [[ -z "$pids" ]]; then
      echo "not running; checkpoint at step $(current_step)"
    else
      echo "running (pid $(echo $pids)); checkpoint at step $(current_step)"
      grep '^\[train\]' "$LOG" | tail -1 | cut -c1-160
      grep '^\[eval\]' "$LOG" | tail -1 | cut -c1-160
    fi
    exit 0 ;;
  stop)
    pids=$(running_pids)
    if [[ -z "$pids" ]]; then echo "not running"; exit 0; fi
    echo "stopping pid $(echo $pids) (it saves a checkpoint first)..."
    kill -TERM $pids
    for _ in $(seq 1 300); do [[ -z "$(running_pids)" ]] && break; sleep 1; done
    if [[ -n "$(running_pids)" ]]; then echo "still running after 5 minutes" >&2; exit 1; fi
    rm -f "$PID_FILE"
    echo "stopped; checkpoint at step $(current_step)"
    exit 0 ;;
esac

[[ -x "$BIN" ]] || { echo "missing $BIN; build it first: make build" >&2; exit 1; }
[[ -f "$DATA_DIR/checkpoint/model.pt" ]] || echo "warning: no checkpoint in $DATA_DIR, training starts from scratch" >&2

# two trainers on one data directory overwrite each other's checkpoints
pids=$(running_pids)
if [[ -n "$pids" ]]; then
  echo "a trainer is already running (pid $(echo $pids)); stop it first: $0 --stop" >&2
  exit 1
fi

START_STEP=$(current_step)

# learner throughput of the most recent run in train_log.csv: its rows are
# the trailing ones whose seconds column keeps increasing
if [[ -z "$RATE" ]]; then
  RATE=$(awk -F, 'NR > 1 {
      if ($1 + 0 < prev) { s0 = $1; k0 = $2 }
      if (s0 == "") { s0 = $1; k0 = $2 }
      prev = $1 + 0; s1 = $1; k1 = $2 }
    END { if (s1 - s0 > 600) printf "%.2f", (k1 - k0) / (s1 - s0); else print "" }' \
    "$DATA_DIR/train_log.csv" 2>/dev/null || true)
  RATE=${RATE:-4.8}
fi

# reach the lr floor at 95% of the budget so the last stretch trains annealed
if [[ -z "$HORIZON" ]]; then
  HORIZON=$(awk -v s="$START_STEP" -v r="$RATE" -v m="$MINUTES" \
    'BEGIN { printf "%d", s + r * m * 60 * 0.95 }')
fi

CMD=("$BIN" --data-dir "$DATA_DIR" --minutes "$MINUTES"
  --dims 128 --heads 4 --repr-layers 4 --dyn-layers 3 --pred-layers 1
  --batch-size 384 --unroll 5 --replay 400000 --min-replay 20000 --sample-reuse 2
  --lr "$LR" --lr-horizon "$HORIZON"
  --actors 4 --games-per-actor 512 --simulations 32 --fast-simulations 8 --full-search-prob 0.25
  --root gumbel --komi 7.5
  --snapshot-interval 4000 --eval-games 128 --eval-baseline-games 128
  ${EXTRA[@]+"${EXTRA[@]}"})

echo "resume from step $START_STEP, ${MINUTES} min at ~${RATE} steps/s, lr $LR, lr horizon $HORIZON"
if [[ "$ACTION" == dry-run ]]; then
  printf '%q ' "${CMD[@]}"; echo
  exit 0
fi

echo "===== run started $(date -Is): resume from step $START_STEP, $MINUTES min, lr $LR, lr horizon $HORIZON =====" >> "$LOG"
echo "$START_STEP" > "$DATA_DIR/run_start_step.txt"
# setsid detaches from this terminal; as a single background command $! is
# the trainer itself (setsid and nohup exec it in place)
setsid nohup "${CMD[@]}" >> "$LOG" 2>&1 < /dev/null &
PID=$!
sleep 3
if ! kill -0 "$PID" 2>/dev/null; then
  echo "trainer exited right away; see $LOG:" >&2
  tail -5 "$LOG" >&2
  exit 1
fi
echo "$PID" > "$PID_FILE"
echo "started pid $PID; log: $LOG"
echo "watch it with: scripts/muzero_9x9_monitor.sh"
