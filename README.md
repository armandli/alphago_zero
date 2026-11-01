# alphago_zero
Implementation of alphago zero in C++

# dependencies
- CMake >= 3.15
- GCC >= 14 (C++26: `<print>`, `std::println`)
- LibTorch (C++ distribution of PyTorch, C++20 or later headers)
- CUDA toolkit matching your LibTorch build (only for CUDA builds of LibTorch, e.g. `cu132` needs CUDA 13.2)

Fetched automatically by CMake at configure time:
- [CLI11](https://github.com/CLIUtils/CLI11) v2.7.2
- [FTXUI](https://github.com/ArthurSonzogni/FTXUI) v7.0.0
- [simdjson](https://github.com/simdjson/simdjson) v5.0.8 (model config parsing, training result output)
- [GoogleTest](https://github.com/google/googletest) v1.17.0 (unit tests; disable with `-DALPHAGO_BUILD_TESTS=OFF`)

# build
```
make                                  # finds ./libtorch or ~/.local/libtorch
make LIBTORCH=/path/to/libtorch       # explicit LibTorch location
make CXX_COMPILER=g++-14              # explicit compiler
make CUDA_HOME=/usr/local/cuda-13.2   # explicit CUDA toolkit
```
Binaries are written to `build/bin/`.

# test
```
make test
```

# MuZero 9x9

`muzero_9x9_selfplay` trains a 9x9 Go player with MuZero: a learned model
(representation, dynamics, prediction) searched by Monte-Carlo graph search
and trained on its own self-play games, with snapshots evaluated as it goes.

```
build/bin/muzero_9x9_selfplay --minutes 60                 # train, resuming from data/muzero_9x9
build/bin/muzero_9x9_selfplay --help                       # every option with its default
build/bin/vs_muzero_9x9 -m data/muzero_9x9/final_model.pt  # play against the result
```

Ctrl-C stops after saving a checkpoint; the next run resumes from it.
Everything is written under `--data-dir` (default `data/muzero_9x9`):

| path | contents |
|------|----------|
| `experience/games_*.bin` | every self-play game, reloaded into the replay buffer on resume (git-ignored, several GB) |
| `checkpoint/` | model, optimizer state, learner step, generated positions |
| `snapshots/step_*.pt` | model snapshots, one every `--snapshot-interval` steps |
| `final_model.pt` | the model at the end of the last run |
| `eval/results.csv`, `eval/ratings.csv`, `eval/summary.log` | snapshot matches and Elo ratings (random player = 0) |
| `train_log.csv`, `train_stdout.log` | losses and throughput over time |
| `config.txt` | the options of the last run |

Components (`src/muzero/`):

| file | contents |
|------|----------|
| `go_game.h` | Go as action indices, komi 7.5 area scoring, move cap, ownership |
| `go_encoder.h` | one token per point, features relative to the side to move (stones, liberties, legality, ko, last moves), 8 board symmetries |
| `transformer_muzero.h` | pre-norm transformer (RMSNorm, SwiGLU, fused attention) for representation, dynamics and prediction; token embeddings for the board |
| `mcgs.h` | Monte-Carlo graph search on the learned model, Gumbel MuZero or PUCT root, resumable for batched inference |
| `inference.h` | batched GPU inference with latents kept in a GPU pool |
| `selfplay.h` | self-play actors (many games per thread), playout cap randomization, sample ratio control |
| `experience.h` | trajectories, sliding-window replay buffer, binary experience files |
| `learner.h` | K-step unroll training, AdamW with warmup and cosine decay, bf16 |
| `evaluation.h` | batched arena, random and UCT rollout baselines, Elo, snapshot evaluator |
| `training_loop.h` | self-play, learner and evaluator threads, logging, checkpoints, resume |

Techniques, by source:

- MuZero (Schrittwieser et al. 2020): learned latent model, K = 5 step unroll,
  gradient scaling into the dynamics
- EfficientZero (Ye et al. 2021): SimSiam consistency loss between the
  unrolled latent and the encoded real next state
- Scholz et al. 2021: reconstruction of the board from every latent
- Gumbel MuZero (Danihelka et al. 2022): policy improvement with few simulations
- Monte-Carlo graph search (Czech et al. 2020; KataGo's formulation): the real
  rules detect transpositions so each position is evaluated once
- KataGo (Wu 2019): playout cap randomization, final ownership auxiliary target
