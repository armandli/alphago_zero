#ifndef RLGAMES_JSON_IO
#define RLGAMES_JSON_IO

#include <charconv>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <functional>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>
#include <algorithm>

#include <simdjson.h>

#include <type_alias.h>
#include <pytorch_util.h>

namespace rlgames {

namespace s  = std;
namespace sj = simdjson;
namespace od = simdjson::ondemand;

// Thrown for unreadable, malformed or schema-violating JSON, and for failures
// writing JSON files. The message names the file and the offending element.
struct JsonError : s::runtime_error {
  using s::runtime_error::runtime_error;
};

namespace json_detail {

inline void check(sj::error_code e, const s::string& what) {
  if (e) throw JsonError(what + ": " + sj::error_message(e));
}

// {"i": <uint>, "j": <uint>, "k": <uint>}; "i" is required, "j" and "k"
// default to 0. Unknown keys and non-integer / negative values are rejected.
inline TensorDim parse_tensordim(od::value value, const s::string& ctx) {
  od::object obj;
  check(value.get_object().get(obj), ctx + " must be an object");

  TensorDim dim(0U, 0U, 0U);
  bool has_i = false;
  for (auto field_result : obj) {
    od::field field;
    check(s::move(field_result).get(field), ctx);
    s::string_view key;
    check(field.unescaped_key().get(key), ctx + " has an invalid key");

    uint* target = nullptr;
    if      (key == "i") { target = &dim.i; has_i = true; }
    else if (key == "j") { target = &dim.j; }
    else if (key == "k") { target = &dim.k; }
    else throw JsonError(ctx + " has unknown key \"" + s::string(key) + "\"");

    s::string field_ctx = ctx + "." + s::string(key);
    uint64_t n = 0;
    check(field.value().get_uint64().get(n),
      field_ctx + " must be a non-negative integer");
    if (n > s::numeric_limits<uint>::max())
      throw JsonError(field_ctx + " is out of range");
    *target = static_cast<uint>(n);
  }
  if (not has_i) throw JsonError(ctx + " is missing \"i\"");
  return dim;
}

// float in its shortest round-trip form; JSON has no NaN/Infinity, so
// non-finite values are written as null
inline void append_float(sj::builder::string_builder& sb, float v) {
  if (not s::isfinite(v)) {
    sb.append_null();
    return;
  }
  char buf[32];
  auto [end, ec] = s::to_chars(buf, buf + sizeof(buf), v);
  if (ec != s::errc()) throw JsonError("unable to format float");
  sb.append_raw(s::string_view(buf, end - buf));
}

inline void append_key(sj::builder::string_builder& sb, s::string_view key) {
  sb.escape_and_append_with_quotes(key);
  sb.append_colon();
}

}  // namespace json_detail

// Parse a model configuration file: a JSON array of layer objects, each
// either {"tensordim": {...}} or {"conv_resnet_option": [{"tensordim": {...}}, ...]}.
// Returns the dimensions flattened in file order.
inline s::vector<TensorDim> load_tensor_dims(const s::string& filename) {
  using json_detail::check;
  using json_detail::parse_tensordim;

  // parser, input buffer and document must stay alive together
  sj::padded_string json;
  check(sj::padded_string::load(filename).get(json),
    "unable to read model config " + filename);
  od::parser parser;
  od::document doc;
  check(parser.iterate(json).get(doc), filename);

  od::array layers;
  check(doc.get_array().get(layers), filename + ": top level must be an array");

  s::vector<TensorDim> dims;
  size_t layer_idx = 0;
  for (auto layer_result : layers) {
    s::string layer_ctx = filename + ": layer[" + s::to_string(layer_idx) + "]";
    od::object layer;
    check(layer_result.get_object().get(layer), layer_ctx + " must be an object");

    size_t field_count = 0;
    for (auto field_result : layer) {
      od::field field;
      check(s::move(field_result).get(field), layer_ctx);
      s::string_view key;
      check(field.unescaped_key().get(key), layer_ctx + " has an invalid key");
      ++field_count;

      if (key == "tensordim") {
        dims.push_back(parse_tensordim(field.value(), layer_ctx + ".tensordim"));
      } else if (key == "conv_resnet_option") {
        s::string opt_ctx = layer_ctx + ".conv_resnet_option";
        od::array options;
        check(field.value().get_array().get(options), opt_ctx + " must be an array");
        size_t opt_idx = 0;
        for (auto option_result : options) {
          s::string entry_ctx = opt_ctx + "[" + s::to_string(opt_idx) + "]";
          od::object option;
          check(option_result.get_object().get(option),
            entry_ctx + " must be an object");
          size_t option_fields = 0;
          for (auto option_field_result : option) {
            od::field option_field;
            check(s::move(option_field_result).get(option_field), entry_ctx);
            s::string_view option_key;
            check(option_field.unescaped_key().get(option_key),
              entry_ctx + " has an invalid key");
            if (option_key != "tensordim")
              throw JsonError(entry_ctx + " has unknown key \""
                + s::string(option_key) + "\"");
            ++option_fields;
            dims.push_back(
              parse_tensordim(option_field.value(), entry_ctx + ".tensordim"));
          }
          if (option_fields != 1)
            throw JsonError(entry_ctx + " must contain exactly one \"tensordim\"");
          ++opt_idx;
        }
      } else {
        throw JsonError(layer_ctx + " has unknown key \"" + s::string(key) + "\"");
      }
    }
    if (field_count != 1)
      throw JsonError(layer_ctx
        + " must contain exactly one of \"tensordim\" or \"conv_resnet_option\"");
    ++layer_idx;
  }
  if (not doc.at_end())
    throw JsonError(filename + ": unexpected content after the top-level array");
  return dims;
}

template <typename Option>
Option load_model_option(const s::string& filename) {
  return Option(load_tensor_dims(filename));
}

// Serialize self-play training statistics.
inline s::string training_result_to_json(
    const s::vector<float>& losses,
    const s::vector<uint>& step_counts,
    uint64 a1win,
    uint64 a2win,
    uint64 ties)
{
  using json_detail::append_float;
  using json_detail::append_key;

  sj::builder::string_builder sb;
  sb.start_object();
  sb.append_key_value("player1_wins", a1win);
  sb.append_comma();
  sb.append_key_value("player2_wins", a2win);
  sb.append_comma();
  sb.append_key_value("ties", ties);
  sb.append_comma();

  append_key(sb, "losses");
  sb.start_array();
  for (size_t i = 0; i < losses.size(); ++i) {
    if (i > 0) sb.append_comma();
    append_float(sb, losses[i]);
  }
  sb.end_array();
  sb.append_comma();

  append_key(sb, "step_counts");
  sb.start_array();
  for (size_t i = 0; i < step_counts.size(); ++i) {
    if (i > 0) sb.append_comma();
    sb.append(step_counts[i]);
  }
  sb.end_array();
  sb.append_comma();

  // null when there were no games instead of 0/0
  append_key(sb, "average_steps");
  if (step_counts.empty()) {
    sb.append_null();
  } else {
    uint64 total = s::ranges::fold_left(step_counts, uint64{0}, s::plus<>{});
    append_float(sb, (float)total / (float)step_counts.size());
  }
  sb.end_object();

  s::string_view out;
  json_detail::check(sb.view().get(out), "unable to serialize training result");
  return s::string(out);
}

inline void save_training_result(
    const s::string& filename,
    const s::vector<float>& losses,
    const s::vector<uint>& step_counts,
    uint64 a1win,
    uint64 a2win,
    uint64 ties)
{
  s::string json = training_result_to_json(losses, step_counts, a1win, a2win, ties);
  s::ofstream ofs(filename, s::ios::binary | s::ios::trunc);
  if (not ofs) throw JsonError("unable to open " + filename + " for writing");
  ofs << json;
  ofs.flush();
  if (not ofs) throw JsonError("unable to write " + filename);
}

}  // namespace rlgames

#endif  // RLGAMES_JSON_IO
