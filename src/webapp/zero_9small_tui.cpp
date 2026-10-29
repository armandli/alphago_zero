#include <cassert>
#include <ctime>
#include <cstdlib>
#include <string>
#include <sstream>
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
#include <zero_agent.h>

#include <torch/torch.h>

namespace s = std;
namespace t = torch;
namespace R = rlgames;
namespace f = ftxui;

constexpr ubyte SZ = 9;
constexpr uint action_size = R::ZeroGoActionEncoder<SZ>::action_size();
using ModelContainerType = R::ModelContainer<
  R::ZeroModelSmall,
  R::ZeroGoStateEncoder<SZ>,
  R::ZeroGoActionEncoder<SZ>,
  t::optim::Adam>;
using AgentType = R::ZeroAgent<
  ModelContainerType,
  R::null_distribution<action_size>,
  R::Splitmix,
  R::GoBoard<SZ>,
  R::GoGameState<SZ>,
  action_size>;

int main(int argc, char* argv[]) {
  s::string model_config_file;
  s::string model_file;
  s::string optimizer_file;

  CLI::App cli{"AlphaGo Zero 9x9 — terminal UI"};
  cli.add_option("-c,--model-config",
    model_config_file,
    "Model configuration file")->required()->check(CLI::ExistingFile);
  cli.add_option("-m,--model",
    model_file,
    "Model checkpoint file")->required()->check(CLI::ExistingFile);
  cli.add_option("-o,--optimizer",
    optimizer_file,
    "Optimizer checkpoint file")->required()->check(CLI::ExistingFile);
  CLI11_PARSE(cli, argc, argv);

  srand(time(nullptr));

  t::Device device(t::kCPU);
  if (t::cuda::is_available())
    device = t::Device(t::kCUDA);

  R::ZeroGoStateEncoder<SZ> state_encoder;
  R::ZeroGoActionEncoder<SZ> action_encoder;
  R::TensorDimP state_size = state_encoder.state_size();

  ModelContainerType model_container(
    R::ZeroModelSmall(
      state_size,
      action_size,
      R::load_model_option<R::ZeroModelSmallOptions>(model_config_file)),
    s::move(state_encoder),
    s::move(action_encoder),
    1E-5F
  );
  R::load_model(model_container, model_file, optimizer_file, device);
  model_container.model->to(device);

  AgentType agent(model_container, device, 1200, 0, 0., 0., rand());

  R::GoGameState<SZ> game_state;
  s::string move_input;
  s::string status =
    "You play Black.  Enter a move (e.g. E5) and press Play or Enter.";

  auto render_board = [&]() -> f::Element {
    s::ostringstream ss;
    game_state.board().print(ss);
    s::istringstream iss(ss.str());
    s::vector<f::Element> rows;
    s::string line;
    while (s::getline(iss, line))
      rows.push_back(f::text(line));
    return f::vbox(s::move(rows));
  };

  auto screen = f::App::TerminalOutput();
  auto quit   = screen.ExitLoopClosure();

  auto on_play = [&]() {
    if (game_state.is_over()) {
      status = "Game over. Press Reset to start a new game.";
      return;
    }
    auto opt_move = R::string_to_move(move_input, SZ);
    move_input.clear();
    if (not opt_move) {
      status = "Invalid format — use column letter + row number, e.g. E5.";
      return;
    }
    R::Move move = *opt_move;
    if (not game_state.is_valid_move(move)) {
      status = "Illegal move on this board. Try again.";
      return;
    }
    game_state.apply_move(move);
    if (not game_state.is_over()) {
      R::Move ai_move = agent.select_move(game_state);
      game_state.apply_move(ai_move);
    }
    if (game_state.is_over()) {
      R::Player winner = game_state.winner();
      if      (winner == R::Player::Black) status = "Black (you) won!";
      else if (winner == R::Player::White) status = "White (AI) won!";
      else                                 status = "Tie!";
    } else {
      status = "Your turn. Enter a move:";
    }
  };

  auto on_reset = [&]() {
    game_state = R::GoGameState<SZ>();
    move_input.clear();
    status =
      "You play Black.  Enter a move (e.g. E5) and press Play or Enter.";
  };

  auto input     = f::Input(&move_input, "e.g. E5");
  auto play_btn  = f::Button("Play",  on_play);
  auto reset_btn = f::Button("Reset", on_reset);
  auto quit_btn  = f::Button("Quit",  quit);

  input |= f::CatchEvent([&](f::Event e) {
    if (e == f::Event::Return) { on_play(); return true; }
    return false;
  });

  auto layout = f::Container::Vertical({
    input,
    f::Container::Horizontal({play_btn, reset_btn, quit_btn}),
  });

  auto renderer = f::Renderer(layout, [&]() {
    return f::vbox({
      f::text("AlphaGo Zero 9x9") | f::bold | f::center,
      f::separator(),
      render_board(),
      f::separator(),
      f::hbox({f::text(" Move: "), input->Render() | f::flex}),
      f::hbox({
        play_btn->Render(),
        f::text(" "),
        reset_btn->Render(),
        f::text(" "),
        quit_btn->Render(),
      }),
      f::text(" " + status),
    }) | f::border;
  });

  screen.Loop(renderer);
}
