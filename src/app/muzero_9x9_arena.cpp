// Play two MuZero 9x9 models against each other with the snapshot
// evaluator's match code (batched, colors alternating, random openings).

#include <cstdio>
#include <print>
#include <random>
#include <string>

#include <CLI/CLI.hpp>
#include <torch/torch.h>

#include <go_game.h>
#include <go_encoder.h>
#include <evaluation.h>
#include <inference.h>
#include <transformer_muzero.h>

namespace s = std;
namespace t = torch;
namespace Z = rlgames::muzero;

int main(int argc, char* argv[]) try {
  using Game = Z::GoGame<9>;
  using Enc  = Z::GoEncoder<9>;

  s::string file_a, file_b;
  int games = 128, simulations = 32, opening_moves = 4, max_moves = 162;
  float komi = 7.5F;
  uint64_t seed = 0;
  CLI::App app{"Match two MuZero 9x9 models"};
  app.add_option("model_a", file_a, "First model")->required()->check(CLI::ExistingFile);
  app.add_option("model_b", file_b, "Second model")->required()->check(CLI::ExistingFile);
  app.add_option("-g,--games", games, "Games (colors alternate)")->capture_default_str();
  app.add_option("-s,--simulations", simulations, "Search simulations per move")->capture_default_str();
  app.add_option("--opening-moves", opening_moves, "Random opening moves")->capture_default_str();
  app.add_option("--komi", komi, "Komi")->capture_default_str();
  app.add_option("--max-moves", max_moves, "Move cap")->capture_default_str();
  app.add_option("--seed", seed, "Random seed, 0 for random")->capture_default_str();
  CLI11_PARSE(app, argc, argv);

  t::Device device = t::cuda::is_available() ? t::Device(t::kCUDA) : t::Device(t::kCPU);
  t::ScalarType dtype = device.is_cuda() ? t::kBFloat16 : t::kFloat;
  int64_t capacity = static_cast<int64_t>(games) * (simulations + 2);

  Z::TransformerMuZero model_a = Z::load_model(file_a, device);
  Z::TransformerMuZero model_b = Z::load_model(file_b, device);
  Z::ModelInference inf_a(model_a->config, device, dtype, capacity);
  Z::ModelInference inf_b(model_b->config, device, dtype, capacity);
  inf_a.set_weights(model_a, 0);
  inf_b.set_weights(model_b, 0);

  Z::NetworkPlayer<Game> a(file_a, &inf_a, simulations);
  Z::NetworkPlayer<Game> b(file_b, &inf_b, simulations);
  Enc encoder;
  s::mt19937_64 rng(seed ? seed : s::random_device()());
  Z::MatchResult r = Z::play_match<Game, Enc>(a, b, &encoder, games, opening_moves, komi, max_moves, rng);

  double score = r.score();
  double elo = (score <= 0. or score >= 1.) ? (score <= 0. ? -999. : 999.)
                                            : -400. * s::log10(1. / score - 1.);
  s::println("{} vs {}: {}-{}-{} (score {:.3f}, elo diff {:+.0f})",
             file_a, file_b, r.wins, r.draws, r.losses, score, elo);
  return 0;
} catch (const s::exception& e) {
  s::println(stderr, "error: {}", e.what());
  return 1;
}
