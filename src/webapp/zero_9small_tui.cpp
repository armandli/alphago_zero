#include <cassert>
#include <ctime>
#include <cstdlib>
#include <functional>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <print>
#include <cstdio>
#include <sstream>
#include <thread>
#include <array>
#include <vector>

#include <CLI/CLI.hpp>

#include <ftxui/component/app.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/dom/elements.hpp>

#include <type_alias.h>
#include <types.h>
#include <input_parser.h>
#include <go_types.h>
#include <splitmix.h>
#include <null_distribution.h>
#include <pytorch_util.h>
#include <go_action_encoder.h>
#include <go_zero_encoder.h>
#include <model_base.h>
#include <zero_model_small.h>
#include <zero_mcgs_agent.h>

#include <go_game.h>
#include <go_encoder.h>
#include <inference.h>
#include <mcgs.h>
#include <transformer_muzero.h>

#include <torch/torch.h>

namespace s = std;
namespace t = torch;
namespace R = rlgames;
namespace Z = rlgames::muzero;
namespace f = ftxui;

constexpr ubyte SZ = 9;
constexpr uint action_size = R::ZeroGoActionEncoder<SZ>::action_size();
using ModelContainerType = R::ModelContainer<
  R::ZeroModelSmall,
  R::ZeroGoStateEncoder<SZ>,
  R::ZeroGoActionEncoder<SZ>,
  t::optim::Adam>;
using AgentType = R::ZeroMCGSAgent<
  ModelContainerType,
  R::null_distribution<action_size>,
  R::Splitmix,
  R::GoBoard<SZ>,
  R::GoGameState<SZ>,
  action_size>;
using Game = Z::GoGame<SZ>;
using MuZeroEncoder = Z::GoEncoder<SZ>;

// AI move selection: returns an action index of Game
using Engine = s::function<int(const Game&)>;

// AlphaGo Zero small model searched by ZeroMCGSAgent
Engine make_zero_engine(
    const s::string& model_config_file,
    const s::string& model_file,
    const s::string& optimizer_file,
    t::Device device,
    int simulations)
{
  R::ZeroGoStateEncoder<SZ> state_encoder;
  R::ZeroGoActionEncoder<SZ> action_encoder;
  R::TensorDimP state_size = state_encoder.state_size();

  auto model_container = s::make_shared<ModelContainerType>(
    R::ZeroModelSmall(
      state_size,
      action_size,
      R::load_model_option<R::ZeroModelSmallOptions>(model_config_file)),
    s::move(state_encoder),
    s::move(action_encoder),
    1E-5F
  );
  R::load_model(*model_container, model_file, optimizer_file, device);
  model_container->model->to(device);

  auto agent = s::make_shared<AgentType>(
    *model_container,
    device,
    simulations,
    0.5F,   /*exploration factor; 0 would ignore the policy priors*/
    0.,     /*dirichlet distribution alpha (unused by null_distribution)*/
    0.,     /*noise factor*/
    rand()  /*random seed*/
  );
  // the agent refers to the model container, so keep both alive together
  return [model_container, agent](const Game& game) {
    return Game::to_action(agent->select_move(game.state()));
  };
}

// MuZero transformer model searched by Gumbel MCGS over the learned model
struct MuZeroEngine {
  MuZeroEncoder           encoder;
  Z::TransformerMuZero    model;
  Z::ModelInference       inference;
  Z::MCGS<Game, MuZeroEncoder> search;
  Z::InferenceRequests    req;
  Z::InferenceResults     res;
  int                     simulations;

  MuZeroEngine(const s::string& model_file, t::Device device, int simulations):
    model(Z::load_model(model_file, device)),
    inference(model->config, device,
      device.is_cuda() ? t::kBFloat16 : t::kFloat, simulations + 2),
    search(&encoder, Z::MCGSConfig(), 0, simulations + 2, s::random_device()()),
    simulations(simulations)
  {
    inference.set_weights(model, 0);
    req.observation_size = encoder.observation_size();
  }

  int operator()(const Game& game) {
    search.start(game, simulations, false);
    while (search.step(req)) {
      inference.infer(req, res);
      search.receive(res);
      req.clear();
    }
    return search.result(0.F).action;
  }
};

