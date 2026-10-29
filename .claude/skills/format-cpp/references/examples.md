# Before/After Examples

Every rule demonstrated with before and after code. Exception cases noted where applicable.

---

## Group A: Whitespace

### A1 — No tabs + A2 — 2-space indent

```cpp
// BEFORE
void foo() {
	if (x) {
		bar();
	    baz();    // mixed tabs and 4-space
	}
}

// AFTER
void foo() {
  if (x) {
    bar();
    baz();
  }
}
```

### A3 — Namespace body not indented

```cpp
// BEFORE
namespace my_project {
  struct Widget {
    int value;
  };
}

// AFTER
namespace my_project {

struct Widget {
  int value;
};

}  // namespace my_project
```

Note: When A3 and D3 both apply to nested namespaces, D3 collapses them first, then A3 ensures the body is not indented. See the D3 example below.

### A4 — 80-character target

```cpp
// BEFORE
auto result = some_function(very_long_argument_name, another_argument, yet_another_argument, final_arg);

// AFTER
auto result = some_function(
    very_long_argument_name,
    another_argument,
    yet_another_argument,
    final_arg);
```

**Exception — don't break if it harms readability:**
```cpp
// Keep as-is: breaking this makes it worse
#include <unordered_map>
```

---

## Group B: Braces

### B1 — Same-line opening brace

```cpp
// BEFORE
void foo()
{
  if (x)
  {
    bar();
  }
}

struct Widget
{
  int x;
};

// AFTER
void foo() {
  if (x) {
    bar();
  }
}

struct Widget {
  int x;
};
```

### B2 — Multi-line signature exception

```cpp
// BEFORE
void process(
    std::string_view name,
    int count,
    bool verbose) {
  // body
}

// AFTER
void process(
    std::string_view name,
    int count,
    bool verbose)
{
  // body
}
```

Note: B2 overrides B1 only when the signature itself spans multiple lines.

---

## Group C: Preprocessor

### C1 — Header guards

```cpp
// BEFORE (widget.h)
#pragma once

struct Widget {
  int value;
};

// AFTER (widget.h)
#ifndef WIDGET_H
#define WIDGET_H

struct Widget {
  int value;
};

#endif  // WIDGET_H
```

For nested paths like `src/util/parser.h`, use `PARSER_H` (filename only).

### C2 — Tidy includes

```cpp
// BEFORE (widget.cpp)
#include <vector>
#include "widget.h"
#include <string>
#include "utils.h"
#include <cstdlib>
#include <vector>

// AFTER (widget.cpp)
#include <widget.h>

#include <cstdlib>

#include <string>
#include <vector>

#include <utils.h>
```

---

## Group D: Namespaces

### D1 — No global `using namespace` + D2 — Namespace aliases

```cpp
// BEFORE
#include <chrono>
#include <string>
#include <filesystem>

using namespace std;
using namespace std::chrono;
using namespace std::filesystem;

string get_name() {
  auto now = system_clock::now();
  path p = current_path();
  string result = "hello";
  return result;
}

// AFTER
#include <chrono>
#include <string>
#include <filesystem>

std::string get_name() {
  namespace sc = std::chrono;
  namespace sf = std::filesystem;
  auto now = sc::system_clock::now();
  sf::path p = sf::current_path();
  std::string result = "hello";
  return result;
}
```

**When aliases are introduced**: A namespace alias is added when a qualified namespace appears 3+ times in a scope, or when replacing a `using namespace` directive.

**Exception — `using namespace` inside function scope is acceptable but still converted:**
```cpp
// BEFORE
void test() {
  using namespace std::chrono_literals;
  auto duration = 5s;
}

// AFTER — chrono_literals is special, keep using-declaration for literals
void test() {
  using namespace std::chrono_literals;
  auto duration = 5s;
}
```

Literal operator namespaces (`std::chrono_literals`, `std::string_literals`, etc.) are exceptions — `using namespace` is the idiomatic way to use them and should be kept, but only at function scope.

### D3 — Nested namespace syntax (C++17)

