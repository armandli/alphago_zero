---
name: simdjson-guide-cpp
description: Expert reference guide for the simdjson C++ library (SIMD-accelerated JSON parsing and serialization). Auto-activates when writing C++ code that encodes or decodes JSON strings, JSON files, or NDJSON/JSON-lines streams; when simdjson, simdjson::ondemand, padded_string, or string_builder appear in code; or when the user asks how to parse JSON fast in C++, On-Demand vs DOM, simdjson padding, lifetime and single-pass iteration rules, error codes, or serializing C++ structs to JSON. Covers correct usage patterns, the constraints that cause most simdjson bugs, CMake setup, and JSON features simdjson does not support (mutation, JSON5 comments, schema validation, bignums, canonical output) that need custom code. Do NOT use for JSON in other languages, or to explain existing nlohmann/json, RapidJSON, or Boost.JSON code with no simdjson angle.
---

# simdjson C++ Guide

simdjson is a SIMD-accelerated JSON library (C++11 minimum; best features need C++17/20). It parses
at gigabytes per second by **not** building a tree: the default On-Demand front-end is a *forward
iterator over the raw JSON text*.

Current release: **5.0.2**. Always pin a release tag, never `main`.

## Three Components

| Component | Namespace / header | Purpose |
|-----------|-------------------|---------|
| **On-Demand** (default) | `simdjson::ondemand` | Fast forward-only parsing. Use this for decoding. |
| **DOM** | `simdjson::dom` | Fully materialized tree. Use when you need random/repeated access. |
| **Builder** | `simdjson::builder` | JSON *generation* (`string_builder`, `to_json`). |

Only `#include "simdjson.h"`. No other header is supported.

## Before You Start: Is simdjson the Right Choice?

simdjson is a **parsing** library first. Check the task against this:

- **Decoding large/hot-path JSON** → simdjson On-Demand, ideal fit.
- **Project already uses nlohmann/json, RapidJSON or Boost.JSON** → do NOT silently migrate. simdjson
  has no DOM mutation and a stricter lifetime model, so a port is a real refactor. Mention the
  tradeoff and let the user decide.
- **Building/mutating small JSON payloads** → simdjson's builder works, but a mutable-DOM library is
  more ergonomic. simdjson wins only when serialization is measurably hot.
- **Need JSON5, comments, schema validation, or editing a document in place** → simdjson cannot do
  it. See [references/limitations.md](references/limitations.md).

## CMake Setup

```cmake
include(FetchContent)
FetchContent_Declare(simdjson
  GIT_REPOSITORY https://github.com/simdjson/simdjson.git
  GIT_TAG tags/v5.0.2
  GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(simdjson)

add_executable(my_app main.cpp)
target_link_libraries(my_app PRIVATE simdjson)
```

Requires CMake 3.15+. Alternative: `find_package(simdjson CONFIG REQUIRED)` for vcpkg/Conan/system
installs, or copy `singleheader/simdjson.h` + `simdjson.cpp` into the project.

Do **not** pass `-march=native` or `/arch:AVX2` unless the binary targets one known machine — simdjson
dispatches on CPU features at runtime.

## Build Flags That Matter

| Build | Flags | Why |
|-------|-------|-----|
| Debug | `SIMDJSON_DEVELOPMENT_CHECKS=1` (automatic in debug) | Catches `OUT_OF_ORDER_ITERATION` and lifetime misuse. **Always run new simdjson code in debug first.** |
| Release | `NDEBUG` defined | Without it, expensive internal checks stay on. CMake Release sets this. |

Never link debug-built simdjson against release code or vice versa.

## The Five Cardinal Rules

Most simdjson bugs are lifetime or single-pass violations, not syntax errors. These rules are the
core of the library's contract:

1. **Keep three things alive together.** The `parser`, the input buffer, and the `document` must all
   outlive every value read from them. A `string_view` from simdjson points into the input buffer or
   into the parser's internal string buffer — never into the value you assigned it to.

2. **One document per parser at a time; one `document` instance per document.** `document` is
   non-copyable. Pass it by reference. Need two documents at once? Use two parsers.

3. **Consume each value exactly once.** Casting a value to `double`, calling `get_string()`, or
   reading a key *advances the iterator*. Reading it twice is a bug, not a cache hit.

4. **Finish the current object/array before touching a sibling.** `content["bids"].get_array()`
   becomes invalid the moment you call `content["asks"]`. Grab a value and convert it immediately.

