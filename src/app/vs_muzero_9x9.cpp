// Play 9x9 Go against a trained MuZero model.

#include <cstdio>
#include <iostream>
#include <print>
#include <random>
#include <string>

#include <CLI/CLI.hpp>
#include <torch/torch.h>

#include <types.h>
#include <input_parser.h>
#include <go_game.h>
#include <go_encoder.h>
#include <inference.h>
#include <mcgs.h>
#include <transformer_muzero.h>

namespace s = std;
namespace t = torch;
namespace R = rlgames;
namespace Z = rlgames::muzero;

int main(int argc, char* argv[]) try {
  using Game = Z::GoGame<9>;
  using Enc  = Z::GoEncoder<9>;

  s::string model_file;
  int simulations = 128;
  s::string color = "black";
  float komi = 7.5F;
  CLI::App app{"Play 9x9 Go against a MuZero model"};
  app.add_option("-m,--model", model_file, "Model file, e.g. data/muzero_9x9/final_model.pt")
    ->required()->check(CLI::ExistingFile);
  app.add_option("--simulations", simulations, "Search simulations per move")->capture_default_str();
  app.add_option("--color", color, "Your color")->check(CLI::IsMember({"black", "white"}))->capture_default_str();
  app.add_option("--komi", komi, "Komi")->capture_default_str();
  CLI11_PARSE(app, argc, argv);

  t::Device device = t::cuda::is_available() ? t::Device(t::kCUDA) : t::Device(t::kCPU);
  Z::TransformerMuZero model = Z::load_model(model_file, device);
  Z::ModelInference inference(model->config, device,
    device.is_cuda() ? t::kBFloat16 : t::kFloat, simulations + 2);
  inference.set_weights(model, 0);

  Enc encoder;
  Z::MCGS<Game, Enc> search(&encoder, Z::MCGSConfig(), 0, simulations + 2, s::random_device()());
  Z::InferenceRequests req;
  req.observation_size = encoder.observation_size();
  Z::InferenceResults res;

  Game game(komi);
  int human = (color == "black") ? 0 : 1;
  s::println("You play {}. Enter a move (e.g. E5), pass or resign.", color);
  while (not game.is_terminal()) {
    s::cout << game.board() << s::endl;
    int action;
    if (game.to_play() == human) {
      auto move = R::parse_move(game.state(), 9);
      if (not move) return 0;
      if (move->mty == R::M::Resign) {
        s::println("You resign.");
        return 0;
      }
      action = Game::to_action(*move);
    } else {
      search.start(game, simulations, false);
      while (search.step(req)) {
        inference.infer(req, res);
        search.receive(res);
        req.clear();
      }
      Z::SearchResult r = search.result(0.F);
      action = r.action;
      s::cout << "MuZero plays " << Game::to_move(action)
              << s::format(" (value {:+.2f})", r.root_value) << s::endl;
    }
    game.apply(action);
  }
  s::cout << game.board() << s::endl;
  float margin = game.score_margin();
  s::println("{} wins by {:.1f}", margin > 0 ? "Black" : "White", margin > 0 ? margin : -margin);
  return 0;
} catch (const s::exception& e) {
  s::println(stderr, "error: {}", e.what());
  return 1;
}
