# simdjson Limitations and Custom Implementations

What simdjson deliberately does not do, why, and what to write instead. simdjson is a
*parser and generator*, not a JSON document manipulation toolkit — most gaps below are design
choices in service of speed, not oversights.

## 1. No Document Mutation

**There is no API to change a parsed value and write the document back.** `dom::element` and
`ondemand::value` are read-only views; neither has a setter, `insert`, or `erase`.

Round-trip editing requires parse → own model → re-serialize:

```cpp
struct Config { std::string host; int64_t port; bool tls; };

Config load(std::string_view path) {
  simdjson::ondemand::parser parser;
  auto json = simdjson::padded_string::load(path);
  simdjson::ondemand::document doc = parser.iterate(json);
  return Config{
    std::string(std::string_view(doc["host"])),   // copy out of the parser buffer
    doc["port"].get_int64(),
    doc["tls"].get_bool()
  };
}

std::string save(const Config& c) {
  simdjson::builder::string_builder sb;
  sb.start_object();
  sb.append_key_value("host", c.host); sb.append_comma();
  sb.append_key_value("port", c.port); sb.append_comma();
  sb.append_key_value("tls",  c.tls);
  sb.end_object();
  return std::string(sb.view().value());
}
```

**Unknown fields are dropped** by this approach. To preserve them, capture each unrecognized value
as a raw slice and re-emit it verbatim:

```cpp
std::vector<std::pair<std::string, std::string>> passthrough;  // key -> raw JSON
for (auto field : doc.get_object()) {
  std::string_view key = field.unescaped_key();
  if (key == "host" || key == "port" || key == "tls") { /* typed above */ continue; }
  // .value() is required: to_json_string returns simdjson_result<string_view>, and
  // std::string has no constructor taking one. The copy is also required -- the slice
  // points into the input buffer.
  passthrough.emplace_back(std::string(key),
                           std::string(simdjson::to_json_string(field.value()).value()));
}
// later, when writing:
for (auto& [k, raw] : passthrough) {
  sb.append_comma();
  sb.escape_and_append_with_quotes(k);
  sb.append_colon();
  sb.append_raw(raw);      // already valid JSON — must NOT be escaped
}
```

Needing frequent in-place edits of arbitrary JSON is a signal that a mutable-DOM library
(nlohmann/json, Boost.JSON) fits the task better. Say so rather than contorting simdjson.

## 2. Forward-Only, Single-Consumption Access

On-Demand walks the text once. Re-reading a value is undefined, not merely slow.

```cpp
// WRONG — "price" consumed twice
double a = doc["price"];
double b = doc["price"];

// WRONG — the bids array dies when "asks" is requested
auto bids = doc["bids"].get_array();
auto asks = doc["asks"].get_array();
for (auto b : bids) { /* UB */ }

// RIGHT — consume fully, in order
double price = doc["price"];
std::vector<double> bids;
for (double v : doc["bids"]) { bids.push_back(v); }
std::vector<double> asks;
for (double v : doc["asks"]) { asks.push_back(v); }
```

Escape hatches, in order of preference:

1. **Store what you need on the first pass** (above). Almost always correct.
2. **`doc.rewind()`** — restart from the beginning without re-indexing. Invalidates every existing
   value, object, array, and unescaped string. Values are *re-parsed*, so a two-pass numeric
   workload pays twice; it suits "pass 1 counts structure, pass 2 reads data".
3. **`array.reset()` / `object.reset()`** — like `rewind()` but does **not** reset the parser's string
   buffer, so strings must still be unescaped only once. Never call it while iterating that same
   container.
4. **Switch to the DOM front-end** when the access pattern is genuinely random:

```cpp
simdjson::dom::parser parser;
simdjson::dom::element doc = parser.load("data.json");
// Random, repeated, any-order access is fine here:
double x = doc["a"]["b"];
double y = doc["a"]["b"];   // legal in DOM
```

DOM materializes the whole document (more memory, fully validated up front). It is still fast — the
On-Demand API is not a prerequisite for good performance.

## 3. No JSON5 / JSONC