5. **Validation is lazy.** `iterate()` only indexes the document and checks UTF-8. Malformed JSON
   deeper in the document surfaces as an error *when you reach it*. Every access can fail.

Reuse one `parser` across all documents — it retains its buffers, so steady-state parsing does zero
allocation.

## Decoding: Canonical Pattern

```cpp
#include "simdjson.h"
using namespace simdjson;

ondemand::parser parser;                            // reuse across documents
padded_string json = padded_string::load("cars.json");
ondemand::document doc = parser.iterate(json);      // parser + json + doc stay in scope

for (ondemand::object car : doc["cars"]) {
  std::string_view make = car["make"];              // convert immediately
  int64_t year = car["year"];
  double total = 0;
  for (double pressure : car["tire_pressure"]) { total += pressure; }
  // `make` is only valid until the next document is parsed — copy it to retain:
  std::string owned_make(make);
}
```

Input must be padded by `SIMDJSON_PADDING` bytes. Pick the right input type:

| Source | Use |
|--------|-----|
| File | `padded_string::load("f.json")` |
| Literal in source | `R"({"a":1})"_padded` |
| Existing `std::string_view` / `std::string` (C++17+) | `simdjson::padded_input(sv)` — usually no copy |
| Non-const `std::string` | pass directly, or `simdjson::pad(s)` to silence sanitizers |
| Your own buffer with slack | `parser.iterate(ptr, len, capacity)` |
| Large file, zero-copy | `simdjson::padded_memory_map` (POSIX always; Windows opt-in) |

## Decoding: Error Handling Without Exceptions

Every fallible API returns `simdjson_result<T>`. `.get(out)` writes the value and returns an
`error_code` (`SUCCESS` is falsy):

```cpp
ondemand::parser parser;
padded_string json;
if (auto e = padded_string::load("cars.json").get(json)) {
  std::cerr << error_message(e); return 1;
}
ondemand::document doc;
if (auto e = parser.iterate(json).get(doc)) {
  std::cerr << error_message(e); return 1;
}
int64_t year;
if (auto e = doc["cars"].at(0)["year"].get(year)) {
  std::cerr << error_message(e); return 1;
}
```

Errors chain: `doc["a"]["b"]["c"].get(v)` reports one error for the whole path. Compact, but you lose
which step failed — split the chain when you need precise diagnostics.

**Recoverable:** `NO_SUCH_FIELD`, `INCORRECT_TYPE`, `NUMBER_OUT_OF_RANGE`. Handle and continue.

**Fatal:** `TAPE_ERROR`, `INCOMPLETE_ARRAY_OR_OBJECT`. Test with `simdjson::is_fatal(error)`. After
one, `doc.is_alive()` is false and **you must stop touching the document** — continuing is UB.

With exceptions (the default), the same code throws `simdjson_error`; catch it and read `e.error()`.
Build with `SIMDJSON_EXCEPTIONS=0` to disable entirely.

There is no `has_key()`. Probe instead:

```cpp
ondemand::value v;
if (!obj["optional"].get(v)) { /* present, and v holds it */ }
```

## Encoding: Canonical Pattern

Serialization is a separate, lower-level facility — you own the document structure.

```cpp
simdjson::builder::string_builder sb;   // reuse; optional ctor arg reserves bytes
sb.start_object();
sb.append_key_value("make", car.make);  // escapes the string for you
sb.append_comma();
sb.append_key_value("year", car.year);
sb.append_comma();
sb.append_key_value("tire_pressure", car.tire_pressure);  // std::vector, C++20
sb.end_object();

std::string_view out;
if (sb.view().get(out)) { /* error */ }
```

`sb.view()` is the recommended accessor (the `std::string`/`std::string_view` casts can throw). The
view borrows from `sb` — it dies with it. Call `sb.clear()` to reuse the buffer between documents.

**You** are responsible for commas, colons, and matching `start_`/`end_` calls — the builder does not
validate structure. Escaping is handled by `append_key_value` and
`escape_and_append_with_quotes`; `append_raw` deliberately does not escape.

Shortcuts, in increasing order of magic:

```cpp
std::string j = simdjson::to_json(std_container);   // C++20: vector/map/scalars work as-is
simdjson::to_json(obj, reused_string);              // reuse a std::string
sb << my_struct;                                    // C++26 static reflection, automatic
```

For custom types pre-C++26, write a `tag_invoke(serialize_tag, builder, const T&)` overload in
`namespace simdjson`. Details and the C++26 annotation set (`rename`, `skip`, `flatten`,
`rename_all`) are in [references/serialization.md](references/serialization.md).