```cpp
// BEFORE
namespace my_project {
namespace core {
namespace detail {

struct Widget {
  int value;
};

}  // namespace detail
}  // namespace core
}  // namespace my_project

// AFTER (C++17 or later)
namespace my_project::core::detail {

struct Widget {
  int value;
};

}  // namespace my_project::core::detail
```

**Partially nested — only collapse consecutive nesting:**
```cpp
// BEFORE
namespace outer {

void free_function();

namespace inner {
namespace deep {

struct Foo {};

}  // namespace deep
}  // namespace inner
}  // namespace outer

// AFTER — outer has its own content, so only inner::deep collapse
namespace outer {

void free_function();

namespace inner::deep {

struct Foo {};

}  // namespace inner::deep
}  // namespace outer
```

Only collapse namespaces that are directly and exclusively nested — if a namespace has content besides a single nested namespace, it cannot be collapsed with its child.

**Exception — pre-C++17 project (NOT converted):**
```cpp
// Keep as-is when targeting C++14 or earlier
namespace my_project {
namespace detail {

struct Widget {};

}  // namespace detail
}  // namespace my_project
```

---

## Group E: Types and Members

### E1 — `struct` over `class`

```cpp
// BEFORE
class Widget {
public:
  Widget(int v) : value_(v) {}
  int get() const { return value_; }

private:
  int value_;
};

// AFTER
struct Widget {
  Widget(int v) : mValue(v) {}
  int get() const { return mValue; }

private:
  int mValue;
};
```

### E2 — Default access first

```cpp
// BEFORE
struct Processor {
private:
  int state_;

public:
  using ResultType = int;
  void run();
  Processor();

protected:
  void helper();
};

// AFTER
struct Processor {
  using ResultType = int;

  Processor();
  void run();

protected:
  void helper();

private:
  int mState;
};
```

Order within each section: types/aliases, static members, constructors/destructor, methods, data members.

### E3 — `protected` over `private` for methods

```cpp
// BEFORE
struct Base {
  void public_method();

private:
  void internal_method();    // method — should be protected
  int data_;                 // data — stays private
};

// AFTER
struct Base {
  void public_method();

protected:
  void internal_method();

private:
  int mData;
};
```

### E4 — `typename` in templates

```cpp
// BEFORE
template<class T, class Alloc = std::allocator<T>>
struct Container {
  // ...
};

// AFTER
template<typename T, typename Alloc = std::allocator<T>>
struct Container {
  // ...
};
```

### E5 — Inline small methods

```cpp
// BEFORE (in header)
struct Point {
  int x() const;
  int y() const;
  double distance_to(Point const& other) const;
};

// Defined elsewhere:
int Point::x() const { return x_; }
int Point::y() const { return y_; }
double Point::distance_to(Point const& other) const {
  auto dx = x_ - other.x_;
  auto dy = y_ - other.y_;
  return std::sqrt(dx * dx + dy * dy);
}

// AFTER
struct Point {
  int x() const { return mX; }
  int y() const { return mY; }
  double distance_to(Point const& other) const {
    auto dx = mX - other.mX;
    auto dy = mY - other.mY;
    return std::sqrt(dx * dx + dy * dy);
  }

private:
  int mX, mY;
};
```

`distance_to` has exactly 3 statements, so it qualifies. Methods with 4+ statements stay out-of-line.

### E6 — Enum underlying type

```cpp
// BEFORE — no underlying type specified
enum class Color {
  Red,
  Green,
  Blue,
};

enum class Direction {
  North,
  South,
  East,
  West,
};

// AFTER — `: int` added as default underlying type
enum class Color : int {
  Red,
  Green,
  Blue,
};

enum class Direction : int {
  North,
  South,
  East,
  West,
};
```

**Exception — already has a type (leave as-is):**
```cpp
// Keep as-is: type is already explicit
enum class Status : uint8_t {
  Ok,
  Error,
  Pending,
};
```

**Plain enum (also converted):**
```cpp
// BEFORE
enum OldStyle { A, B, C };

// AFTER
enum OldStyle : int { A, B, C };
```

---

## Group F: Formatting

### F1 — One item per line at 80 chars

