// simdjson-based model config loading and training result output
// (replacement for the former RapidJSON implementation).

#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <simdjson.h>

#include <json_io.h>
#include <zero_model_small.h>
#include <zero_model_resnet_small.h>

namespace R  = rlgames;
namespace fs = std::filesystem;
namespace od = simdjson::ondemand;

namespace {

const std::string DATA = ALPHAGO_DATA_DIR;

struct TempDir {
  fs::path path;
  TempDir() {
    path = fs::temp_directory_path()
         / ("alphago_json_test_" + std::to_string(::testing::UnitTest::GetInstance()->random_seed())
            + "_" + ::testing::UnitTest::GetInstance()->current_test_info()->name());
    fs::create_directories(path);
  }
  ~TempDir() { fs::remove_all(path); }

  std::string write(const std::string& name, const std::string& content) const {
    fs::path p = path / name;
    std::ofstream(p, std::ios::binary) << content;
    return p.string();
  }
};

void expect_dim(const R::TensorDim& d, uint i, uint j, uint k) {
  EXPECT_EQ(d.i, i);
  EXPECT_EQ(d.j, j);
  EXPECT_EQ(d.k, k);
}

// expect JsonError whose message contains `fragment`
void expect_config_error(const std::string& json, const std::string& fragment) {
  TempDir dir;
  std::string file = dir.write("config.json", json);
  try {
    (void)R::load_tensor_dims(file);
    ADD_FAILURE() << "no error for: " << json;
  } catch (const R::JsonError& e) {
    EXPECT_NE(std::string(e.what()).find(fragment), std::string::npos)
      << "message '" << e.what() << "' lacks '" << fragment << "'";
  }
}

}  // namespace

// ---------------------------------------------------------------- loading

TEST(LoadTensorDims, SmallModelConfigFromRepo) {
  auto dims = R::load_tensor_dims(DATA + "/agents/zero_model_small_config.json");
  ASSERT_EQ(dims.size(), 6U);
  EXPECT_NO_THROW(R::ZeroModelSmallOptions{dims});
}

TEST(LoadTensorDims, ResnetModelConfigFromRepo) {
  auto dims = R::load_tensor_dims(DATA + "/agents/zero_model_resnet_small_config.json");
  ASSERT_EQ(dims.size(), 18U);
  expect_dim(dims[0], 21, 3, 3);
  expect_dim(dims[3], 168, 3, 3);
  expect_dim(dims[4], 256, 3, 3);
  // conv_resnet_option: channel-only entry, then two 3x3 convs
  expect_dim(dims[6], 21, 0, 0);
  expect_dim(dims[7], 21, 3, 3);
  expect_dim(dims[8], 21, 3, 3);
  expect_dim(dims[15], 168, 0, 0);

  auto opt = R::load_model_option<R::ZeroModelResnetSmallOptions>(
    DATA + "/agents/zero_model_resnet_small_config.json");
  EXPECT_EQ(opt.c1rsz.xcsz, 21U);
  EXPECT_EQ(opt.c4rsz.c2sz.i, 168U);
}