int main(int argc, char* argv[]) try {
  s::string model_config_file;
  s::string model_file;
  s::string optimizer_file;
  s::string muzero_model_file;
  s::optional<int> simulations;
  s::optional<float> komi;

  CLI::App cli{"AlphaGo Zero / MuZero 9x9 — terminal UI"};
  auto* zero_group = cli.add_option_group("AlphaGo Zero", "AlphaGo Zero small model");
  auto* config_opt = zero_group->add_option("-c,--model-config",
    model_config_file,
    "Model configuration file")->check(CLI::ExistingFile);
  auto* model_opt = zero_group->add_option("-m,--model",
    model_file,
    "Model checkpoint file")->check(CLI::ExistingFile);
  auto* optimizer_opt = zero_group->add_option("-o,--optimizer",
    optimizer_file,
    "Optimizer checkpoint file")->check(CLI::ExistingFile);
  config_opt->needs(model_opt, optimizer_opt);
  model_opt->needs(config_opt, optimizer_opt);
  optimizer_opt->needs(config_opt, model_opt);

  auto* muzero_group = cli.add_option_group("MuZero", "MuZero transformer model");
  auto* muzero_opt = muzero_group->add_option("-u,--muzero-model",
    muzero_model_file,
    "MuZero model file, e.g. data/muzero_9x9/final_model.pt")->check(CLI::ExistingFile);
  muzero_opt->excludes(config_opt, model_opt, optimizer_opt);

  cli.add_option("-s,--simulations", simulations,
    "Search simulations per move (default: 1200 AlphaGo Zero, 128 MuZero)")
    ->check(CLI::PositiveNumber);
  cli.add_option("-k,--komi", komi,
    "Komi (default: 1.5 AlphaGo Zero, 7.5 MuZero as it was trained with)");
  CLI11_PARSE(cli, argc, argv);

  bool use_muzero = not muzero_model_file.empty();
  if (not use_muzero and model_file.empty()) {
    s::println(stderr, "error: give either -c, -m and -o (AlphaGo Zero) or -u (MuZero)\n");
    s::print(stderr, "{}", cli.help());
    return 1;
  }

  srand(time(nullptr));

  t::Device device(t::kCPU);
  if (t::cuda::is_available())
    device = t::Device(t::kCUDA);

  Engine engine;
  if (use_muzero)
    engine = [e = s::make_shared<MuZeroEngine>(
                muzero_model_file, device, simulations.value_or(128))](const Game& game) {
      return (*e)(game);
    };
  else
    engine = make_zero_engine(model_config_file, model_file, optimizer_file,
      device, simulations.value_or(1200));

  const float game_komi = komi.value_or(use_muzero ? 7.5F : R::default_komi<SZ>());
  const s::string engine_name = use_muzero ? "MuZero" : "AlphaGo Zero";
  const s::string start_status =
    "You play Black.  Click a point (or type e.g. E5) to play.";

  Game game(game_komi);
  bool human_resigned = false;
  bool ai_thinking = false;
  int  generation = 0;      // bumped on reset so a stale AI reply is dropped
  int  hover = -1;          // point under the mouse, or -1
  s::string move_input;
  s::string status = start_status;
  s::array<f::Box, Game::NUM_POINTS> cell_boxes;

  auto screen = f::App::TerminalOutput();
  auto quit   = screen.ExitLoopClosure();
  // declared after screen so it is joined before screen is destroyed
  s::jthread ai_worker;

  auto game_over = [&]() { return human_resigned or game.is_terminal(); };

  auto column_label = [](int c) {
    char ch = static_cast<char>('A' + c);
    if (ch >= 'I') ++ch;
    return ch;
  };

  auto action_name = [](int action) {
    s::ostringstream ss;
    ss << Game::to_move(action);
    return ss.str();
  };

  auto end_status = [&]() {
    float margin = game.score_margin();
    return s::format("{} won by {:.1f}!",
      margin > 0 ? "Black (you)" : "White (AI)", margin > 0 ? margin : -margin);
  };

  // AI search runs on a worker thread; its reply is applied on the UI thread
  auto start_ai = [&]() {
    ai_thinking = true;
    status = "AI is thinking...";
    if (ai_worker.joinable()) ai_worker.join();
    ai_worker = s::jthread([&, snapshot = game, gen = generation]() {
      int ai_action = engine(snapshot);
      screen.Post([&, ai_action, gen]() {
        ai_thinking = false;
        if (gen != generation) return;
        game.apply(ai_action);
        s::string played = "AI played " + action_name(ai_action) + ". ";
        status = played + (game.is_terminal() ? end_status() : "Your turn.");
      });
    });
  };

  auto play_action = [&](int action) {
    if (ai_thinking) {
      status = "Wait — the AI is still thinking.";
      return;
    }
    if (game_over()) {
      status = "Game over. Press Reset to start a new game.";
      return;
    }
    if (not game.is_legal(action)) {
      status = "Illegal move on this board. Try again.";
      return;
    }
    game.apply(action);
    if (game.is_terminal()) status = end_status();
    else                    start_ai();
  };

  auto resign = [&]() {
    if (ai_thinking or game_over()) return;
    human_resigned = true;
    status = "You resigned. White (AI) won!";
  };

  auto on_text_move = [&]() {
    auto opt_move = R::string_to_move(move_input, SZ);
    move_input.clear();
    if (not opt_move) {
      status = "Invalid format — use column letter + row number (e.g. E5), pass or resign.";
      return;
    }
    if (opt_move->mty == R::M::Resign) resign();
    else                               play_action(Game::to_action(*opt_move));
  };

  auto on_reset = [&]() {
    game = Game(game_komi);
    human_resigned = false;
    ++generation;
    move_input.clear();
    status = ai_thinking ? "Reset. Waiting for the AI to finish its search..." : start_status;
  };

  auto render_board = [&]() -> f::Element {
    auto labels = [&]() {
      s::vector<f::Element> row{f::text("   ")};
      for (int c = 0; c < SZ; ++c)
        row.push_back(f::text(s::format(" {} ", column_label(c))) | f::dim);
      return f::hbox(s::move(row));
    };
    int last = game.last_action();
    s::vector<f::Element> rows{labels()};
    for (int r = SZ - 1; r >= 0; --r) {
      s::vector<f::Element> row{f::text(s::format("{:2} ", r + 1)) | f::dim};
      for (int c = 0; c < SZ; ++c) {
        int a = r * SZ + c;
        R::Player p = game.board().get(R::Pt(r, c));
        f::Element cell =
          p == R::Player::Black ? f::text(" X ") | f::bold | f::color(f::Color::RedLight) :
          p == R::Player::White ? f::text(" O ") | f::bold | f::color(f::Color::CyanLight) :
                                  f::text(" . ") | f::dim;
        if (a == last) cell = cell | f::underlined;
        if (a == hover and not ai_thinking and not game_over() and game.is_legal(a))
          cell = cell | f::inverted;
        row.push_back(cell | f::reflect(cell_boxes[a]));
      }
      row.push_back(f::text(s::format(" {}", r + 1)) | f::dim);
      rows.push_back(f::hbox(s::move(row)));
    }
    rows.push_back(labels());
    return f::vbox(s::move(rows));
  };

  auto cell_at = [&](int x, int y) {
    for (int a = 0; a < Game::NUM_POINTS; ++a)
      if (cell_boxes[a].Contain(x, y)) return a;
    return -1;
  };

  auto input      = f::Input(&move_input, "click the board, or type e.g. E5 / pass / resign");
  auto pass_btn   = f::Button("Pass",   [&] { play_action(Game::PASS); });
  auto resign_btn = f::Button("Resign", resign);
  auto reset_btn  = f::Button("Reset",  on_reset);
  auto quit_btn   = f::Button("Quit",   quit);

  input |= f::CatchEvent([&](f::Event e) {
    if (e == f::Event::Return) { on_text_move(); return true; }
    return false;
  });

  auto layout = f::Container::Vertical({
    input,
    f::Container::Horizontal({pass_btn, resign_btn, reset_btn, quit_btn}),
  });

  auto renderer = f::Renderer(layout, [&]() {
    return f::vbox({
      f::text(engine_name + " 9x9") | f::bold | f::center,
      f::separator(),
      render_board(),
      f::separator(),
      f::hbox({f::text(" Move: "), input->Render() | f::flex}),
      f::hbox({
        pass_btn->Render(),
        f::text(" "),
        resign_btn->Render(),
        f::text(" "),
        reset_btn->Render(),
        f::text(" "),
        quit_btn->Render(),
      }),
      f::text(" " + status),
    }) | f::border;
  });

  renderer |= f::CatchEvent([&](f::Event e) {
    if (not e.is_mouse()) return false;
    f::Mouse& m = e.mouse();
    int a = cell_at(m.x, m.y);
    hover = a;
    if (a >= 0 and m.button == f::Mouse::Left and m.motion == f::Mouse::Pressed) {
      play_action(a);
      return true;
    }
    return false;   // let buttons and the input see the mouse too
  });

  screen.Loop(renderer);
} catch (const s::exception& e) {
  // e.g. rlgames::JsonError for a bad model config
  s::println(stderr, "error: {}", e.what());
  return 1;
}
