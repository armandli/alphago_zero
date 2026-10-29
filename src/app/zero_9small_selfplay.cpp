#include <cassert>
#include <ctime>
#include <cstdlib>
#include <string>
#include <random>
#include <chrono>
#include <vector>
#include <iostream>
#include <filesystem>
#include <algorithm>

#include <CLI/CLI.hpp>

#include <type_alias.h>
#include <types.h>
#include <go_types.h>
#include <splitmix.h>
#include <dirichlet_distribution.h>
#include <pytorch_util.h>
#include <zero_episodic_exp.h>
#include <go_action_encoder.h>
#include <go_zero_encoder.h>
#include <model_base.h>
#include <zero_model_small.h>
#include <zero_agent.h>

#include <torch/torch.h>

namespace s = std;
namespace c = s::chrono;
namespace t = torch;
namespace R = rlgames;

constexpr ubyte SZ = 9;

int main(int argc, char* argv[]){
  uint episodes = 1000;
  uint batchsize = 10;
  s::string model_config_file;
  s::string model_file;
  s::string optimizer_file;
  s::string result_file;

  CLI::App app{"AlphaGo Zero 9x9 self-play training"};
  app.add_option("-e,--episodes",    episodes,          "Number of training episodes")->default_val(1000);
  app.add_option("-b,--batchsize",   batchsize,         "Games per episode batch")->default_val(10);
  app.add_option("-c,--model-config",model_config_file, "Model configuration file")->required()->check(CLI::ExistingFile);
  app.add_option("-m,--model",       model_file,        "Model checkpoint file")->required();
  app.add_option("-o,--optimizer",   optimizer_file,    "Optimizer checkpoint file")->required();
  app.add_option("-r,--result",      result_file,       "Output result file")->required();
  CLI11_PARSE(app, argc, argv);

  srand(time(nullptr));
  uint max_bsize = batchsize * SZ * SZ * SZ;

  t::Device device(t::kCPU);
  if (t::cuda::is_available()){
    s::cout << "Using GPU" << s::endl;
    device = t::Device(t::kCUDA);
  }

  R::ZeroGoStateEncoder<SZ> state_encoder;
  R::ZeroGoActionEncoder<SZ> action_encoder;

  R::TensorDimP state_size = state_encoder.state_size();
  constexpr uint action_size = R::ZeroGoActionEncoder<SZ>::action_size();

  R::ModelContainer<R::ZeroModelSmall, R::ZeroGoStateEncoder<SZ>, R::ZeroGoActionEncoder<SZ>, t::optim::Adam> model_container(
    R::ZeroModelSmall(state_size, action_size, R::load_model_option<R::ZeroModelSmallOptions>(model_config_file)),
    s::move(state_encoder),
    s::move(action_encoder),
    1E-5F, //learning_rate
    1E-5F  //weight decay
  );

  if (s::filesystem::exists(model_file) && s::filesystem::exists(optimizer_file)){
    s::cout << "Using saved model and optimizer parameters" << s::endl;
    R::load_model(model_container, model_file, optimizer_file, device);
  }

  model_container.model->to(device);

  //the agents will share the same model, but use a different experience collector buffer
  R::ZeroAgent<decltype(model_container), R::dirichlet_distribution<action_size>, R::Splitmix, R::GoBoard<SZ>, R::GoGameState<SZ>, action_size> agent1(
    model_container,
    device,
    1600,   /*max expansion*/
    0.2,    /*exploration factor*/
    0.03,   /*dirichlet distribution alpha*/
    0.25,   /*noise factor*/
    rand()  /*random seed*/
  );

  R::ZeroAgent<decltype(model_container), R::dirichlet_distribution<action_size>, R::Splitmix, R::GoBoard<SZ>, R::GoGameState<SZ>, action_size> agent2(
    model_container,
    device,
    1600,   /*max expansion*/
    0.2,    /*exploration factor*/
    0.03,   /*dirichlet distribution alpha*/
    0.25,   /*noise factor*/
    rand()  /*random seed*/
  );

  s::vector<float> losses;
  s::vector<uint> step_counts;
  uint64 a1_wins = 0, a2_wins = 0, tie_count = 0;

  uint reporting_interval = s::max(episodes / 10U, 1U);

  for (uint i = 0; i < episodes; ++i){
    R::ZeroEpisodicExpCollector buffer1(max_bsize, state_size, action_size, device);
    R::ZeroEpisodicExpCollector buffer2(max_bsize, state_size, action_size, device);
    agent1.set_exp(buffer1);
    agent2.set_exp(buffer2);

    R::ZeroExperience experience(device);
    for (uint j = 0; j < batchsize; ++j){
      R::GoGameState<SZ> state;
      R::Player turn = R::Player::Black;

      auto gstart = c::high_resolution_clock::now();
      uint step_count = 0;

      while (not state.is_over()){
        R::Move move(R::M::Pass);
        switch (turn){
        case R::Player::Black: move = agent1.select_move(state); break;
        case R::Player::White: move = agent2.select_move(state); break;
        default: assert(false);
        }
        state.apply_move(move);
        step_count++;
        turn = R::other_player(turn);
      }

      step_counts.push_back(step_count);
      R::Player winner = state.winner();
      switch (winner){
      case R::Player::Black:
        buffer1.complete_episode(decltype(agent1)::MAX_SCORE);
        buffer2.complete_episode(decltype(agent2)::MIN_SCORE);
        a1_wins += 1;
        break;
      case R::Player::White:
        buffer1.complete_episode(decltype(agent1)::MIN_SCORE);
        buffer2.complete_episode(decltype(agent2)::MAX_SCORE);
        a2_wins += 1;
        break;
      case R::Player::Unknown:
        buffer1.complete_episode(decltype(agent1)::TIE_SCORE);
        buffer2.complete_episode(decltype(agent2)::TIE_SCORE);
        tie_count += 1;
        break;
      default: assert(false);
      }

      auto gstop = c::high_resolution_clock::now();
      auto duration = c::duration_cast<c::microseconds>(gstop - gstart);

      s::cout << "Game time: " << duration.count() << " microseconds" << s::endl;

      R::append_experiences(experience, buffer1, buffer2);
    }
    float loss = train(model_container, experience);

    if (i % reporting_interval){
      s::cout << "Episode " << i << ". Loss " << loss << s::endl;
    }

    losses.push_back(loss);
  }

  s::cout << "Self play complete. Saving training and result." << s::endl;

  R::save_model(model_container, model_file, optimizer_file);

  R::save_training_result(result_file, losses, step_counts, a1_wins, a2_wins, tie_count);
}