simdjson is strictly RFC 8259. **Comments, trailing commas, unquoted keys, single-quoted strings,
hex literals, and leading `+` or `.` in numbers are all rejected.** There is no lenient mode.

Config files with comments must be preprocessed. A correct stripper must respect string literals and
escapes — a naive `//` search corrupts URLs:

```cpp
// Strips // and /* */ comments plus trailing commas, outside of string literals.
std::string strip_jsonc(std::string_view in) {
  std::string out;
  out.reserve(in.size());
  bool in_string = false;
  for (size_t i = 0; i < in.size(); ++i) {
    char c = in[i];
    if (in_string) {
      out += c;
      if (c == '\\' && i + 1 < in.size()) { out += in[++i]; }   // keep escape pair intact
      else if (c == '"') { in_string = false; }
      continue;
    }
    if (c == '"') { in_string = true; out += c; continue; }
    if (c == '/' && i + 1 < in.size()) {
      if (in[i + 1] == '/') { while (i < in.size() && in[i] != '\n') { ++i; } out += '\n'; continue; }
      if (in[i + 1] == '*') {
        i += 2;
        while (i + 1 < in.size() && !(in[i] == '*' && in[i + 1] == '/')) { ++i; }
        ++i;  // land on '/'
        continue;
      }
    }
    if (c == ',') {  // drop the comma if the next non-space char closes a container
      size_t j = i + 1;
      while (j < in.size() && std::isspace(static_cast<unsigned char>(in[j]))) { ++j; }
      if (j < in.size() && (in[j] == '}' || in[j] == ']')) { continue; }
    }
    out += c;
  }
  return out;
}

std::string cleaned = strip_jsonc(raw);
auto doc = parser.iterate(simdjson::pad(cleaned));
```

Prefer fixing the producer over shipping a stripper when you control both ends.

## 4. `NaN` and `Infinity`

Rejected by default — they are not valid JSON. Opt in at build time *and* before the include:

```cpp
#define SIMDJSON_ENABLE_NAN_INF 1
#include "simdjson.h"
```

plus `cmake -D SIMDJSON_ENABLE_NAN_INF=ON`. Then `Infinity`, `-Infinity`, `Inf`, `-Inf`, `NaN` parse
case-insensitively as `double`.

The builder never *emits* these — a non-finite `double` has no JSON representation. Decide on a
convention and enforce it yourself:

```cpp
void append_double(simdjson::builder::string_builder& sb, double v) {
  if (std::isfinite(v)) { sb.append(v); }
  else { sb.append_null(); }        // or append_raw("\"NaN\""), per your schema
}
```

## 5. Numbers: Big Integers and Exact Decimals

simdjson parses to `int64_t`, `uint64_t`, or `double` only.

- Integers outside 64-bit range → `BIGINT_ERROR` / `NUMBER_OUT_OF_RANGE`.
- There is **no decimal type**. `0.1` becomes the nearest `double`. Correctly rounded (ULP 0) and
  round-trip exact at 17 significant digits — but still binary floating point, so unsuitable for
  money.
- Values at or beyond ±1e308 are rejected outright.

Detect and fall back to the raw text:

```cpp
// get_number_type() returns simdjson_result<number_type> -- `auto type = ...` leaves it
// wrapped and `type == number_type::big_integer` will not compile. Unwrap first.
simdjson::ondemand::number_type type;
if (auto e = value.get_number_type().get(type); e) { return e; }

if (type == simdjson::ondemand::number_type::big_integer) {
  std::string_view token = value.raw_json_token();   // digits exactly as written
  // The token carries trailing whitespace -- trim before handing it to a parser.
  my_bignum = BigInt(token);                         // your bignum / decimal type
} else if (type == simdjson::ondemand::number_type::signed_integer) {
  if (auto e = value.get_int64().get(n); e) { return e; }
}
```

For currency, keep the raw token and parse to a fixed-point integer yourself, or require the producer
to send minor units (cents) as integers — the simplest fix.

`raw_json_token()` returns the token as it appears in the input; `raw_json()` returns the full raw
slice of any value, including whole objects and arrays.