```cpp
// BEFORE
std::map<std::string, std::vector<int>> create_mapping(std::string_view key, std::vector<int> const& values, bool sorted);

// AFTER
std::map<std::string, std::vector<int>> create_mapping(
    std::string_view key,
    std::vector<int> const& values,
    bool sorted);
```

### F2 — Trailing commas

```cpp
// BEFORE
enum class Color {
  Red,
  Green,
  Blue
};

// AFTER
enum class Color {
  Red,
  Green,
  Blue,
};
```

Note: C++ does not allow trailing commas in function calls or template arguments. Only add where syntactically valid (enums, initializer lists, macro args).

### F3 — Break before `case`

```cpp
// BEFORE
switch (kind) {
  case TokenKind::Number:
    parseNumber();
    break;
  case TokenKind::Plus:
  case TokenKind::Minus:
    parseBinaryOp();
    break;
  default:
    parseError();
    break;
}

// AFTER
switch (kind) {
  break; case TokenKind::Number:
    parseNumber();

  break; case TokenKind::Plus: case TokenKind::Minus:
    parseBinaryOp();

  break; default:
    parseError();
}
```

### F3a — Mandatory `default` label

```cpp
// BEFORE
switch (dir) {
  case Dir::Up:
    moveUp();
    break;
  case Dir::Down:
    moveDown();
    break;
}

// AFTER
switch (dir) {
  break; case Dir::Up:
    moveUp();

  break; case Dir::Down:
    moveDown();

  break; default:
    assert(false); // should never get here
}
```

### F4 — Alternative operators

```cpp
// BEFORE
if (!valid && (x || y)) {
  result = a ^ b;
  mask = flags & MASK;
  combined = a | b;
  inverted = ~bits;
  if (x != y) {           // comparison operator: do NOT convert
    int* ptr = &obj;      // address-of: do NOT convert
  }
}

// AFTER
if (not valid and (x or y)) {
  result = a xor b;
  mask = flags & MASK;    // binary &: do NOT convert
  combined = a | b;       // binary |: do NOT convert
  inverted = ~bits;       // bitwise NOT: do NOT convert
  if (x != y) {           // comparison operator: unchanged
    int* ptr = &obj;      // address-of: unchanged
  }
}
```

---

## Group G: Semantic Transformations

Rules G1 and G3 require C++17 or later. Skip them entirely if the project targets C++14 or earlier.

### G1 — `string` to `string_view` (C++17)

```cpp
// BEFORE
void print_name(const std::string& name) {
  std::cout << "Hello, " << name << "\n";
}

void store_name(const std::string& name) {
  names_.push_back(name);
}

// AFTER
void print_name(std::string_view name) {
  std::cout << "Hello, " << name << "\n";
}

// store_name: NOT converted — stores the string
void store_name(const std::string& name) {
  names_.push_back(name);
}
```

**Exception — `.c_str()` usage:**
```cpp
// NOT converted — passes to C API expecting null-terminated
void open_file(const std::string& path) {
  FILE* f = fopen(path.c_str(), "r");
}
```

**Exception — passes to function requiring `std::string`:**
```cpp
// NOT converted — callee takes std::string by value
void process(const std::string& data) {
  transform(data);  // transform(std::string s)
}
```

**Local variable conversion:**
```cpp
// BEFORE
void example(std::string_view input) {
  const std::string prefix = input.substr(0, 3);
  std::cout << prefix;
}

// AFTER
void example(std::string_view input) {
  std::string_view prefix = input.substr(0, 3);
  std::cout << prefix;
}
```

### G2 — Angle-bracket includes

```cpp
// BEFORE
#include "string"
#include "vector"
#include "boost/asio.hpp"
#include "my_project/widget.h"

// AFTER
#include <string>
#include <vector>
#include <boost/asio.hpp>
#include <widget.h>    // modify makefile to add -I path if needed
```

After converting project-local headers, the build file (CMakeLists.txt / Makefile) may need `-I` paths added for the angle-bracket includes to resolve.

### G3 — No raw-pointer returns to `optional` (C++17)

