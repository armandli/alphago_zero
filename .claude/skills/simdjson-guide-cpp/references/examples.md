# simdjson Complete Examples

Every program here was compiled with `g++ -std=c++20 -O2` against the **simdjson v5.0.2**
single-header amalgamation and run. They are copy-pasteable starting points, not fragments.

## Contents

1. [Build setup](#1-build-setup)
2. [Parse a file, extract fields (exception-free)](#2-parse-a-file-extract-fields-exception-free)
3. [Iterate an array of objects](#3-iterate-an-array-of-objects)
4. [Struct round-trip with `tag_invoke`](#4-struct-round-trip-with-tag_invoke)
5. [NDJSON streaming with per-line error recovery](#5-ndjson-streaming-with-per-line-error-recovery)
6. [Fast fixed-field extraction with a key selector](#6-fast-fixed-field-extraction-with-a-key-selector)
7. [Recursive tree walk and re-serialization](#7-recursive-tree-walk-and-re-serialization)
8. [Two passes over one document with `rewind`](#8-two-passes-over-one-document-with-rewind)
9. [Hardened parsing of untrusted input](#9-hardened-parsing-of-untrusted-input)
10. [Building JSON dynamically](#10-building-json-dynamically)

---

## 1. Build setup

### CMake with FetchContent (recommended)

```cmake
cmake_minimum_required(VERSION 3.20)
project(myapp CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)
FetchContent_Declare(
  simdjson
  GIT_REPOSITORY https://github.com/simdjson/simdjson.git
  GIT_TAG        tags/v5.0.2
  GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(simdjson)

add_executable(myapp main.cpp)
target_link_libraries(myapp PRIVATE simdjson)
```

Pin a tag. `GIT_TAG main` will silently change the API under you.

Do **not** add `-march=native`. simdjson selects its SIMD kernel at runtime; `-march=native`
produces a binary that crashes on older CPUs and buys nothing.

### Single header (no build system)

```bash
# Download simdjson.h and simdjson.cpp from the release's singleheader/ directory
g++ -std=c++20 -O2 -o myapp main.cpp simdjson.cpp
```

### Build flags that matter

| Flag | When |
|---|---|
| `-DSIMDJSON_DEVELOPMENT_CHECKS=1` | Debug builds. Asserts on out-of-order iteration — catches the #1 On-Demand bug class. |
| `-DNDEBUG` | Release builds. |
| `-DSIMDJSON_EXCEPTIONS=0` | You compile with `-fno-exceptions`. All results must then be unwrapped with `.get()`. |
| `-DSIMDJSON_ENABLE_NAN_INF=1` | Only if your producer emits non-standard `NaN`/`Infinity`. |

Never ship a release build with `SIMDJSON_DEVELOPMENT_CHECKS=1` — it is a large slowdown.

## 2. Parse a file, extract fields (exception-free)

```cpp
#include "simdjson.h"
#include <cstdio>
#include <string>

using namespace simdjson;

struct config {
  std::string host;
  uint64_t    port;
  bool        tls;
};

error_code load_config(const char* path, config& out) {
  // padded_string owns the bytes and provides SIMDJSON_PADDING slack.
  padded_string json;
  if (auto e = padded_string::load(path).get(json); e) { return e; }

  // One parser per thread, reused across calls in real code.
  ondemand::parser parser;
  ondemand::document doc;
  if (auto e = parser.iterate(json).get(doc); e) { return e; }

  std::string_view host;
  if (auto e = doc["host"].get_string().get(host); e) { return e; }
  out.host = host;              // COPY: the view points into parser scratch space

  if (auto e = doc["port"].get_uint64().get(out.port); e) { return e; }
  if (auto e = doc["tls"].get_bool().get(out.tls);  e) { return e; }

  return SUCCESS;
  // json and parser die here -- that is why out.host had to be a copy
}

int main(int argc, char** argv) {
  if (argc < 2) { return 2; }
  config c{};
  if (auto e = load_config(argv[1], c); e) {
    std::fprintf(stderr, "config error: %s\n", error_message(e));
    return 1;
  }
  std::printf("%s:%llu tls=%d\n", c.host.c_str(), (unsigned long long)c.port, (int)c.tls);
  return 0;
}
```

The whole point of this example is the lifetime: `padded_string` and `parser` are locals, so
`std::string_view host` cannot escape the function. Copying into `out.host` is mandatory, not
defensive.

## 3. Iterate an array of objects

```cpp
#include "simdjson.h"
#include <cstdio>
#include <string>
#include <vector>

using namespace simdjson;

struct tweet { uint64_t id; std::string text; std::string author; };

error_code parse_tweets(padded_string& json, std::vector<tweet>& out) {
  ondemand::parser parser;
  ondemand::document doc;
  if (auto e = parser.iterate(json).get(doc); e) { return e; }

  ondemand::array statuses;
  if (auto e = doc["statuses"].get_array().get(statuses); e) { return e; }

  for (auto element : statuses) {            // element is simdjson_result<value>
    ondemand::value v;
    if (auto e = element.get(v); e) { return e; }

    tweet t{};
    if (auto e = v["id"].get_uint64().get(t.id); e) { return e; }

    std::string_view sv;
    if (auto e = v["text"].get_string().get(sv); e) { return e; }
    t.text = sv;

    // Nested object: read it in one go, do not re-enter v["user"] twice.
    ondemand::object user;
    if (auto e = v["user"].get_object().get(user); e) { return e; }
    if (auto e = user["screen_name"].get_string().get(sv); e) { return e; }
    t.author = sv;

    out.push_back(std::move(t));
  }
  return SUCCESS;
}

int main() {
  auto json = R"({"statuses":[
    {"id":1,"text":"hello","user":{"screen_name":"ada"}},
    {"id":2,"text":"world","user":{"screen_name":"grace"}}
  ]})"_padded;

  std::vector<tweet> tweets;
  if (auto e = parse_tweets(json, tweets); e) {
    std::fprintf(stderr, "%s\n", error_message(e));
    return 1;
  }
  for (const auto& t : tweets) {
    std::printf("%llu @%s: %s\n", (unsigned long long)t.id, t.author.c_str(), t.text.c_str());
  }
  return 0;
}
```

Each field is read exactly once, in order. `v["id"]`, `v["text"]`, `v["user"]` happen to match the
document order, so each lookup is a short forward scan.

## 4. Struct round-trip with `tag_invoke`

Compiled and run as shown. Works on C++20 — no reflection needed.

```cpp
#include "simdjson.h"
#include <cstdio>
#include <optional>
#include <string>
#include <vector>

using namespace simdjson;

struct car {
  std::string make;
  std::string model;
  int64_t     year;
  std::vector<double> tire_pressure;
  std::optional<std::string> nickname;
};

// ---- Deserialization ------------------------------------------------------
// `auto&` lets this one overload serve value, object, array, document.
error_code tag_invoke(deserialize_tag, auto& val, car& out) noexcept {
  ondemand::object obj;
  if (auto e = val.get_object().get(obj); e) { return e; }

  std::string_view sv;
  if (auto e = obj["make"].get_string().get(sv); e) { return e; }
  out.make = sv;
  if (auto e = obj["model"].get_string().get(sv); e) { return e; }
  out.model = sv;
  if (auto e = obj["year"].get_int64().get(out.year); e) { return e; }

  ondemand::array arr;
  if (auto e = obj["tire_pressure"].get_array().get(arr); e) { return e; }
  out.tire_pressure.clear();
  for (auto elem : arr) {
    double d;
    if (auto e = elem.get_double().get(d); e) { return e; }
    out.tire_pressure.push_back(d);
  }
  return SUCCESS;
}

// ---- Serialization --------------------------------------------------------
void tag_invoke(serialize_tag, auto& b, const car& c) {
  b.start_object();
  b.append_key_value("make", c.make);
  b.append_comma();
  b.append_key_value("model", c.model);
  b.append_comma();
  b.append_key_value("year", c.year);
  b.append_comma();
  b.escape_and_append_with_quotes("tire_pressure");
  b.append_colon();
  b.append(c.tire_pressure);          // a range serializes as a JSON array
  if (c.nickname) {                    // emit optional fields only when present
    b.append_comma();
    b.append_key_value("nickname", *c.nickname);
  }
  b.end_object();
}

int main() {
  auto json = R"({"make":"Toyota","model":"Camry","year":2018,
                  "tire_pressure":[40.1,39.9,37.7,38.2]})"_padded;

  ondemand::parser parser;
  ondemand::document doc;
  if (auto e = parser.iterate(json).get(doc); e) { return 1; }

  car c;
  if (auto e = doc.get<car>().get(c); e) {          // generic get<T> finds tag_invoke
    std::fprintf(stderr, "%s\n", error_message(e));
    return 1;
  }
  std::printf("%s %s %lld (%zu tires)\n", c.make.c_str(), c.model.c_str(),
              (long long)c.year, c.tire_pressure.size());

  c.nickname = "zippy";
  std::string out;
  if (auto e = to_json(c, out); e) { return 1; }    // out-param form: reuses the buffer
  std::printf("%s\n", out.c_str());

  // Containers compose for free once tag_invoke exists:
  std::vector<car> fleet{c, c};
  std::string fleet_json;
  if (auto e = to_json(fleet, fleet_json); e) { return 1; }
  std::printf("%zu bytes of fleet JSON\n", fleet_json.size());
  return 0;
}
```

Verified output:

```
Toyota Camry 2018 (4 tires)
{"make":"Toyota","model":"Camry","year":2018,"tire_pressure":[40.1,39.9,37.7,38.2],"nickname":"zippy"}
```

Once `tag_invoke` exists, `std::vector<car>`, `std::optional<car>`, and
`std::map<std::string, car>` all work with no additional code.

## 5. NDJSON streaming with per-line error recovery

```cpp
#include "simdjson.h"
#include <cstdio>
#include <string>

using namespace simdjson;

int main() {
  // One JSON document per line. Line 3 is deliberately malformed.
  auto nd = R"({"user":"ada","bytes":120}
{"user":"grace","bytes":80}
{"user":"alan","bytes":}
{"user":"ada","bytes":300})"_padded;

  ondemand::parser parser;
  ondemand::document_stream stream;
  // newline_delimited is faster than the default when input really is one doc per line.
  if (auto e = parser.iterate_many(nd.data(), nd.size(), 1000000,
                                   stream_format::newline_delimited).get(stream); e) {
    std::fprintf(stderr, "stream init: %s\n", error_message(e));
    return 1;
  }

  uint64_t total = 0, ok = 0, bad = 0;
  for (auto doc_result : stream) {
    ondemand::document_reference doc;
    if (auto e = doc_result.get(doc); e) { ++bad; continue; }   // malformed line: skip it

    uint64_t bytes;
    if (auto e = doc["bytes"].get_uint64().get(bytes); e) { ++bad; continue; }
    total += bytes;
    ++ok;
  }
  std::printf("ok=%llu bad=%llu total=%llu truncated=%zu\n",
              (unsigned long long)ok, (unsigned long long)bad,
              (unsigned long long)total, stream.truncated_bytes());
  return 0;
}
```

Two things to copy from this:

- **`continue` on a per-document error.** `iterate_many` recovers at the next document boundary, so
  one bad line does not abort the stream.
- **`truncated_bytes()`** tells you how many trailing bytes form an incomplete final document. When
  reading a socket or a rotating log, carry those bytes forward into the next chunk instead of
  discarding them.

`batch_size` (1 MB here) must exceed your largest single document, or you get `CAPACITY`.

## 6. Fast fixed-field extraction with a key selector

When the field names are known at compile time, one pass beats *k* separate lookups — and it does
not care what order the producer wrote the keys in.

```cpp
#include "simdjson.h"
#include <cstdio>
#include <string>

using namespace simdjson;

int main() {
  // Note: keys are NOT in the order we ask for them.
  auto json = R"({"city":"Oslo","name":"ada","age":36})"_padded;

  ondemand::parser parser;
  ondemand::document doc;
  if (auto e = parser.iterate(json).get(doc); e) { return 1; }
  ondemand::object obj;
  if (auto e = doc.get_object().get(obj); e) { return 1; }

  std::string_view name, city;
  uint64_t age = 0;

  // Bind each key straight to a variable. One handler per key, in key order.
  auto r = obj.for_each<"name", "city", "age">(name, city, age);
  if (r.error) {
    std::fprintf(stderr, "%s\n", error_message(r.error));
    return 1;
  }
  std::printf("matched=%zu name=%.*s city=%.*s age=%llu\n",
              r.matched_count, (int)name.size(), name.data(),
              (int)city.size(), city.data(), (unsigned long long)age);
  return 0;
}
```

Verified output: `matched=3 name=ada city=Oslo age=36`

`for_each_result` has **public fields** `error` and `matched_count`, not accessors, and is
`[[nodiscard]]` — it is the only channel through which handler errors surface. Check
`matched_count` when a missing key is meaningful; a key simply absent from the JSON is not an error.

`for_each` consumes `obj`. Do not use that object instance afterward.

Mixing a lambda in for custom logic:

```cpp
uint64_t id = 0;
std::string author;
auto r2 = obj.for_each<"id", "user">(
  id,                                                   // assigned via value::get
  [&](ondemand::value v) -> error_code {                // custom: descend one level
    std::string_view sv;
    if (auto e = v["screen_name"].get_string().get(sv); e) { return e; }
    author = sv;
    return SUCCESS;
  });
```

Keep the key count small (a handful). The limit is 255, but the compile-time perfect hash degrades
badly well before that.

## 7. Recursive tree walk and re-serialization

For genuinely schema-less input. This walks any document and rebuilds minified JSON, using
`raw_json_token()` so scalars are reproduced exactly as written (no float reformatting).

```cpp
#include "simdjson.h"
#include <cstdio>
#include <string>

using namespace simdjson;

static constexpr int MAX_WALK_DEPTH = 64;

error_code walk(ondemand::value v, std::string& out, int depth) {
  if (depth > MAX_WALK_DEPTH) { return DEPTH_ERROR; }   // our stack, our limit

  ondemand::json_type t;
  if (auto e = v.type().get(t); e) { return e; }

  switch (t) {
  case ondemand::json_type::array: {
    ondemand::array arr;
    if (auto e = v.get_array().get(arr); e) { return e; }
    out += '[';
    bool first = true;
    for (auto child : arr) {
      ondemand::value cv;
      if (auto e = child.get(cv); e) { return e; }
      if (!first) { out += ','; }
      first = false;
      if (auto e = walk(cv, out, depth + 1); e) { return e; }
    }
    out += ']';
    return SUCCESS;
  }
  case ondemand::json_type::object: {
    ondemand::object obj;
    if (auto e = v.get_object().get(obj); e) { return e; }
    out += '{';
    bool first = true;
    for (auto field : obj) {
      std::string_view k;
      if (auto e = field.escaped_key().get(k); e) { return e; }  // keep original escaping
      if (!first) { out += ','; }
      first = false;
      out += '"'; out.append(k); out += "\":";
      if (auto e = walk(field.value(), out, depth + 1); e) { return e; }
    }
    out += '}';
    return SUCCESS;
  }
  default: {
    // On a value, raw_json_token() returns string_view directly (NOT a simdjson_result).
    std::string_view tok = v.raw_json_token();
    while (!tok.empty() && (tok.back() == ' '  || tok.back() == '\n' ||
                            tok.back() == '\t' || tok.back() == '\r')) {
      tok.remove_suffix(1);                 // the token includes trailing whitespace
    }
    out.append(tok);
    return SUCCESS;
  }
  }
}

int main() {
  auto json = R"({"x":[1,2,{"y":null,"z":true}],"w":"hi"})"_padded;

  ondemand::parser parser;
  ondemand::document doc;
  if (auto e = parser.iterate(json).get(doc); e) { return 1; }

  // Promote the document to a value so the recursion has a uniform entry point.
  ondemand::value root;
  if (auto e = doc.get_value().get(root); e) { return 1; }

  std::string out;
  if (auto e = walk(root, out, 0); e) {
    std::fprintf(stderr, "%s\n", error_message(e));
    return 1;
  }
  std::printf("%s\n", out.c_str());
  return 0;
}
```

Verified output: `{"x":[1,2,{"y":null,"z":true}],"w":"hi"}`

Three non-obvious details, all of which cost a debugging session if missed:

1. `value::raw_json_token()` returns `std::string_view` **directly**;
   `document::raw_json_token()` returns `simdjson_result<std::string_view>`.
2. The token **includes trailing whitespace** — trim it.
3. simdjson's `max_depth` limits *its* stage-1 tracking, not *your* recursion. The explicit
   `MAX_WALK_DEPTH` check is what stops a crafted `[[[[[...` from blowing your stack.

If you only need to pass a subtree through untouched, skip the walk:
`simdjson::to_json_string(v)` returns a `string_view` slice of the original text.

## 8. Two passes over one document with `rewind`

```cpp
#include "simdjson.h"
#include <cstdio>

using namespace simdjson;

int main() {
  auto json = R"({"items":[{"w":3},{"w":5},{"w":2}],"label":"batch-7"})"_padded;

  ondemand::parser parser;
  ondemand::document doc;
  if (auto e = parser.iterate(json).get(doc); e) { return 1; }

  // Pass 1: sum the weights.
  uint64_t total = 0;
  ondemand::array items;
  if (auto e = doc["items"].get_array().get(items); e) { return 1; }
  for (auto item : items) {
    uint64_t w;
    if (auto e = item["w"].get_uint64().get(w); e) { return 1; }
    total += w;
  }

  // rewind() invalidates every value/object/array/string_view obtained so far.
  doc.rewind();

  // Pass 2: read a field we already scanned past.
  std::string_view label;
  if (auto e = doc["label"].get_string().get(label); e) {
    std::fprintf(stderr, "%s\n", error_message(e));
    return 1;
  }
  std::printf("%.*s total=%llu\n", (int)label.size(), label.data(),
              (unsigned long long)total);
  return 0;
}
```

Verified output: `batch-7 total=10`

`rewind()` keeps the structural index built by stage 1, so a second pass costs roughly stage-2 time
only — much cheaper than re-parsing, and far cheaper than switching to DOM.

Use it when you need two genuinely independent passes. Do **not** use it inside a loop to fake
random access; that is O(n²). If you need true random access, use the DOM front-end.

## 9. Hardened parsing of untrusted input

Everything you should do differently when the JSON comes from the network.

```cpp
#include "simdjson.h"
#include <cstdio>
#include <string>

using namespace simdjson;

constexpr size_t MAX_BODY     = 8u << 20;   // 8 MB
constexpr size_t MAX_STR_LEN  = 4096;

struct request { std::string op; std::string payload; };

error_code parse_request(ondemand::parser& parser, std::string& body, request& out) {
  if (body.size() > MAX_BODY) { return CAPACITY; }       // reject before allocating

  ondemand::document doc;
  if (auto e = parser.iterate(pad(body)).get(doc); e) { return e; }

  std::string_view sv;

  if (auto e = doc["op"].get_string().get(sv); e) { return e; }
  if (sv.size() > MAX_STR_LEN) { return CAPACITY; }      // bound every string you keep
  out.op = sv;

  if (auto e = doc["payload"].get_string().get(sv); e) { return e; }
  if (sv.size() > MAX_STR_LEN) { return CAPACITY; }
  out.payload = sv;

  // On-Demand validates ONLY what you read. To detect trailing garbage you must first
  // consume the WHOLE document -- at_end() is meaningless before that. rewind() + raw_json()
  // is the cheap way: it walks the retained structural index, it does not re-parse.
  doc.rewind();
  std::string_view whole;
  if (auto e = doc.raw_json().get(whole); e) { return e; }
  if (!doc.at_end()) { return TRAILING_CONTENT; }

  return SUCCESS;
}

int main() {
  ondemand::parser parser;
  // Make oversized input fail with CAPACITY instead of allocating.
  parser.set_max_capacity(MAX_BODY);
  // NOTE: parser.allocate(cap, depth) does NOT reject deep documents in On-Demand --
  // max_depth is a no-op outside SIMDJSON_DEVELOPMENT_CHECKS. This code never recurses
  // (it reads two flat fields), so depth is not a risk here. If you add a recursive walk,
  // carry your own depth counter as in example 7, or switch to dom::parser, which does
  // return DEPTH_ERROR.

  std::string body = R"({"op":"ping","payload":"{}"})";
  request req;
  if (auto e = parse_request(parser, body, req); e) {
    std::fprintf(stderr, "reject: %s\n", error_message(e));
    return 1;
  }
  std::printf("op=%s payload=%s\n", req.op.c_str(), req.payload.c_str());
  return 0;
}
```

The five hardening measures, none of which simdjson does for you:

| Risk | Measure |
|---|---|
| Memory exhaustion from a huge body | Size check up front **and** `set_max_capacity()` |
| Stack blowup from deep nesting in a recursive walk | Your own depth counter (example 7), or `dom::parser` — **not** `allocate()`'s `max_depth` |
| Unbounded strings copied into your structs | Explicit `sv.size()` check before every copy |
| Trailing garbage silently ignored | Consume the whole document (`rewind()` + `raw_json()`), *then* `doc.at_end()` |
| Duplicate keys | simdjson accepts them; dedupe yourself if semantics depend on it |

See [limitations.md](limitations.md) for duplicate keys, big integers, and UTF-8 handling.

## 10. Building JSON dynamically

When the shape is decided at runtime, drive `string_builder` yourself.

```cpp
#include "simdjson.h"
#include <cstdio>
#include <optional>
#include <string>
#include <vector>

using namespace simdjson;

struct row { uint64_t id; std::string name; std::optional<double> score; };

error_code render(const std::vector<row>& rows, std::string& out) {
  builder::string_builder sb{4096};        // pre-size; reuse across calls with clear()

  sb.start_object();
  sb.append_key_value("count", rows.size());
  sb.append_comma();

  sb.escape_and_append_with_quotes("rows");
  sb.append_colon();
  sb.start_array();

  bool first = true;
  for (const auto& r : rows) {
    if (!first) { sb.append_comma(); }     // the `first` flag beats trimming a trailing comma
    first = false;

    sb.start_object();
    sb.append_key_value("id", r.id);
    sb.append_comma();
    sb.append_key_value("name", r.name);   // escaped for you
    if (r.score) {                          // conditional field: still comma-correct
      sb.append_comma();
      sb.append_key_value("score", *r.score);
    }
    sb.end_object();
  }
  sb.end_array();
  sb.end_object();

  // append* never reports failure. view() is the ONLY error channel.
  std::string_view view;
  if (auto e = sb.view().get(view); e) { return e; }
  if (!sb.validate_unicode()) { return STRING_ERROR; }   // inputs were untrusted
  out.assign(view);                                       // copy: view dies with sb
  return SUCCESS;
}

int main() {
  std::vector<row> rows{ {1, "ada", 9.5}, {2, "gra\"ce", std::nullopt} };
  std::string json;
  if (auto e = render(rows, json); e) {
    std::fprintf(stderr, "%s\n", error_message(e));
    return 1;
  }
  std::printf("%s\n", json.c_str());
  return 0;
}
```

Verified output (note `gra\"ce` escaped correctly):

```
{"count":2,"rows":[{"id":1,"name":"ada","score":9.5},{"id":2,"name":"gra\"ce"}]}
```

Four rules this example encodes:

1. **Check `sb.view()`.** Appends swallow allocation failure silently; the result is the only place
   it surfaces.
2. **`append_key_value` escapes string values; `append_raw` does not.** Never pass user data to
   `append_raw` — that is a JSON injection.
3. **`view()` borrows the builder's buffer.** Copy it out before `sb` dies or `clear()` runs.
4. **`validate_unicode()` is opt-in.** The builder does not check UTF-8 as it writes.

---

## See also

- [parsing-api.md](parsing-api.md) — complete decode API
- [serialization.md](serialization.md) — complete encode API, reflection, pretty-printing
- [limitations.md](limitations.md) — mutation, JSON5, big integers, schemas, and their workarounds