## 6. Duplicate Keys Are Accepted

Per RFC 8259, simdjson does not enforce key uniqueness and reports **every** occurrence. `obj["k"]`
returns the first match. If duplicates are a security or correctness concern, detect them:

```cpp
std::unordered_set<std::string> seen;
for (auto field : doc.get_object()) {
  // .value() is required: iterating yields simdjson_result<field>, so unescaped_key()
  // returns simdjson_result<string_view>, which std::string cannot construct from.
  std::string key(field.unescaped_key().value());
  if (!seen.insert(key).second) { return error("duplicate key: " + key); }
  // ... consume field.value()
}
```

## 7. No Schema Validation, JSON Patch, or Merge Patch

Entirely out of scope. simdjson validates *syntax* (and UTF-8), never *shape*. For JSON Schema use a
dedicated library; for light validation, hand-roll against the On-Demand reader — it is cheap because
you are already walking the document:

```cpp
std::optional<std::string> validate_user(simdjson::ondemand::object obj) {
  std::string_view name;
  if (obj["name"].get_string().get(name)) { return "name missing or not a string"; }
  if (name.empty())                       { return "name must be non-empty"; }
  int64_t age;
  if (obj["age"].get_int64().get(age))    { return "age missing or not an integer"; }
  if (age < 0 || age > 150)               { return "age out of range"; }
  return std::nullopt;
}
```

Note the shape: validate as you consume, since you cannot go back for a second look.

## 8. No Canonical or Sorted-Key Output

The builder writes keys in the order you call it. There is no JCS / RFC 8785 canonicalization. For
stable output (hashing, signatures, golden-file tests), sort before emitting:

```cpp
std::map<std::string, std::string> fields;   // std::map keeps keys sorted
// ... fill with key -> already-serialized JSON value
sb.start_object();
bool first = true;
for (const auto& [k, v] : fields) {
  if (!first) { sb.append_comma(); }
  first = false;
  sb.escape_and_append_with_quotes(k);
  sb.append_colon();
  sb.append_raw(v);
}
sb.end_object();
```

True canonicalization also pins number formatting and string escaping, which simdjson does not
guarantee across versions. Do not rely on byte-identical output between simdjson releases for
signatures.

## 9. Pretty-Printing a Parsed Document

Three distinct facilities, often confused:

| Need | Tool |
|------|------|
| Minify a JSON string, no parsing | `simdjson::minify(in, len, out, out_len)` — very fast, does not validate |
| Re-emit a parsed value as compact JSON | `simdjson::to_json_string(value)` — returns a raw `string_view` slice, no allocation; **consumes** the value |
| Pretty-print your own C++ structs | `simdjson::to_fractured_json_string(obj, opts)` — C++26 reflection, operates on *your types* |

There is **no** pretty-printer for an arbitrary parsed document. Write a recursive walk:

```cpp
void pretty(simdjson::ondemand::value v, std::string& out, int indent = 0) {
  std::string pad(indent * 2, ' '), pad2((indent + 1) * 2, ' ');
  switch (v.type()) {
  case simdjson::ondemand::json_type::object: {
    out += "{\n";
    bool first = true;
    for (auto f : v.get_object()) {
      if (!first) { out += ",\n"; }
      first = false;
      out += pad2 + "\"" + std::string(f.escaped_key().value()) + "\": ";
      pretty(f.value(), out, indent + 1);
    }
    out += "\n" + pad + "}";
    break;
  }
  case simdjson::ondemand::json_type::array: {
    out += "[\n";
    bool first = true;
    for (auto e : v.get_array()) {
      if (!first) { out += ",\n"; }
      first = false;
      out += pad2;
      pretty(e.value(), out, indent + 1);
    }
    out += "\n" + pad + "]";
    break;
  }
  // Scalars: raw_json_token() gives the original text, avoiding reformatting.
  default: out += std::string(v.raw_json_token()); break;
  }
}
```