TEST(LoadTensorDims, KeyOrderAndWhitespaceDoNotMatter) {
  TempDir dir;
  auto dims = R::load_tensor_dims(dir.write("c.json", R"(
    [ { "tensordim" : { "k" : 5, "i" : 7, "j" : 6 } },
      {"conv_resnet_option":[{"tensordim":{"i":2}},{"tensordim":{"j":1,"i":3}}]} ]
  )"));
  ASSERT_EQ(dims.size(), 3U);
  expect_dim(dims[0], 7, 6, 5);
  expect_dim(dims[1], 2, 0, 0);
  expect_dim(dims[2], 3, 1, 0);
}

TEST(LoadTensorDims, EmptyArray) {
  TempDir dir;
  EXPECT_TRUE(R::load_tensor_dims(dir.write("c.json", "[]")).empty());
}

TEST(LoadTensorDims, MissingFile) {
  EXPECT_THROW(R::load_tensor_dims("/nonexistent/config.json"), R::JsonError);
}

TEST(LoadTensorDims, RejectsMalformedAndInvalidConfigs) {
  expect_config_error("", "config.json");
  expect_config_error(R"([{"tensordim":{"i":1}})", "config.json");          // truncated
  expect_config_error(R"([{"tensordim":{"i":3.14.1}}])", "tensordim");      // bad number
  expect_config_error(R"({"tensordim":{"i":1}})", "top level must be an array");
  expect_config_error(R"([5])", "layer[0] must be an object");
  expect_config_error(R"([{"tensordim":{"j":3}}])", "missing \"i\"");
  expect_config_error(R"([{"tensordim":{"i":-1}}])", "tensordim.i must be a non-negative integer");
  expect_config_error(R"([{"tensordim":{"i":2.5}}])", "tensordim.i must be a non-negative integer");
  expect_config_error(R"([{"tensordim":{"i":"8"}}])", "tensordim.i must be a non-negative integer");
  expect_config_error(R"([{"tensordim":{"i":4294967296}}])", "tensordim.i is out of range");
  expect_config_error(R"([{"tensordim":{"i":1,"x":2}}])", "unknown key \"x\"");
  expect_config_error(R"([{"tensordim":[1,2,3]}])", "tensordim must be an object");
  expect_config_error(R"([{"conv":{"i":1}}])", "unknown key \"conv\"");
  expect_config_error(R"([{}])", "exactly one of");
  expect_config_error(R"([{"tensordim":{"i":1},"tensordim":{"i":2}}])", "exactly one of");
  expect_config_error(R"([{"conv_resnet_option":{"i":1}}])", "must be an array");
  expect_config_error(R"([{"conv_resnet_option":[{"dim":{"i":1}}]}])",
                      "conv_resnet_option[0] has unknown key");
  expect_config_error(R"([{"conv_resnet_option":[{}]}])", "exactly one \"tensordim\"");
  expect_config_error(R"([{"tensordim":{"i":1}}] trailing)", "config.json");
  expect_config_error("[{\"tensordim\":{\"i\":1}}]\xff", "config.json");    // invalid UTF-8
}

TEST(LoadModelOption, WrongDimensionCountThrows) {
  TempDir dir;
  std::string file = dir.write("c.json", R"([{"tensordim":{"i":1,"j":3,"k":3}}])");
  EXPECT_THROW(R::load_model_option<R::ZeroModelSmallOptions>(file), std::invalid_argument);
  EXPECT_THROW(R::load_model_option<R::ZeroModelResnetSmallOptions>(file), std::invalid_argument);
}

// ----------------------------------------------------------------- output

TEST(TrainingResultJson, ExactOutput) {
  std::string json = R::training_result_to_json({0.5F, 0.25F}, {10, 20}, 3, 4, 1);
  EXPECT_EQ(json,
    R"({"player1_wins":3,"player2_wins":4,"ties":1,)"
    R"("losses":[0.5,0.25],"step_counts":[10,20],"average_steps":15})");
}

TEST(TrainingResultJson, RoundTripsThroughParser) {
  std::vector<float> losses = {1.2345678F, 0.1F, 3e-7F, 12345.678F};
  std::vector<uint> steps = {81, 120, 7};
  std::string json = R::training_result_to_json(losses, steps, 10, 20, 0);

  od::parser parser;
  simdjson::padded_string padded(json);
  od::document doc = parser.iterate(padded);
  EXPECT_EQ(uint64_t(doc["player1_wins"]), 10U);
  EXPECT_EQ(uint64_t(doc["player2_wins"]), 20U);
  EXPECT_EQ(uint64_t(doc["ties"]), 0U);
  size_t i = 0;
  for (double v : doc["losses"].get_array()) {
    ASSERT_LT(i, losses.size());
    EXPECT_EQ(float(v), losses[i]) << "loss " << i;  // shortest float repr is exact
    ++i;
  }
  EXPECT_EQ(i, losses.size());
  i = 0;
  for (uint64_t v : doc["step_counts"].get_array()) EXPECT_EQ(v, steps[i++]);
  EXPECT_EQ(i, steps.size());
  EXPECT_FLOAT_EQ(float(double(doc["average_steps"])), (81.F + 120.F + 7.F) / 3.F);
}

TEST(TrainingResultJson, NonFiniteLossesBecomeNull) {
  float inf = std::numeric_limits<float>::infinity();
  std::string json = R::training_result_to_json(
    {std::nanf(""), 1.F, inf, -inf}, {1}, 0, 0, 0);
  EXPECT_NE(json.find(R"("losses":[null,1,null,null])"), std::string::npos) << json;
  // and the output is valid JSON
  od::parser parser;
  simdjson::padded_string padded(json);
  od::document doc = parser.iterate(padded);
  size_t nulls = 0;
  for (auto v : doc["losses"].get_array()) nulls += v.is_null().value();
  EXPECT_EQ(nulls, 3U);
}

TEST(TrainingResultJson, NoGamesGivesNullAverage) {
  std::string json = R::training_result_to_json({}, {}, 0, 0, 0);
  EXPECT_EQ(json,
    R"({"player1_wins":0,"player2_wins":0,"ties":0,)"
    R"("losses":[],"step_counts":[],"average_steps":null})");
}

TEST(TrainingResultJson, LargeCounts) {
  uint64 big = std::numeric_limits<uint64>::max();
  std::string json = R::training_result_to_json({}, {}, big, 0, 0);
  EXPECT_NE(json.find("\"player1_wins\":18446744073709551615"), std::string::npos) << json;
}

TEST(SaveTrainingResult, WritesFile) {
  TempDir dir;
  std::string file = (dir.path / "result.json").string();
  R::save_training_result(file, {0.5F}, {2}, 1, 0, 0);
  std::ifstream in(file);
  std::string content((std::istreambuf_iterator<char>(in)), {});
  EXPECT_EQ(content, R::training_result_to_json({0.5F}, {2}, 1, 0, 0));
}

TEST(SaveTrainingResult, UnwritablePathThrows) {
  EXPECT_THROW(
    R::save_training_result("/nonexistent/dir/result.json", {}, {}, 0, 0, 0),
    R::JsonError);
}
