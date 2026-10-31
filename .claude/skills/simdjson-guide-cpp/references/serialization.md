# simdjson Serialization Reference

Verified against simdjson **v5.0.2**. The builder requires **C++20 concepts**
(`SIMDJSON_SUPPORTS_CONCEPTS`); the reflection path additionally requires a P2996 compiler and
`SIMDJSON_STATIC_REFLECTION=1`.

## Read this first: most of the builder is reflection-gated

`include/simdjson/generic/builder/json_builder.h` is wrapped in `#if SIMDJSON_STATIC_REFLECTION`.
With a normal compiler (GCC 15, Clang 21 — reflection **off**, which is the default today) only a
small fallback survives. Verified by compile probe on GCC 15 with reflection off:

| Entry point | Reflection OFF | Reflection ON |
|---|---|---|
| `builder::string_builder` and all its methods | available | available |
| `tag_invoke(serialize_tag, …)` | available | available |
| `to_json(obj, std::string&)` | **available** | available |
| `to_json(obj)` → `simdjson_result<std::string>` | **available** | available |
| `to_json_string(obj)` for a *C++ object* | **NOT available** | available |
| `extract_from<"a","b">(obj)` | **NOT available** | available |
| `sb << obj` (`operator<<`) | **NOT available** | available |
| `to_fractured_json_string(obj)` | **NOT available** | available |
| `to_json_string(ondemand::value&)` — re-emit *parsed* JSON | available | available |

**So: write `to_json(obj, out)`, not `to_json_string(obj)`.** The latter looks like the natural
name and appears throughout simdjson's own docs, but it only compiles under static reflection. The
two `to_json_string` families are easy to confuse — the `ondemand::value` overloads (§9) are always
available and do something completely different.

Check the macro before using a gated feature:

```cpp
#if SIMDJSON_STATIC_REFLECTION
  auto json = simdjson::to_json_string(obj).value();
#else
  std::string json;
  if (auto e = simdjson::to_json(obj, json); e) { return e; }
#endif
```

## Contents