This version relies on `SIMDJSON_EXCEPTIONS` (the `switch (v.type())` and `get_object()` conversions
throw on error). In a no-exceptions build, return `error_code` from `pretty` and unwrap each result
with `.get(out)`.

Guard this against hostile depth — see §12.

## 10. No Incremental / Streaming Input

simdjson needs the **entire document** in one contiguous padded buffer. You cannot feed it bytes as
they arrive from a socket. There is no push parser and no SAX-style callback API over partial input.

- **One document arriving in chunks** → accumulate into a buffer, parse once it is complete. Reserve
  `SIMDJSON_PADDING` extra bytes so no copy is needed:

```cpp
std::string buf;
buf.reserve(expected + simdjson::SIMDJSON_PADDING);
while (recv_chunk(buf)) { }                      // append until complete
auto doc = parser.iterate(simdjson::pad(buf));
```

- **A stream of complete documents** (NDJSON, JSON lines, concatenated objects) → `iterate_many`,
  which *is* lazy and parses one document at a time:

```cpp
simdjson::ondemand::document_stream stream;
if (auto e = parser.iterate_many(json, 1000000).get(stream)) { /* handle */ }
for (auto doc : stream) {
  int64_t v;
  if (doc["foo"].get(v)) { continue; }
  // ...
}
```

`batch_size` (default 1 MB) must exceed your largest single document or you get `CAPACITY`; check
`stream.truncated_bytes()` for an unprocessed tail. The buffer passed to `iterate_many` must outlive
the stream — a temporary will not compile.

## 11. Size Limits

- **4 GiB per document.** Hard limit; larger input is refused. Streams via `iterate_many` have no
  total size cap (each document must still fit).
- **Unbounded parser growth.** The parser expands to fit the largest document seen and never shrinks.
  In a long-lived server, bound it:

```cpp
simdjson::ondemand::parser parser(1000 * 1000);   // never grow past 1 MB
// or pin it exactly:
parser.allocate(1000 * 1000);
// ...
if (error == simdjson::CAPACITY) { respond(413); }  // payload too large
```

## 12. Depth: Your Recursion, Not simdjson's

Parsing is iterative and uses no stack regardless of nesting. **Your** recursive traversal does, and a
stack overflow is a crash, not a catchable error — a few KB of `[[[[[[...` suffices.

**`parser.allocate(capacity, max_depth)` does not limit depth in On-Demand.** The header is explicit:

> The maximum depth of this parser [...] is only relevant when the macro
> `SIMDJSON_DEVELOPMENT_CHECKS` is set to true. The document's instance `current_depth()` method
> should be used to monitor the parsing depth and limit it if desired.

Measured on v5.0.2 — `ondemand::parser` with `allocate(4096, 5)` parsing `[[[...1...]]]`:

| Nesting | On-Demand (release) | On-Demand (`DEVELOPMENT_CHECKS=1`) | DOM |
|---|---|---|---|
| 3 | SUCCESS | SUCCESS | SUCCESS |
| 10 | **SUCCESS** | **SUCCESS** | `DEPTH_ERROR` |
| 40 | **SUCCESS** | **SUCCESS** | `DEPTH_ERROR` |

So there are exactly two working options:

**(a) Use the DOM front-end**, which really does enforce it in stage 1:

```cpp
simdjson::dom::parser parser;
if (auto e = parser.allocate(capacity, 30); e) { return e; }
simdjson::dom::element el;
if (auto e = parser.parse(json).get(el); e) { return e; }   // DEPTH_ERROR if deeper than 30
```

**(b) Check `current_depth()` yourself** inside every recursive walker — the only option if you want
On-Demand's speed:

```cpp
error_code walk(simdjson::ondemand::value v, int depth) {
  if (depth > 30) { return simdjson::DEPTH_ERROR; }   // your counter, your limit
  // ... recurse with depth + 1
}
```

`current_depth()` returns `int32_t` directly (not a `simdjson_result`) and is cheap, but note it
reports **1** at the root value regardless of what is nested below — it tells you where the cursor
is, not how deep the document goes. Passing your own `depth` parameter down is simpler and harder to
get wrong.

