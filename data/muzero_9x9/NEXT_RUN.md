# MuZero 9x9: state and next training direction

## State after 2026-11-18 (run 4)

- Checkpoint and final_model.pt at step 407954.
- Model weights are 4c's; it is level with the run 3 final (step_00277259):
  254-258 over 512 games at 64 simulations (-3 Elo).
- In-run Elo ~740-760, mcts_rollout_200 ~790. The model has been flat since
  ~step 260k.

## What was tried on 2026-11-18 and did not help

| run | setup                                         | vs step_00277259 (256-game arena)          |
|-----|-----------------------------------------------|--------------------------------------------|
| 4a  | 64 sims, warm restart at lr 3e-4              | fell steadily to -75..-120, stopped at 312554 |
| 4b  | run 3 weights, 64 sims, lr 1e-4               | flat at ~-35 for 10 snapshots, stopped at 357230 |
| 4c  | run 3 weights, 32 sims, lr 1e-4 -> 1e-5       | ~-25 mid-run, level once annealed (final -3) |

4a/4b snapshots are in `abandoned/` (not in git, safe to delete).

Lessons:
- Before 2026-11-18 the lr schedule counted from step 0, so earlier "warm
  restarts" began well below their nominal peak. `--lr-start` (set to the
  resume step by `scripts/muzero_9x9_train.sh`) fixes that.
- An annealed reference beats any snapshot trained at a higher lr. Compare
  only models at the end of their own cosine decay, with 512 games. Mid-run
  snapshots trailing by 20-40 Elo is normal and not a reason to stop.
- lr 3e-4 is too high for this model now; 1e-4 is safe.
- 64-sim Gumbel targets looked worse (4b), but 4b was stopped before it
  annealed, so that verdict is not certain.

## Direction for the next run

1. **Gumbel target settings, short annealed runs (~2 h each).** The policy
   target is softmax(logits + sigma(completed Q)) with c_visit 50, c_scale 0.1
   and min-max rescaled Q (`src/muzero/mcgs.h`, `MCGSConfig`). Expose them as
   flags, then try a softer target (c_scale 0.05), at 32 and at 64 sims. Each
   run: `--lr 1e-4`, full cosine within the run, final model vs
   step_00407954 over 512 games.
2. **If nothing in step 1 gains > +30 Elo, the model is capacity-bound**
   (1.7M params: d128, 4/3/1 layers). Start a larger model (e.g. d192 or
   d256, 6 repr layers) in a new data dir. To skip the weak early phase,
   first train it offline on the existing experience in
   `data/muzero_9x9/experience`, then run self-play. Judge it against
   step_00407954.