**Value type conversion:**
```cpp
// BEFORE
struct Registry {
  Widget* find(int id) {
    auto it = widgets_.find(id);
    if (it != widgets_.end()) {
      return &it->second;
    }
    return nullptr;
  }
};

void caller() {
  Widget* w = registry.find(42);
  if (w) {
    w->activate();
  }
}

// AFTER
struct Registry {
  std::optional<Widget> find(int id) {
    auto it = widgets_.find(id);
    if (it != widgets_.end()) {
      return it->second;
    }
    return std::nullopt;
  }
};

void caller() {
  auto w = registry.find(42);
  if (w.has_value()) {
    w->activate();
  }
}
```

**Reference semantics (when caller needs to modify original):**
```cpp
// BEFORE
Widget* get_active() {
  return active_ ? &active_widget_ : nullptr;
}

// AFTER
std::optional<std::reference_wrapper<Widget>> get_active() {
  return active_
      ? std::optional{std::ref(active_widget_)}
      : std::nullopt;
}
```

**Exception — polymorphic (NOT converted):**
```cpp
// Keep as-is: virtual function, polymorphic usage
struct ShapeFactory {
  virtual Shape* create(std::string_view type) = 0;
};
```

**Exception — ownership transfer (NOT converted, suggest unique_ptr):**
```cpp
// Keep as-is but warn: should use std::unique_ptr
Widget* create_widget() {
  return new Widget();
}
// Suggest: std::unique_ptr<Widget> create_widget()
```

### G4 — No in-out parameters

**Position advanced through a reference → returned in a named struct:**
```cpp
// BEFORE
// On success advances pos; on error returns nullopt and leaves pos unchanged.
std::optional<char32_t> decode_utf8(std::string_view s, std::size_t& pos);

std::size_t count_code_points(std::string_view s) {
  std::size_t count = 0;
  std::size_t pos = 0;
  while (pos < s.size()) {
    if (not decode_utf8(s, pos).has_value()) break;
    ++count;
  }
  return count;
}

// AFTER
struct DecodedCodePoint {
  char32_t code_point;
  std::size_t next;
};

std::optional<DecodedCodePoint> decode_utf8(std::string_view s, std::size_t pos);

std::size_t count_code_points(std::string_view s) {
  std::size_t count = 0;
  std::size_t pos = 0;
  while (pos < s.size()) {
    std::optional<DecodedCodePoint> d = decode_utf8(s, pos);
    if (not d.has_value()) break;
    pos = d->next;
    ++count;
  }
  return count;
}
```

**Value read and overwritten → returned:**
```cpp
// BEFORE
void clamp_width(int& width) {
  if (width > 80) width = 80;
}
clamp_width(w);

// AFTER
int clamp_width(int width) {
  return width > 80 ? 80 : width;
}
w = clamp_width(w);
```

**Exception — in-place mutation is the purpose (NOT converted):**
```cpp
// Keep as-is: the function exists to modify the buffer, and copying it would be costly
void apply(const Edit& edit, std::string& buffer);
void sort_entries(std::vector<Entry>& entries);
std::ostream& operator<<(std::ostream& os, const Widget& w);
```

---

## Group H: Naming Conventions

### H1 — Function names: `lower_snake_case`

```cpp
// BEFORE
void ParseInput();
int GetValue();
void computeTotal();

// AFTER
void parse_input();
int get_value();
void compute_total();
```

**Exception — constructors, destructors, and operator overloads (NOT renamed):**
```cpp
// Keep as-is: name is fixed by the language
struct Widget {
  Widget();
  ~Widget();
  Widget operator+(const Widget& other) const;
};
```

### H2 — Class names: `UpperCamelCase`, abbreviated when too long

```cpp
// BEFORE
struct http_request {};
struct jsonParser {};

// AFTER
struct HttpRequest {};
struct JsonParser {};
```

**Abbreviation when the name exceeds ~24 characters:**
```cpp
// BEFORE — 28 characters, "UniformResourceLocator" abbreviates to "URL"
// (not pronounceable as a word, so it stays all capitals)
struct UniformResourceLocatorParser {};

// AFTER
struct URLParser {};
```