1. [Which entry point to use](#1-which-entry-point-to-use)
2. [`string_builder` full API](#2-string_builder-full-api)
3. [Hand-written serialization](#3-hand-written-serialization)
4. [`tag_invoke` custom serialization](#4-tag_invoke-custom-serialization)
5. [`to_json` and `to_json_string`](#5-to_json-and-to_json_string)
6. [Reflection-based serialization](#6-reflection-based-serialization)
7. [Partial output with `extract_from`](#7-partial-output-with-extract_from)
8. [Pretty-printing with fractured JSON](#8-pretty-printing-with-fractured-json)
9. [Re-emitting parsed JSON](#9-re-emitting-parsed-json)
10. [Correctness traps](#10-correctness-traps)

---

## 1. Which entry point to use

| You have | Use | Notes |
|---|---|---|
| A plain struct, C++26 reflection available | `to_json_string(obj)` | No glue code at all |
| A struct, C++20 only | `tag_invoke(serialize_tag, …)` + `to_json(obj, out)` | One function per type |
| Ad-hoc / dynamic shape | `string_builder` directly | You emit the punctuation |
| A subset of a struct's fields | `extract_from<"a","b">(obj)` | **Reflection only** |
| Human-readable output | `to_fractured_json_string(obj, opts)` | **Reflection only** |
| A parsed `ondemand::value` to pass through | `to_json_string(value)` | Different overload; see §9 |

Everything funnels into `string_builder`, so the three styles compose: a `tag_invoke` for one type
can call `to_json` for its members, and reflection-serialized structs can contain
`tag_invoke`-serialized members.

## 2. `string_builder` full API

```cpp
#include <simdjson.h>
simdjson::builder::string_builder sb;           // default capacity 1024
simdjson::builder::string_builder sb2{1 << 20}; // pre-size to avoid reallocs
```

```cpp
static constexpr size_t DEFAULT_INITIAL_CAPACITY = 1024;
string_builder(size_t initial_capacity = DEFAULT_INITIAL_CAPACITY);
```

### Structure

```cpp
void start_object();   // '{'
void end_object();     // '}'
void start_array();    // '['
void end_array();      // ']'
void append_comma();   // ','
void append_colon();   // ':'
```

### Values

```cpp
template <typename N> void append(N v);        // any arithmetic type, incl. bool
void append(char c);                           // raw character — NOT a JSON string
void append_null();                            // 'null'

void escape_and_append(std::string_view);              // escaped, NO surrounding quotes
void escape_and_append_with_quotes(std::string_view);   // escaped, WITH quotes  <- the usual one
void escape_and_append_with_quotes(char);
void escape_and_append_with_quotes(const char*);
template <constevalutil::fixed_string key>
void escape_and_append_with_quotes();                   // compile-time constant key, fastest
```

`append(bool)` emits `true`/`false`. `append(double)` emits the shortest round-trippable
representation. Integers use a fast digit writer.

Note the asymmetry: `append(char c)` appends the **raw byte**, it does not quote it.
`escape_and_append_with_quotes(char)` is the string version. This is an easy bug — if a field is a
`char` holding `'A'` and you call `append`, you emit a bare `A` and produce invalid JSON.

### Raw passthrough

```cpp
void append_raw(const char* c);                  // NUL-terminated
void append_raw(std::string_view);
void append_raw(const char* str, size_t len);
template <size_t N> void append_raw_n(const char* str);  // compile-time-constant length
```

`append_raw` performs **no escaping and no validation**. Use it only for text you know is already
valid JSON (e.g. a `raw_json()` subtree from a parse, or a number you formatted yourself). Passing
user data to `append_raw` is an injection bug.

### Key-value shorthand

```cpp
template <typename K, typename V> void append_key_value(K key, V value);
template <constevalutil::fixed_string key, typename V> void append_key_value(V value);
```

The key is escaped and quoted; the value is escaped and quoted **only if it is string-like**.
`nullptr` emits `null`. The `fixed_string` template form is faster — the key is written with a
constant-size memcpy.

```cpp
sb.start_object();
sb.append_key_value("name", "ada");     // "name":"ada"
sb.append_comma();
sb.append_key_value<"age">(36);         // "age":36
sb.end_object();
```

### Output and state

```cpp
simdjson_result<std::string_view> view() const;   // view into the builder's buffer
simdjson_result<const char*>      c_str();        // NUL-terminates, then returns the pointer
size_t size() const;                              // bytes written; 0 if an error occurred
bool   validate_unicode() const;                  // true if the content is valid UTF-8
void   clear();                                   // reset position, keep the allocation
operator std::string() const;                     // SIMDJSON_EXCEPTIONS only; may throw
operator std::string_view() const;                // SIMDJSON_EXCEPTIONS only; lifetime-bound
```

`append*` methods are `void` and **never report failure inline**. Allocation failure sets an
internal flag and is surfaced at the end by `view()` / `c_str()` returning `MEMALLOC`. So:

```cpp
std::string_view out;
if (auto e = sb.view().get(out); e) { return e; }   // the ONLY place errors appear
```

Do not skip that check on the assumption that appends cannot fail.

`view()` points into the builder's buffer. It dangles after the builder is destroyed, after
`clear()`, and after any further append that triggers a realloc. Copy into a `std::string` if the
result must outlive the builder.

`clear()` keeps the capacity — reuse one builder across a loop rather than constructing per
iteration.

The builder does **not** validate UTF-8 as it writes. If your inputs may be invalid, call
`validate_unicode()` before shipping the output.

### Stream operator — reflection only

```cpp
template <class Z> string_builder& operator<<(string_builder& b, const Z& z);
```

Defined inside the `SIMDJSON_STATIC_REFLECTION` block, so `sb << obj` **does not compile** on a
normal toolchain. Use `sb.append(obj)` instead, which is always available.

When it is available, `sb << obj` is sugar for `builder::append(sb, obj)` — it serializes the whole
value, it does not concatenate text. `sb << "abc"` emits `"abc"` *with quotes*, not raw `abc`.

## 3. Hand-written serialization

For dynamic shapes, emit the punctuation yourself. The comma-handling pattern:

```cpp
using namespace simdjson;

std::string to_json(const std::vector<record>& rows) {
  builder::string_builder sb{4096};
  sb.start_array();
  bool first = true;
  for (const auto& r : rows) {
    if (!first) { sb.append_comma(); }
    first = false;

    sb.start_object();
    sb.append_key_value("id", r.id);
    sb.append_comma();
    sb.append_key_value("name", r.name);        // escaped for you
    if (r.note) {                                // optional field: emit only when present
      sb.append_comma();
      sb.append_key_value("note", *r.note);
    }
    sb.end_object();
  }
  sb.end_array();

  std::string_view out;
  if (auto e = sb.view().get(out); e) { throw std::runtime_error("serialize failed"); }
  return std::string{out};
}
```

The `bool first` idiom beats "append a comma then trim the last one" — no post-processing, and it
stays correct when fields are conditionally skipped.

## 4. `tag_invoke` custom serialization

Define a free function discoverable by ADL in your type's namespace:

```cpp
namespace myapp {

struct car { std::string make; int64_t year; std::vector<double> tire_pressure; };

void tag_invoke(simdjson::serialize_tag, auto& builder, const car& c) {
  builder.start_object();
  builder.append_key_value("make", c.make);
  builder.append_comma();
  builder.append_key_value("year", c.year);
  builder.append_comma();
  builder.escape_and_append_with_quotes("tire_pressure");
  builder.append_colon();
  builder.append(c.tire_pressure);     // ranges are serialized as JSON arrays
  builder.end_object();
}

} // namespace myapp
```

Returns `void` — errors are collected in the builder, not returned here.

Once defined, the type works everywhere generically:

```cpp
std::string json;
if (auto e = simdjson::to_json(my_car, json); e) { return e; }

std::string list;
if (auto e = simdjson::to_json(std::vector<myapp::car>{a, b}, list); e) { return e; }  // nests
```

Use `auto& builder` rather than naming `string_builder` explicitly — it keeps the signature stable
across simdjson's internal writer types.

`append` already handles, with no work from you:

- any arithmetic type and `bool`
- anything convertible to `std::string_view` (escaped and quoted)
- `std::optional`-like types — a disengaged optional emits `null`
- any `std::ranges::range` — emits a JSON array
- a map whose key is string-view-like — emits a JSON object

So `std::map<std::string, std::vector<std::optional<int>>>` serializes with no custom code.

## 5. `to_json` and `to_json_string`

Always available (both reflection modes):

```cpp
template <class Z>
error_code to_json(const Z& z, std::string& s,
                   size_t initial_capacity = string_builder::DEFAULT_INITIAL_CAPACITY);

template <class Z>
simdjson_result<std::string> to_json(const Z& z,
                   size_t initial_capacity = string_builder::DEFAULT_INITIAL_CAPACITY);
```

Reflection only:

```cpp
template <class Z>
simdjson_result<std::string> to_json_string(const Z& z, size_t initial_capacity = ...);
```

```cpp
// Exception-free, reuses the caller's buffer — best in a loop
std::string buf;
for (const auto& item : items) {
  buf.clear();
  if (auto e = simdjson::to_json(item, buf); e) { return e; }
  send(buf);
}

// One-shot
std::string json;
if (auto e = simdjson::to_json(obj).get(json); e) { return e; }
```

Prefer the `to_json(obj, s)` out-param form on hot paths: it reuses the buffer you already own
instead of returning a fresh `std::string`.

Under **static reflection**, `to_json(obj, s)` additionally gets a fast path — for types whose
serialized size has a static upper bound it writes straight into `s` (via `resize_and_overwrite`
where available) with no intermediate builder and no copy. The non-reflection fallback always goes
through a `string_builder` and then assigns, so the two forms cost the same there.

Both are `simdjson_warn_unused`. Do not discard the result.

## 6. Reflection-based serialization

With C++26 static reflection, plain aggregates need nothing:

```cpp
struct address { std::string city; std::string zip; };
struct user {
  uint64_t id;
  std::string name;
  std::optional<std::string> nickname;
  address home;
  std::vector<std::string> tags;
};

std::string json = simdjson::to_json_string(u).value();
```

The same `simdjson/annotations.h` attributes that control deserialization control output:

```cpp
struct [[= simdjson::rename_all<simdjson::case_style::camel_case>]] account {
  uint64_t id;
  std::string display_name;                                     // -> "displayName"

  [[= simdjson::rename<"e-mail">]] std::string email;           // -> "e-mail"

  [[= simdjson::skip_serializing]] std::string password_hash;   // never written

  [[= simdjson::skip_serializing_if<simdjson::is_none>]]
  std::optional<std::string> bio;                               // omitted when empty

  [[= simdjson::skip_serializing_if<simdjson::is_empty>]]
  std::vector<std::string> tags;                                // omitted when empty

  [[= simdjson::flatten]] pagination page;                      // members hoisted inline

  [[= simdjson::with<unix_time>]] std::chrono::system_clock::time_point created;
};
```

| Annotation | Serialization effect |
|---|---|
| `rename<"k">` | Emit this key instead of the member name |
| `rename_all<style>` | Struct-wide key style; a member `rename` overrides it |
| `skip` | Omit in both directions |
| `skip_serializing` | Omit from output (still read on input) |
| `skip_serializing_if<Pred>` | Omit when `Pred(value)` is true |
| `is_none` | Built-in predicate: empty optional, null pointer |
| `is_empty` | Built-in predicate: `value.empty()` |
| `with<Adapter>` | `Adapter::serialize(builder, value)` handles this member |
| `flatten` | Nested struct's members are emitted at this level |
| `transparent` | Single-member struct emits as that member alone |

`skip_serializing` is the right tool for secrets — it is enforced at compile time, unlike
remembering to clear a field before serializing.

An adapter for `with<>`:

```cpp
struct unix_time {
  static void serialize(simdjson::builder::string_builder& b,
                        const std::chrono::system_clock::time_point& t) {
    b.append(std::chrono::duration_cast<std::chrono::seconds>(t.time_since_epoch()).count());
  }
  static simdjson::error_code deserialize(simdjson::ondemand::value& v,
                                          std::chrono::system_clock::time_point& out) {
    int64_t secs;
    if (auto e = v.get_int64().get(secs); e) { return e; }
    out = std::chrono::system_clock::time_point{std::chrono::seconds{secs}};
    return simdjson::SUCCESS;
  }
};
```

Either function may be omitted; the default behaviour applies in that direction.

Reflection is compile-time-gated. Guard it so the code still builds on older toolchains:

```cpp
#if SIMDJSON_STATIC_REFLECTION
  auto json = simdjson::to_json_string(u);
#else
  auto json = serialize_user_by_hand(u);
#endif
```

## 7. Partial output with `extract_from`

Serialize a named subset of a struct's fields — reflection only:

```cpp
template <constevalutil::fixed_string... FieldNames, typename T>
simdjson_result<std::string> extract_from(const T& obj, size_t initial_capacity = 1024);
```

```cpp
std::string summary;
if (auto e = simdjson::extract_from<"id", "name">(user).get(summary); e) { return e; }
// {"id":1,"name":"ada"}
```

Field names are checked at compile time — a typo is a build error, not a silently missing field.
This is the clean way to emit a public projection of an internal struct without defining a
parallel DTO.

There is also a builder-level overload, `extract_from<"a","b">(sb, obj)`, for embedding the
projection inside a larger document.

## 8. Pretty-printing with fractured JSON

"Fractured" JSON keeps small structures on one line and expands only what does not fit — far more
readable than uniform indentation for nested data.

```cpp
simdjson_result<std::string> to_fractured_json_string(
    const T& obj,
    const fractured_json_options& opts = {},
    size_t initial_capacity = 1024);

template <constevalutil::fixed_string... FieldNames>
simdjson_result<std::string> extract_fractured_json(
    const T& obj, const fractured_json_options& opts = {}, size_t initial_capacity = 1024);
```

```cpp
simdjson::fractured_json_options opts;
opts.indent_spaces = 2;
opts.max_total_line_length = 100;

std::string pretty;
if (auto e = simdjson::to_fractured_json_string(config, opts).get(pretty); e) { return e; }
```

`fractured_json_options`:

| Field | Default | Meaning |
|---|---|---|
| `max_total_line_length` | 120 | Characters per line before expanding |
| `max_inline_complexity` | 2 | Max nesting kept inline (0 = scalars only, 1 = flat, 2 = one level) |
| `max_compact_array_complexity` | 2 | Max element complexity for multiple items per line |
| `indent_spaces` | 4 | Spaces per indent level |
| `always_expand_depth` | -1 | Force full expansion near the root (-1 none, 0 root only, 1 root+children) |
| `enable_table_format` | true | Align columns for arrays of similar objects |

`enable_table_format` is the standout feature: an array of records renders as an aligned table, with
columns ordered by first occurrence and blanks for missing keys. Excellent for diffable fixtures and
CLI output.

These functions need **reflection or a `tag_invoke`** for the type — they take a C++ object, not
parsed JSON. To pretty-print JSON you just *parsed*, you must write the walker yourself; see
[limitations.md](limitations.md#9-pretty-printing-a-parsed-document).

## 9. Re-emitting parsed JSON

A different overload set, declared in `simdjson/generic/ondemand/serialization.h`, turns parsed
On-Demand handles back into minified text:

```cpp
simdjson_result<std::string_view> to_json_string(ondemand::document&);
simdjson_result<std::string_view> to_json_string(ondemand::value&);
simdjson_result<std::string_view> to_json_string(ondemand::object&);
simdjson_result<std::string_view> to_json_string(ondemand::array&);
// plus simdjson_result<...> overloads so you can chain without unwrapping
```

Note the return type: `string_view`, pointing into the **original input buffer**, not an owned
`std::string`. And it **consumes** the value, like `raw_json()`.

```cpp
std::string_view raw;
if (auto e = simdjson::to_json_string(doc["payload"]).get(raw); e) { return e; }
sb.append_raw(raw);           // splice an untouched subtree into new output
```

This is the mechanism for passthrough: parse the fields you care about, re-emit the rest verbatim
without a round-trip through your structs. See
[limitations.md](limitations.md#1-no-document-mutation) for the full unknown-field passthrough
pattern.

For DOM, `simdjson::minify(element)` streams minified JSON to an `ostream`.

## 10. Correctness traps

1. **Check `view()`/`c_str()`.** Appends silently swallow allocation failure. The returned
   `simdjson_result` is the only error channel.
2. **`append_raw` does not escape.** Never hand it user-controlled text; that is a JSON injection.
3. **`append(char)` is not a string.** Use `escape_and_append_with_quotes(char)` for a one-character
   JSON string.
4. **`escape_and_append` omits the quotes.** The `_with_quotes` variant is the one you almost always
   want.
5. **The builder does not validate UTF-8.** Call `validate_unicode()` if inputs are untrusted.
6. **`view()` dangles** after the builder dies, after `clear()`, and after a reallocating append.
   Copy it if it must outlive the builder.
7. **No duplicate-key or depth protection.** The builder writes what you tell it. If your data can
   produce the same key twice, or nest arbitrarily deep, you must prevent it.
8. **Reuse one builder with `clear()`** in loops. Constructing per iteration throws away the
   capacity you already paid for.
9. **`to_json`/`to_json_string`/`extract_from` are `warn_unused`.** Discarding the result discards
   the error.

---

## See also

- [parsing-api.md](parsing-api.md) — the decoding side, including `tag_invoke` for deserialization
- [limitations.md](limitations.md) — mutation, pretty-printing a parsed doc, canonical output
- [examples.md](examples.md) — complete compilable programs
