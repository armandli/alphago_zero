# simdjson Parsing API Reference

Verified against simdjson **v5.0.2**. Every symbol below exists in that release; names changed across
major versions, so check `include/simdjson/generic/ondemand/` if you target a different tag.

## Contents

1. [Parser and document lifetime](#1-parser-and-document-lifetime)
2. [Input types](#2-input-types)
3. [Scalar accessors](#3-scalar-accessors)
4. [Numbers in depth](#4-numbers-in-depth)
5. [Strings in depth](#5-strings-in-depth)
6. [Arrays](#6-arrays)
7. [Objects and field lookup](#7-objects-and-field-lookup)
8. [Key selectors (fast fixed field sets)](#8-key-selectors-fast-fixed-field-sets)
9. [Type introspection](#9-type-introspection)
10. [JSON Pointer and JSONPath](#10-json-pointer-and-jsonpath)
11. [NDJSON and multi-document streams](#11-ndjson-and-multi-document-streams)
12. [Rewinding and position control](#12-rewinding-and-position-control)
13. [Raw JSON access](#13-raw-json-access)
14. [Custom type deserialization](#14-custom-type-deserialization)
15. [The DOM front-end](#15-the-dom-front-end)

---

## 1. Parser and document lifetime

```cpp
#include <simdjson.h>
using namespace simdjson;

ondemand::parser parser;                       // reuse this; it owns the scratch buffers
padded_string json = padded_string::load("f.json");
ondemand::document doc = parser.iterate(json); // doc borrows BOTH parser and json
```

`parser.iterate()` returns `simdjson_result<ondemand::document>`. The document is a cursor into
`json`'s bytes using `parser`'s scratch space. **Both must outlive the document and everything
obtained from it** (`value`, `object`, `array`, and any `std::string_view`).

A parser holds exactly one live document at a time. Calling `iterate()` again invalidates the
previous document.

```cpp
error_code allocate(size_t capacity, size_t max_depth = DEFAULT_MAX_DEPTH) noexcept;
size_t capacity() const noexcept;
size_t max_depth() const noexcept;
size_t max_capacity() const noexcept;
void set_max_capacity(size_t max_capacity) noexcept;
implementation& active_implementation();        // which SIMD kernel got selected
```

`allocate()` is optional — `iterate()` grows the parser on demand. Call it to pre-size and avoid a
realloc on the first large document.

**`max_depth` does not limit depth in On-Demand.** Per the header, it "is only relevant when the
macro `SIMDJSON_DEVELOPMENT_CHECKS` is set to true", and measurement confirms a 40-deep document
parses fine under `allocate(4096, 5)` in both release and development-checks builds. Only
`dom::parser` actually returns `DEPTH_ERROR`. To bound depth with On-Demand, count it yourself in
your traversal — see
[limitations.md](limitations.md#12-depth-is-your-recursion-not-simdjsons).

`set_max_capacity()` makes oversized inputs fail with `CAPACITY` instead of allocating. Use it when
the input size is attacker-controlled.

Document state checks:

```cpp
bool is_alive() noexcept;        // false after a fatal error; the doc cannot be used further
bool at_end() const noexcept;    // true when the cursor consumed the whole document
simdjson_result<const char*> current_location() const noexcept;  // byte pointer, for error messages
int32_t current_depth() const noexcept;
```

## 2. Input types

simdjson requires `SIMDJSON_PADDING` readable bytes past the end of the JSON. Pick the input type
that gives you that without a copy.

| Type | Owns memory | Use when |
|---|---|---|
| `padded_string` | yes | You are reading a file or own the buffer; simplest correct choice |
| `padded_string::load(path)` | yes | Load a whole file; returns `simdjson_result<padded_string>` |
| `padded_string_view` | no | You already have a buffer with ≥ `SIMDJSON_PADDING` slack |
| `padded_input` | no | Non-owning view over a buffer whose padding you guarantee |
| `padded_memory_map` | maps file | Huge files you do not want to copy into RAM |
| `std::string&` (non-const lvalue) | — | simdjson calls `pad()` on it in place; **must be a mutable lvalue** |
| `"..."_padded` literal | yes | Tests and examples |

```cpp
using namespace simdjson;

// File
auto json = padded_string::load("twitter.json");       // simdjson_result<padded_string>

// Memory map a large file (no copy)
padded_memory_map mm;
if (auto e = padded_memory_map::load("huge.json").get(mm); e) { /* handle */ }
auto doc = parser.iterate(mm);

// Existing std::string — note the lvalue; a temporary is a deleted overload
std::string body = fetch_http_body();
auto doc2 = parser.iterate(body);   // pads body in place, may realloc it

// Raw buffer you padded yourself
auto doc3 = parser.iterate(padded_string_view(buf, len, buf_capacity));
```

`parser.iterate(const std::string&&)` and `iterate(const char*)` with no length are **deleted** on
purpose — they would dangle or read past the end. If the compiler rejects your call, you are being
protected; wrap the data in a `padded_string` instead.

Pad an existing string yourself:

```cpp
inline padded_string_view pad(std::string& s) noexcept;
```

```cpp
std::string s = "...";
auto doc = parser.iterate(simdjson::pad(s));   // grows s's capacity, keeps size(), returns a view
```

`pad()` takes a **mutable lvalue** and returns a `padded_string_view` borrowing it — it does not copy.
The view (and the document) dangle if `s` is modified or destroyed afterward.

## 3. Scalar accessors

Every accessor on `value`/`document` returns `simdjson_result<T>`. Two ways to consume it:

```cpp
// (a) error_code out-param — works with SIMDJSON_EXCEPTIONS off
uint64_t n;
if (auto e = v.get_uint64().get(n); e) { return e; }

// (b) implicit conversion — throws simdjson_error
uint64_t n2 = v.get_uint64();        // or: uint64_t n3 = v;
```

Full accessor list on `document` and `value`:

```cpp
simdjson_result<array>            get_array();
simdjson_result<object>           get_object();
simdjson_result<bool>             get_bool();
simdjson_result<bool>             is_null();      // true if the value is JSON null
simdjson_result<std::string_view> get_string(bool allow_replacement = false);
simdjson_result<std::u8string_view> get_u8string(bool allow_replacement = false);  // C++20 char8_t
simdjson_result<std::string_view> get_wobbly_string();
simdjson_result<raw_json_string>  get_raw_json_string();

simdjson_result<uint64_t> get_uint64();   simdjson_result<int64_t> get_int64();
simdjson_result<uint32_t> get_uint32();   simdjson_result<int32_t> get_int32();
simdjson_result<uint16_t> get_uint16();   simdjson_result<int16_t> get_int16();
simdjson_result<uint8_t>  get_uint8();    simdjson_result<int8_t>  get_int8();
simdjson_result<double>   get_double();   simdjson_result<float>   get_float();
simdjson_result<std::float32_t> get_float32();   // C++23 <stdfloat>
simdjson_result<std::float64_t> get_float64();
```

The narrow integer accessors (`get_uint8` … `get_int32`) parse as 64-bit then range-check, returning
`NUMBER_OUT_OF_RANGE` on overflow. They are not faster than `get_uint64()`; use them to get the
bounds check for free.

`*_in_string` variants read a number that the producer quoted (`{"id":"12345"}`):

```cpp
simdjson_result<uint64_t> get_uint64_in_string();
simdjson_result<int64_t>  get_int64_in_string();
simdjson_result<double>   get_double_in_string();
simdjson_result<float>    get_float_in_string();
```

Generic form, which is also the hook for your own types (see §14):

```cpp
template <typename T> simdjson_result<T> get();         // T deduced from the call
template <typename T> error_code         get(T& out);   // preferred, exception-free
```

## 4. Numbers in depth

`get_number()` parses once and tells you what it found — use it when the JSON may hold either an
integer or a double:

```cpp
ondemand::number num;
if (auto e = v.get_number().get(num); e) { return e; }

switch (num.get_number_type()) {
  case ondemand::number_type::signed_integer:   use(num.get_int64());   break;
  case ondemand::number_type::unsigned_integer: use(num.get_uint64());  break;
  case ondemand::number_type::floating_point_number: use(num.get_double()); break;
  case ondemand::number_type::big_integer:      /* > 64 bits; see below */ break;
}
```

`number` also exposes `is_double()`, `is_uint64()`, `is_int64()`.

Cheaper type probe that does **not** fully parse — it inspects only the leading characters:

```cpp
simdjson_result<number_type> get_number_type();
simdjson_result<bool> is_integer();      // no fractional part / exponent
bool is_negative();                      // leading '-'
```

`number_type::big_integer` means the literal does not fit in `uint64_t`/`int64_t`. simdjson will not
lose data silently — it reports the category and you decide. Recover the digits with
`raw_json_token()`; see
[limitations.md](limitations.md#5-big-integers-and-exact-decimals-are-not-parsed-for-you).

By default `NaN`/`Infinity` are rejected as invalid JSON. Compile with `SIMDJSON_ENABLE_NAN_INF=1`
to accept them (non-standard input only).

## 5. Strings in depth

`get_string()` unescapes into the parser's scratch buffer and hands you a `std::string_view` into
it. That view is **invalidated by the next string read on the same parser**. Copy it if you need it
to live:

```cpp
std::string_view sv;
if (auto e = v.get_string().get(sv); e) { return e; }
std::string owned{sv};        // now safe past the next read

// Or write straight into your own storage — no intermediate view
std::string dst;
if (auto e = v.get_string(dst); e) { return e; }   // overload taking a receiver
```

The receiver overload accepts any type with `resize()` and `data()` (`std::string`,
`std::vector<char>`, small-string types).

| Accessor | Invalid UTF-8 / lone surrogate behaviour |
|---|---|
| `get_string()` | returns `STRING_ERROR` |
| `get_string(true)` | substitutes U+FFFD for unpaired surrogates |
| `get_wobbly_string()` | passes lone surrogates through as WTF-8 (lossless round-trip) |
| `get_raw_json_string()` | no unescaping at all; raw bytes between the quotes |

`raw_json_string` is the zero-copy escape hatch. It is a pointer into the original document with
`raw()` (bytes, not NUL-terminated at the closing quote) and comparison operators that compare
against an unescaped `std::string_view` **without** unescaping the document:

```cpp
for (auto field : obj) {
  ondemand::raw_json_string key;
  if (auto e = field.key().get(key); e) { return e; }
  if (key == "user_id") { ... }       // fast path, no scratch-buffer write
}
```

Unescape it explicitly when needed: `parser.unescape(raw, dst_ptr, allow_replacement)`.

## 6. Arrays

```cpp
for (auto element : doc["results"].get_array()) {
  // element is simdjson_result<ondemand::value>
  std::string_view name;
  if (auto e = element["name"].get_string().get(name); e) { return e; }
}
```

The loop variable is a `simdjson_result`, so errors surface at the point of use, not at `begin()`.
With exceptions enabled you can write `for (ondemand::value element : arr)`.

```cpp
simdjson_result<array_iterator> begin() &;
simdjson_result<array_iterator> end();
simdjson_result<size_t> count_elements() &;   // walks the array, then rewinds it
simdjson_result<bool>   is_empty() &;         // cheap: looks at the next token only
simdjson_result<bool>   reset() &;            // restart iteration of this array
simdjson_result<value>  at(size_t index);     // O(index), forward scan — NOT random access
simdjson_result<std::string_view> raw_json(); // the array's source text (consumes it)
simdjson_result<value>  at_pointer(std::string_view);
simdjson_result<value>  at_path(std::string_view);
template <typename T> error_code get(T& out);
```

`count_elements()` costs a full pass over the array's tokens. If you need the count *and* the
elements, call `count_elements()` first (it rewinds), then iterate — do not iterate and then count.

`at(i)` is a forward scan from the current position. A loop of `at(0..n)` is O(n²); iterate instead.

## 7. Objects and field lookup

```cpp
ondemand::object obj = doc.get_object();
for (auto field : obj) {
  std::string_view key = field.unescaped_key();   // or field.key() for raw_json_string
  ondemand::value  val = field.value();
}
```

`object_iterator`'s `field` type:

On a bare `ondemand::field`:

```cpp
raw_json_string   key() const noexcept;          // zero-copy, still escaped — NOT wrapped
std::string_view  escaped_key() const noexcept;  // raw bytes, no quotes — NOT wrapped
simdjson_result<std::string_view> unescaped_key(bool allow_replacement = false);
error_code        unescaped_key(string_type& receiver, bool allow_replacement = false);
ondemand::value&  value() & noexcept;
```

**But iterating an object yields `simdjson_result<field>`, not `field`**, and on the result wrapper
every accessor is wrapped:

```cpp
for (auto field : obj) {                   // field is simdjson_result<ondemand::field>
  std::string_view k;
  if (auto e = field.escaped_key().get(k); e) { return e; }   // wrapped: must unwrap
  // ...
}
```

So `escaped_key()` returns a bare `string_view` in the reference but a `simdjson_result` in the code
you actually write. `std::string(f.escaped_key())` will not compile — use
`f.escaped_key().value()` or `.get(out)`.

### `operator[]` vs `find_field`

| Call | Semantics | Cost |
|---|---|---|
| `obj["key"]` / `find_field_unordered("key")` | Scans forward to the end, then **wraps around** from the object's start. Order-independent. | O(fields), tolerant |
| `obj.find_field("key")` | Scans forward only. Fails with `NO_SUCH_FIELD` if the key was already passed. | O(fields to key), fastest |

Use `find_field` when you know the producer's key order — it never backtracks. Use `operator[]` when
key order is not guaranteed (most real-world JSON). **Never assume `operator[]` is cheap:** looking
up *k* keys in an object of *n* fields is O(k·n). For a known set of keys, prefer a single pass with
the iterator or a key selector (§8).

Repeated `operator[]` on the same key does **not** work — the field is consumed. See the
[single-consumption rule](limitations.md#2-forward-only-single-consumption).

```cpp
simdjson_result<size_t> count_fields() &;
simdjson_result<bool>   is_empty() &;
simdjson_result<std::string_view> raw_json();      // source text (consumes the object)
simdjson_result<bool>   reset() &;                 // restart from the first field
object_position         get_current_position() const;   // opaque save point
error_code              revert_position(object_position);
simdjson_result<value>  at_pointer(std::string_view);
simdjson_result<value>  at_path(std::string_view);
template <typename T> error_code get(T& out);
template <typename T> error_code extract_into(T& out) &;
```

`reset()` / `get_current_position()` / `revert_position()` are the only sanctioned way to re-read
fields of the same object. `reset()` is how you implement a `has_key()` probe without destroying
your place; `revert_position()` is the cheaper variant that does not rescan from the start.

Two naming traps: the save-point accessor is **`get_current_position()`**, not `position()`, and
`reset()` returns **`simdjson_result<bool>`** (false means the object was empty), not `error_code`.

`object::consume()` and `array::consume()` exist in the headers but are **`protected`** — they are
not public API. To skip the remainder of a container, use `raw_json()` (which consumes it) or just
stop reading.

## 8. Key selectors (fast fixed field sets)

C++20 and later. When you know the exact field names at compile time, `key_selector` builds a
perfect hash and extracts them in **one pass, order-independent**, which beats *k* separate
`operator[]` lookups.

```cpp
// Bind fields straight to variables — one handler per key, in key order
std::string_view name, city;
uint64_t age = 0;
if (auto e = obj.for_each<"name", "city", "age">(name, city, age); e) { return e; }
```

```cpp
// Named selector, index-based callback (use for shared state or complex logic)
using fields = ondemand::key_selector<"id", "text", "user">;
auto res = obj.for_each<fields>([&](std::size_t i, ondemand::value v) -> error_code {
  switch (i) {
    case 0: return v.get_uint64().get(out.id);
    case 1: return v.get_string().get(out.text);
    case 2: return read_user(v, out.user);
  }
  return SUCCESS;
});
if (res.error) { return res.error; }
// res.matched_count == number of distinct selector keys handled
```

`for_each_result` is a `[[nodiscard]]` aggregate with **public fields, not accessors**:

```cpp
struct [[nodiscard]] for_each_result {
  error_code  error{SUCCESS};
  std::size_t matched_count{0};
  constexpr operator error_code() const noexcept;   // so `if (auto e = obj.for_each...)` works
};
```

It is `[[nodiscard]]` on purpose: `for_each` reports parse and handler errors **only** through this
result, which in a no-exceptions build is the sole error channel. To ignore it deliberately, cast to
`void`.

Handlers may be variables (assigned via `value::get`) or invocables, mixed freely. An invocable
returning `error_code` lets you surface parse errors; returning `void` ignores them.

Constraints, all compile-time enforced except the last:

- Each key ≤ 63 characters; keys distinct, non-empty, no backslash / double-quote / NUL.
- Hard limit 255 keys, but **keep it to a handful** — the perfect hash may fail to build or slow
  compilation badly for large sets.
- First occurrence of a duplicate key wins; iteration stops once all keys matched.
- `for_each` **consumes** the object. Do not touch that `object` instance afterward.
- Conditionally `noexcept`: a throwing callback (e.g. one using `uint64_t(value)` conversions)
  propagates out rather than hitting a `noexcept` boundary.

## 9. Type introspection

```cpp
switch (v.type()) {
  case ondemand::json_type::array:   /* v.get_array()  */ break;
  case ondemand::json_type::object:  /* v.get_object() */ break;
  case ondemand::json_type::number:  /* v.get_number() */ break;
  case ondemand::json_type::string:  /* v.get_string() */ break;
  case ondemand::json_type::boolean: /* v.get_bool()   */ break;
  case ondemand::json_type::null:    /* v.is_null()    */ break;
}
```

`type()` only inspects the first byte of the token — it is cheap and does **not** advance or
validate. It cannot tell you integer-vs-double (use `get_number_type()`).

```cpp
simdjson_result<bool> is_scalar();   // not array, not object
simdjson_result<bool> is_string();
```

`is_null()` is an accessor, not just a predicate: it returns `true`/`false` for a well-formed value
and only advances past `null`.

## 10. JSON Pointer and JSONPath

```cpp
simdjson_result<value> at_pointer(std::string_view json_pointer);  // RFC 6901
simdjson_result<value> at_path(std::string_view path);             // JSONPath subset
```

```cpp
// JSON Pointer: /-separated, 0-based array indices, ~0 => '~', ~1 => '/'
auto v = doc.at_pointer("/results/0/user/screen_name");

// JSONPath: dotted and bracketed
auto w = doc.at_path(".results[0].user.screen_name");
```

On a `document`, `at_pointer()` **calls `rewind()` first**, so repeated lookups from the document
root work. On a `value`, it does not rewind and is subject to the normal forward-only rule.

Both are convenience wrappers over forward scanning — a path lookup costs the same as walking there
by hand. In a hot loop, hand-written navigation is clearer and no slower.

Wildcards:

```cpp
error_code for_each_at_path_with_wildcard(std::string_view json_path, Func&& callback);
```

```cpp
doc.for_each_at_path_with_wildcard(".users[*].id", [&](ondemand::value v) -> error_code {
  uint64_t id;
  if (auto e = v.get_uint64().get(id); e) { return e; }
  ids.push_back(id);
  return SUCCESS;
});
```

simdjson does **not** implement full JSONPath (no filters, no recursive descent `..`, no slices). See
[limitations.md](limitations.md#7-no-json-schema-json-patch-or-jsonpath-filters).

## 11. NDJSON and multi-document streams

```cpp
ondemand::document_stream docs;
if (auto e = parser.iterate_many(json).get(docs); e) { return e; }

for (auto doc : docs) {
  std::string_view name;
  if (auto e = doc["name"].get_string().get(name); e) { continue; }  // skip bad line
  use(name);
}
```

```cpp
simdjson_result<document_stream> iterate_many(padded_string_view json,
                                              size_t batch_size = DEFAULT_BATCH_SIZE);
simdjson_result<document_stream> iterate_many(const char* buf, size_t len,
                                              size_t batch_size, stream_format format);
```

`stream_format` (from `simdjson/base.h`):

| Value | Input shape |
|---|---|
| `whitespace_delimited` | default; covers NDJSON/JSONL and any whitespace separation |
| `newline_delimited` | strict NDJSON: one document per line, no raw LF inside a document |
| `json_sequence` | RFC 7464, RS-delimited |
| `comma_delimited` | `{...},{...},{...}` with no enclosing brackets |
| `comma_delimited_array` | `[{...},{...}]` — strips the outer brackets, then behaves like `comma_delimited` |

`newline_delimited` is faster than the default when your input really is one-doc-per-line.

`batch_size` must exceed the largest single document. The default (1 MB) is tuned for throughput;
raising it past a few MB usually hurts (cache pressure). A document larger than `batch_size` yields
`CAPACITY`.

Stream and iterator introspection:

```cpp
size_t size_in_bytes() const;        // total input length
size_t truncated_bytes() const;      // trailing bytes that form an incomplete document
// on the iterator:
size_t current_index() const;        // byte offset of the current document
std::string_view source() const;     // raw text of the current document
```

`truncated_bytes()` is how you handle a stream cut mid-document: parse what you have, carry the
trailing `truncated_bytes()` into the next read. See
[limitations.md](limitations.md#10-no-incremental--streaming-input).

The `std::string&&` and `padded_string&&` overloads of `iterate_many` are **deleted** — the stream
would outlive the buffer.

## 12. Rewinding and position control

```cpp
void rewind() noexcept;      // document: reset the cursor to the very start
error_code reset() noexcept; // object/array: restart iteration of THIS container
```

`doc.rewind()` is cheap: the structural index built by stage 1 is retained, so a second pass costs
roughly the stage-2 time only. It is the right answer when you need two unrelated passes over one
document.

`rewind()` invalidates every `value`, `object`, `array`, and `string_view` previously obtained from
the document. Re-derive them after rewinding.

Fine-grained save/restore inside an object:

```cpp
auto saved = obj.position();
// ... probe some fields ...
obj.revert_position(saved);
```

Also useful for error reporting:

```cpp
simdjson_result<const char*> current_location();  // byte pointer into the input
int32_t current_depth() const;                    // nesting level, root == 1
```

`current_location()` after an error points at the offending byte — compute the offset against your
buffer's `data()` to report a line/column.

## 13. Raw JSON access

```cpp
simdjson_result<std::string_view> raw_json_token();  // document
std::string_view                  raw_json_token();  // value  <- NOT wrapped
simdjson_result<std::string_view> raw_json();        // value / object / array
```

Mind the asymmetry: on a `document` you must unwrap the result; on a `value` it returns the
`string_view` directly.

```cpp
std::string_view tok = v.raw_json_token();            // value: direct
std::string_view dtok;
if (auto e = doc.raw_json_token().get(dtok); e) { return e; }   // document: unwrap
```

`raw_json_token()` returns the current scalar's source text (plus trailing whitespace) **without
parsing it**. This is the escape hatch for anything simdjson will not parse for you: big integers,
exact decimals, custom number formats.

`raw_json()` returns the full source text of a container — the zero-copy way to pass a subtree
through untouched:

```cpp
std::string_view subtree;
if (auto e = doc["metadata"].raw_json().get(subtree); e) { return e; }
// subtree points into the original buffer; copy it if it must outlive the input
```

`raw_json()` on a container **consumes** it. Neither function validates the text it returns — it is
syntactically well-formed by construction, but a `raw_json_token()` of a big integer is just digits,
not a number you can trust in arithmetic.

To re-emit a parsed value as minified JSON, use `simdjson::to_json_string(value)`; for a
pretty-printed form see [limitations.md](limitations.md#9-pretty-printing-a-parsed-document).

## 14. Custom type deserialization

### Preferred: `tag_invoke` (C++20 concepts)

Define a free function found by ADL in your type's namespace:

```cpp
struct point { double x, y; };

namespace myapp {
  error_code tag_invoke(simdjson::deserialize_tag, auto& val, point& out) noexcept {
    simdjson::ondemand::object obj;
    if (auto e = val.get_object().get(obj); e) { return e; }
    if (auto e = obj["x"].get_double().get(out.x); e) { return e; }
    if (auto e = obj["y"].get_double().get(out.y); e) { return e; }
    return simdjson::SUCCESS;
  }
}
```

Then every generic entry point works, including nesting and containers:

```cpp
point p;
if (auto e = doc.get<point>().get(p); e) { return e; }

std::vector<point> path;
if (auto e = doc["path"].get<std::vector<point>>().get(path); e) { return e; }
```

Mark the function `noexcept` when it cannot throw — simdjson propagates that through the
`nothrow_deserializable` concept, which keeps `get<T>()` `noexcept` for callers.

The `auto&` parameter lets one overload serve `value`, `object`, `array`, `document`, and
`document_reference`. Write it that way unless you need per-source behaviour.

Types simdjson deserializes out of the box: `int64_t`, `uint64_t`, `double`, `bool`,
`std::string_view`, `std::u8string_view`, `array`, `object`, `value`, `raw_json_string`, plus
`std::optional`-like types and any range of a deserializable type.

### C++26 static reflection (zero boilerplate)

With a compiler that supports P2996 reflection and `SIMDJSON_STATIC_REFLECTION=1`, plain structs
work with no glue at all:

```cpp
struct user { uint64_t id; std::string name; bool active; };

user u;
if (auto e = doc.get<user>().get(u); e) { return e; }
```

Annotations from `simdjson/annotations.h` control the mapping:

| Annotation | Effect |
|---|---|
| `[[= simdjson::rename<"user_id">]]` | Map the member to a different JSON key |
| `[[= simdjson::alias<"uid", "userId">]]` | Accept additional key spellings on input |
| `[[= simdjson::rename_all<simdjson::case_style::camel_case>]]` | Applied to the struct; member `rename` wins |
| `[[= simdjson::skip]]` | Ignore in both directions |
| `[[= simdjson::skip_deserializing]]` / `skip_serializing` | One direction only |
| `[[= simdjson::default_value]]` | Missing key leaves the member at its default initializer instead of `NO_SUCH_FIELD` |
| `[[= simdjson::default_from<factory>]]` | Missing key assigns `factory()` |
| `[[= simdjson::with<Adapter>]]` | Adapter supplies `serialize` / `deserialize` for that member |
| `[[= simdjson::flatten]]` | Nested struct's members appear at the enclosing level |
| `[[= simdjson::transparent]]` | Single-member struct is (de)serialized as that member alone |
| `[[= simdjson::deny_unknown_fields]]` | Unexpected JSON key → `UNKNOWN_FIELD` instead of being ignored |

`deny_unknown_fields` is the right default for config files; leave it off for API payloads you want
to be forward-compatible with.

Reflection is a **compile-time feature switch**. Guard it, and keep a `tag_invoke` fallback if you
must build on older toolchains:

```cpp
#if SIMDJSON_STATIC_REFLECTION
  // reflection path
#else
  // tag_invoke path
#endif
```

### Convenience: `simdjson::from`

```cpp
auto u = simdjson::from(R"({"id":1,"name":"ada"})"_padded).get<user>();
auto v = simdjson::from(parser, json).get<user>();   // reuse a parser
```

`from()` creates (or borrows) a parser, iterates, and converts in one expression. It owns the
temporary parser, so this is safe for one-shot parsing — but in a loop it reallocates. Use an
explicit long-lived `parser` on any hot path.

## 15. The DOM front-end

Use DOM only when you need random access, repeated access, or out-of-order access to the same
document. It materializes a tape plus a string buffer, costing roughly 2–4× On-Demand's time and
allocating proportional to document size.

```cpp
#include <simdjson.h>
using namespace simdjson;

dom::parser parser;
dom::element doc = parser.load("twitter.json");   // or parser.parse(json)

for (dom::object tweet : doc["statuses"]) {
  std::string_view text = tweet["text"];
  int64_t id            = tweet["id"];
  // Re-reading the same key is fine here — this is the DOM, not On-Demand
  std::string_view again = tweet["text"];
}
```

```cpp
simdjson_result<element> parser.parse(padded_string_view json);
simdjson_result<element> parser.load(const std::string& path);
simdjson_result<document_stream> parser.parse_many(json, batch_size);
simdjson_result<document_stream> parser.load_many(path, batch_size);
```

`dom::element` accessors mirror On-Demand but are **repeatable and order-independent**:

```cpp
simdjson_result<dom::array>  get_array();
simdjson_result<dom::object> get_object();
simdjson_result<const char*> get_c_str();
simdjson_result<std::string_view> get_string();
simdjson_result<int64_t> get_int64();   simdjson_result<uint64_t> get_uint64();
simdjson_result<double>  get_double();  simdjson_result<bool>     get_bool();
bool is_null() const;
dom::element_type type() const;
simdjson_result<element> at_pointer(std::string_view) const;
simdjson_result<element> operator[](std::string_view) const;
```

`dom::array` has `size()`, `at(index)` (true O(1)-ish indexed access), and `begin()`/`end()`.
`dom::object` has `size()`, `operator[]`, `at_pointer()`, and `begin()`/`end()`.

Strings from DOM live in the parser's string buffer and stay valid until the next `parse()` on that
parser — longer-lived than On-Demand's scratch views, but still not forever.

Minify a DOM element back to text:

```cpp
#include <simdjson.h>
std::cout << simdjson::minify(doc) << "\n";
```

DOM has **no mutation API**. Rebuild, don't edit — see
[limitations.md](limitations.md#1-no-document-mutation).

---

## See also

- [limitations.md](limitations.md) — what simdjson will not do, with working workarounds
- [serialization.md](serialization.md) — producing JSON with `string_builder` / `to_json`
- [examples.md](examples.md) — complete compilable programs