```cpp
// BEFORE — 31 characters, "JavaScriptObjectNotation" abbreviates to "JSON"
// (pronounceable as a word, so it's treated like any other word: only the
// first letter is capitalized)
struct JavaScriptObjectNotationParser {};

// AFTER
struct JsonParser {};
```

**Exception — short names are left as-is:**
```cpp
// Keep as-is: already under ~24 characters, even though "Identifier" has a
// common abbreviation
struct UserIdentifier {};
```

### H3 — Member variables: `mUpperCamelCase`

```cpp
// BEFORE
struct Session {
  Session(std::string id) : session_id_(std::move(id)) {}

  const std::string& id() const { return session_id_; }
  void retry() { ++m_retry_count; }

protected:
  std::string session_id_;
  int m_retry_count = 0;
  UrlParser url_parser_;
};

// AFTER
struct Session {
  Session(std::string id) : mSessionId(std::move(id)) {}

  const std::string& id() const { return mSessionId; }
  void retry() { ++mRetryCount; }

protected:
  std::string mSessionId;
  int mRetryCount = 0;
  UrlParser mUrlParser;   // not mURLParser — abbreviations stay plain here
};
```

Note that the member-initializer list and both method bodies were updated along
with the declarations. Missing one of those is the usual way this rule breaks a
build.

**Exception — public data members are left as-is:**
```cpp
// Keep as-is: no access specifier, so these are public (E1 prefers struct).
// A plain data carrier reads worse with the prefix, and renaming churns every
// call site.
struct ToolResult {
  bool ok = false;
  std::string output;
  std::string error;
};
```

**Exception — static members keep their convention:**
```cpp
struct Registry {
protected:
  int mLiveCount = 0;                          // renamed
  static constexpr int kMaxEntries = 512;      // left alone
  static Registry* sInstance;                  // left alone
};
```

---

## Group I: Comments

### I1 — Delete comments the code already says

```cpp
// BEFORE
// Constructs a Widget.
Widget::Widget(std::string name) : mName(std::move(name)) {}

// Returns the name.
const std::string& Widget::name() const { return mName; }

// Loop over the items and total them up.
int total = 0;
for (const Item& item : items) {
  total += item.value;  // add this item's value to the total
}

// AFTER
Widget::Widget(std::string name) : mName(std::move(name)) {}

const std::string& Widget::name() const { return mName; }

int total = 0;
for (const Item& item : items) {
  total += item.value;
}
```

### I2 — What a comment is for

**Magic number:**
```cpp
// BEFORE
if (is_binary(head.substr(0, 8192))) return Rendering::Unsupported;

// AFTER
// 8192: a NUL in any real text file lands well inside the first block.
if (is_binary(head.substr(0, 8192))) return Rendering::Unsupported;
```

**Composed formula:**
```cpp
// Wilson score lower bound at 95%: mean shifted by z^2/2n, over the
// z-widened denominator. Not the plain proportion.
const double score = (p + z * z / (2 * n) - z * std::sqrt(p * (1 - p) / n)) /
                     (1 + z * z / n);
```

**Condition deliberately left unchecked:**
```cpp
// Non-null: every caller checks in_repo() first.
git_repository_workdir(mRepo);
```

**Assumption about input:**
```cpp
// `head` is the first few KB, not the whole file — callers must not
// pass a multi-megabyte string here.
bool is_binary(std::string_view head);
```

**Assumption about output:**
```cpp
// Borrows from `args`; valid only as long as it is.
const std::vector<Edit>* edits_arg(const ToolArgs& args);
```

**Non-obvious optimization:**
```cpp
// Skips the whole subtree rather than filtering its entries, which is
// what makes a large build/ free instead of merely excluded.
if (filter.ignored(path, is_directory)) it.disable_recursion_pending();
```

**Why, not what — the same line, both ways:**
```cpp
// BEFORE
// Truncate the file and write the new notes.
std::ofstream out(kMemoryPath, std::ios::trunc);

// AFTER
// Overwrite rather than append: the notes are already in the prompt each
// turn, so the file can't grow without bound.
std::ofstream out(kMemoryPath, std::ios::trunc);
```

