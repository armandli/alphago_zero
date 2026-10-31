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
#include <print>

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
#include <zero_model_resnet_small.h>
#include <zero_agent.h>
#include <self_play.h>

#include <torch/torch.h>

namespace s = std;
namespace c = s::chrono;
namespace t = torch;
namespace R = rlgames;

constexpr ubyte SZ = 13;

int main(int argc, char* argv[]) try {
  uint episodes = 1000;
  uint batchsize = 10;
  s::string model_config_file;
  s::string model_file;
  s::string optimizer_file;
  s::string result_file;

  CLI::App app{"AlphaGo Zero 13x13 self-play training"};
  app.add_option("-e,--episodes",
    episodes,
    "Number of training episodes")->default_val(1000);
  app.add_option("-b,--batchsize",
    batchsize,
    "Games per episode batch")->default_val(10);
  app.add_option("-c,--model-config",
    model_config_file,
    "Model configuration file")->required()->check(CLI::ExistingFile);
  app.add_option("-m,--model",
    model_file,
    "Model checkpoint file")->required();
  app.add_option("-o,--optimizer",
    optimizer_file,
    "Optimizer checkpoint file")->required();
  app.add_option("-r,--result",
    result_file,
    "Output result file")->required();
  CLI11_PARSE(app, argc, argv);

  srand(time(nullptr));
  uint max_bsize = batchsize * SZ * SZ * SZ;

  t::Device device(t::kCPU);
  if (t::cuda::is_available()) {
    s::println("Using GPU");
    device = t::Device(t::kCUDA);
  }

  R::ZeroGoStateEncoder<SZ> state_encoder;
  R::ZeroGoActionEncoder<SZ> action_encoder;

  R::TensorDimP state_size = state_encoder.state_size();
  constexpr uint action_size = R::ZeroGoActionEncoder<SZ>::action_size();

  R::ModelContainer<
    R::ZeroModelResnetSmall,
    R::ZeroGoStateEncoder<SZ>,
    R::ZeroGoActionEncoder<SZ>,
    t::optim::Adam>
  model_container(
    R::ZeroModelResnetSmall(
      state_size,
      action_size,
      R::load_model_option<R::ZeroModelResnetSmallOptions>(
        model_config_file)),
    s::move(state_encoder),
    s::move(action_encoder),
    1E-5F,  // learning_rate
    1E-5F   // weight decay
  );

  if (s::filesystem::exists(model_file)
      and s::filesystem::exists(optimizer_file))
  {
    s::println("Using saved model and optimizer parameters");
    R::load_model(model_container, model_file, optimizer_file, device);
  }

  model_container.model->to(device);

  // the agents share the same model but use separate experience buffers
  R::ZeroAgent<
    decltype(model_container),
    R::dirichlet_distribution<action_size>,
    R::Splitmix,
    R::GoBoard<SZ>,
    R::GoGameState<SZ>,
    action_size>
  agent1(
    model_container,
    device,
    1600,   /*max expansion*/
    0.2,    /*exploration factor*/
    0.03,   /*dirichlet distribution alpha*/
    0.25,   /*noise factor*/
    rand()  /*random seed*/
  );

  R::ZeroAgent<
    decltype(model_container),
    R::dirichlet_distribution<action_size>,
    R::Splitmix,
    R::GoBoard<SZ>,
    R::GoGameState<SZ>,
    action_size>
  agent2(
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

  for (uint i = 0; i < episodes; ++i) {
    R::SelfPlayBatch batch = R::self_play_batch<SZ>(
      agent1, agent2, batchsize, max_bsize, state_size, action_size, device);

    for (sint64 us : batch.game_microseconds)
      s::println("Game time: {} microseconds", us);
    step_counts.insert(
      s::end(step_counts),
      s::begin(batch.step_counts),
      s::end(batch.step_counts));
    a1_wins   += batch.a1_wins;
    a2_wins   += batch.a2_wins;
    tie_count += batch.ties;

    float loss = train(model_container, batch.experience);

    if (i % reporting_interval == 0)
      s::println("Episode {}. Loss {}", i, loss);

    losses.push_back(loss);
  }

  s::println("Self play complete. Saving training and result.");

  R::save_model(model_container, model_file, optimizer_file);

  R::save_training_result(
    result_file, losses, step_counts, a1_wins, a2_wins, tie_count);
} catch (const s::exception& e) {
  // e.g. rlgames::JsonError for a bad model config
  s::println(stderr, "error: {}", e.what());
  return 1;
}