Apply this to any recursive function over untrusted JSON, including the pretty-printer in §9.

## 13. Encoding: UTF-8 Only

JSON is UTF-8 and so is simdjson, in and out. Documents with a byte-order mark are **rejected**.

- UTF-16/UTF-32 input or output → transcode with [simdutf](https://github.com/simdutf/simdutf).
- `std::u8string_view` accessors exist (`get_u8string()`, `unescaped_u8key()`) when
  `SIMDJSON_SUPPORTS_CHAR8_T` is set; they are zero-copy aliases, not conversions.
- Validate UTF-8 alone with `simdjson::validate_utf8(ptr, len)` — needs no padding, no allocation.
- **Lossless non-UTF-8 strings** (unpaired surrogates) cannot be represented in `std::string_view`.
  Either accept replacement characters (`get_string(true)`) or take WTF-8 via
  `get_wobbly_string()`.
- The builder does not validate what you append — call `sb.validate_unicode()` before `view()` when
  your strings may be malformed.

## 14. Key Lookup Does Not Unescape

`obj["date"]` compares bytes against the raw key as it appears in the document. A document that
writes that key in escaped form — `"\u0064ate"`, legal JSON, identical after unescaping — will
**not** match.
(Verified: `operator[]` returns `NO_SUCH_FIELD`.) This is rarely a problem in practice, but when
escaped keys are possible you must iterate and unescape:

```cpp
for (auto field : doc.get_object()) {
  // .value() is required: iterating yields simdjson_result<field>, so unescaped_key() comes
  // back wrapped and has no operator== against a string literal.
  if (field.unescaped_key().value() == "date") { /* ... */ }
}
```

`unescaped_key()` costs a copy into the parser's string buffer. `escaped_key()` is faster and points
into the document but returns the key still escaped.

## 15. No `has_key()` / `contains()`

Deliberate: a check followed by a read would scan twice. Probe and use the result:

```cpp
simdjson::ondemand::value v;
if (!obj["optional"].get(v)) {
  // present; v holds it
}
```

With the ordered `find_field()`, a miss leaves the cursor stuck past the field. Save and restore
instead of paying for a full `reset()`:

```cpp
auto pos = object.get_current_position();
double opt;
if (object.find_field("optional").get(opt)) { object.revert_position(pos); }
double next = object.find_field("y");   // still findable
```

A saved position is valid only for that object, and only until it is `reset()` or the parser starts a
new document.

## 16. Thread Safety

One parser per thread; a `document` is an iterator and must stay on one thread. Only `iterate_many` /
`parse_many` use internal threads. Global dispatch state is initialized on first parse and torn down
at the end of `main` — threads still parsing during teardown can race; `std::quick_exit` avoids it.

## 17. On-Demand Validates Only What You Read

A document can start well-formed and end in garbage; you will not know until you reach the garbage —
possibly after acting on the valid prefix. When ingesting untrusted JSON and partial processing is
unacceptable, either use the **DOM** front-end (validates everything up front) or confirm you reached
the end cleanly:

```cpp
// Read the fields you need first, COPYING anything you keep (rewind invalidates views).
// ...

// Then consume the whole document and confirm nothing is left over.
doc.rewind();
std::string_view whole;
if (auto e = doc.raw_json().get(whole); e) { return e; }   // consumes the document
if (!doc.at_end()) { return TRAILING_CONTENT; }
```

Two traps here:

- `at_end()` returns `bool`, not `error_code`, and `true` means **fully consumed** (the good case).
  `if (doc.at_end())` fires on success — the condition you want is `if (!doc.at_end())`.
- `at_end()` is only meaningful **after** you have consumed the whole document. Reading a couple of
  fields with `doc["a"]` leaves the cursor mid-object, so `at_end()` is `false` for a perfectly
  valid document. `rewind()` + `raw_json()` is the cheap way to consume it: the structural index
  from stage 1 is retained, so this is a walk, not a re-parse.

`object::consume()` and `array::consume()` look like the obvious tool for this but are **protected**
— not public API. Use the `rewind()` + `raw_json()` pattern instead.