The code shows the truncation either way. Only the second comment says something the reader couldn't have worked out.

### I3 — Brief, and only what's missing

A comment that qualifies under I2 but runs long is condensed in place — the fact stays, the narration goes.

```cpp
// BEFORE
// This function walks the directory tree starting at the given root. It
// uses a recursive_directory_iterator, and for each entry it encounters
// it asks the IgnoreFilter whether the path should be skipped. If the
// entry is a directory and it is ignored, we call
// disable_recursion_pending so that the iterator does not descend into
// it. Otherwise, if the entry is a regular file, we compute its path
// relative to the root and push it onto the results vector, which is
// then returned to the caller.
std::vector<std::string> walk_files(const std::filesystem::path& root, ...);

// AFTER
// Paths relative to `root`, in iteration order. Ignored directories are
// never descended into, so a large build/ costs nothing.
std::vector<std::string> walk_files(const std::filesystem::path& root, ...);
```

### Exceptions — left untouched

```cpp
#ifndef TOOLS_UTIL_H     // C1 requires the guard
#define TOOLS_UTIL_H

namespace agent {

// TODO(#412): drop once the v2 endpoint ships.
std::string legacy_path();

int parse(const char* text);  // NOLINT(readability-identifier-naming)

}  // namespace agent      <- D3 requires this closing comment

#endif  // TOOLS_UTIL_H    <- C1 requires this one
```

---

## Compound Example: Multiple Rules Interacting (C++17 project)

```cpp
// BEFORE (processor.h)
#pragma once
#include "string"
#include <vector>
#include <processor_utils.h>
#include "string"

using namespace std;

namespace my_project {
namespace core {

template<class T>
class Processor
{
  public:
    Processor(const string& name) : name_(name) {}
    T* process(const string& input)
    {
        if (input.empty() || !validate(input))
        {
            return nullptr;
        }
        auto result = new T();
        return result;
    }

  private:
    string name_;
    bool validate(const string& s) { return !s.empty(); }
};

}
}
```

```cpp
// AFTER (processor.h)
#ifndef PROCESSOR_H
#define PROCESSOR_H

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <processor_utils.h>

namespace my_project::core {

template<typename T>
struct Processor {
  Processor(std::string_view name) : mName(name) {}

  std::optional<T> process(std::string_view input) {
    if (input.empty() or not validate(input)) {
      return std::nullopt;
    }
    return T();
  }

protected:
  bool validate(std::string_view s) { return not s.empty(); }

private:
  std::string mName;
};

}  // namespace my_project::core

#endif  // PROCESSOR_H
```

**Rules applied in this compound example:**
- C1: `#pragma once` -> header guard
- C2: sorted includes, removed duplicate `"string"`
- G2: `"string"` -> `<string>`
- D1: removed `using namespace std`
- D3: `namespace my_project { namespace core {` -> `namespace my_project::core {`
- E4: `class T` -> `typename T`
- E1: `class` -> `struct`
- B1: same-line braces
- A2: 2-space indent (was 4-space)
- A3: namespace body not indented
- E2: public first, then protected, then private
- E3: `validate` moved from private to protected
- F4: `||` -> `or`, `!` -> `not`
- G1: `const string&` -> `string_view` (input is read-only so converted)
- G3: `T*` return -> `std::optional<T>` (not polymorphic, not ownership — was `new`, but result is a value type once fixed)
- H3: `name_` -> `mName`

Note: the `name` parameter stays `std::string_view` even though it's stored in a `std::string` member — the conversion from `string_view` to `string` happens implicitly in the initializer list, which is safe and efficient.

## Compound Example: Pre-C++17 Project

When targeting C++14 or earlier, rules D3, G1, and G3 are skipped. Compared to the C++17 compound example above, only three things differ:

1. **D3 skipped** — namespaces stay separate: `namespace my_project { namespace core {` (with matching closing braces)
2. **G1 skipped** — `const std::string&` parameters stay instead of converting to `std::string_view`
3. **G3 skipped** — `T*` return with `nullptr` stays instead of converting to `std::optional<T>`

All other rules (A, B, C, D1–D2, E, F, G2) apply identically.