simdjson does **not** escape invalid UTF-8 for you — call `sb.validate_unicode()` before `view()` if
your strings could be malformed.

## Capability Boundaries

simdjson will not do these. Each needs your own code — recipes are in
[references/limitations.md](references/limitations.md).

| Gap | What to do instead |
|-----|-------------------|
| **Edit a parsed document in place** | No mutation API exists. Parse into your own structs, then re-serialize with the builder. |
| **Re-read or random-access values** | On-Demand is forward-only and single-consumption. Store what you need, or use the DOM front-end, or `doc.rewind()` (re-parses). |
| **JSON5 / JSONC**: comments, trailing commas, unquoted keys, single quotes | Strict RFC 8259 only. Strip/convert before parsing. |
| **`NaN` / `Infinity` literals** | Rejected by default; opt in with `SIMDJSON_ENABLE_NAN_INF=1` (non-standard JSON). |
| **Integers beyond 64-bit, exact decimals** | `BIGINT_ERROR` / precision loss. Read the raw token via `raw_json_token()` and parse it yourself. |
| **Reject duplicate keys** | simdjson reports all duplicates without complaint. Dedupe while iterating. |
| **JSON Schema validation, JSON Patch/Merge Patch** | Not provided at all. Use another library or hand-roll. |
| **Canonical / sorted-key output (JCS)** | Builder emits in call order. Sort keys yourself before appending. |
| **Pretty-print a *parsed* document** | `to_json_string` yields raw minified slices. Fractured JSON works on your C++ types, not a parsed doc. Walk the tree yourself. |
| **Incremental parsing from a socket** | Needs the whole document in one padded buffer. Buffer it fully, or use `iterate_many` for streams of *complete* documents. |
| **Documents over 4 GiB** | Hard single-document limit. Split, or stream with `iterate_many`. |
| **UTF-16 / UTF-32** | UTF-8 only. Transcode with [simdutf](https://github.com/simdutf/simdutf). |
| **Match escaped keys** | `obj["date"]` will not match `"date"`. Iterate and compare `field.unescaped_key()`. |

Also mind the depth limit: parsing is iterative and costs no stack, but **your** recursive traversal
does. A few KB of `[[[[[` can blow the stack. Cap it:

```cpp
parser.allocate(0, 30);   // reject documents deeper than 30 (default 1024)
```

...and/or check `element.current_depth()` inside recursive walkers.

## Thread Safety

Single-threaded by design. Use **one parser per thread**; a `document` is an iterator and is not
thread-safe. Only `iterate_many` / `parse_many` spin up internal threads. CPU detection is
thread-safe. Threads outliving `main` can race teardown of simdjson's global dispatch state —
consider `std::quick_exit` if that applies.

## Verifying simdjson Code

Do not report simdjson work as done without running it — lifetime bugs are silent in release builds
and often produce plausible-looking wrong output.

1. **Build and run in Debug first** so `SIMDJSON_DEVELOPMENT_CHECKS` is active. An
   `OUT_OF_ORDER_ITERATION` error means rule 3 or 4 was violated; fix it rather than calling
   `reset()` to paper over it.
2. **Assert on parsed values**, not just on the absence of errors — a forward-only iterator misused
   can yield values from the wrong field without erroring.
3. **Test the failure paths** with genuinely malformed input (`{"n":3.14.1}`, a truncated document,
   invalid UTF-8) and confirm the code neither crashes nor touches a document after a fatal error.
4. **Check retained strings outlive their source.** If a `std::string_view` is stored past the
   parse, that is a bug — it must be copied into a `std::string`.
5. Run under ASan/UBSan where possible. Expect benign "read beyond buffer" reports from
   `padded_input` and `std::string` inputs; silence them with `simdjson::pad()` rather than
   suppressing the sanitizer.

## References

Load on demand:

- **Full parsing API** — accessors, iteration, field lookup, number types, JSON Pointer, JSONPath, key
  selectors, NDJSON via `iterate_many`, rewinding, raw JSON access, DOM front-end, custom type
  deserialization: [references/parsing-api.md](references/parsing-api.md)
- **Serialization** — `string_builder` full API, `to_json`, `tag_invoke`, C++26 reflection
  annotations, fractured (pretty) JSON: [references/serialization.md](references/serialization.md)
- **Limitations and custom implementations** — every gap above with working workaround code:
  [references/limitations.md](references/limitations.md)
- **Complete compilable examples** — file parsing, streaming, struct round-trip, tree walking,
  error-free style: [references/examples.md](references/examples.md)
