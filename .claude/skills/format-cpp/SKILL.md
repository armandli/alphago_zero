---
name: format-cpp
description: Formats C++ code according to 32 specific style rules covering whitespace, braces, preprocessor directives, namespaces, types, formatting, semantic transformations, naming conventions, and comments. Use when user asks to "format my C++ code", "apply C++ style rules", "clean up this C++ file", or "run format-cpp on X". Run refactor-cpp before this skill. Do NOT use for explaining C++ code, debugging, or writing new C++ code from scratch.
argument-hint: "[file or directory path]"
---

## Workflow

### Step 1: Identify Target Files and Detect C++ Standard

If `$ARGUMENTS` is provided, use it as the target path. Otherwise, ask the user which files to format.

- If a directory is given, find all `.cpp`, `.cc`, `.cxx`, `.h`, `.hpp`, `.hxx` files recursively
- If a single file is given, format just that file
- Read each file before modifying it

**Detect the C++ standard version** for the project. Check these sources in order:
1. `CMakeLists.txt`: look for `CMAKE_CXX_STANDARD`, `target_compile_features(... cxx_std_17)`, or similar
2. `Makefile` / `Makefile.*`: look for `-std=c++17`, `-std=c++20`, etc. in `CXXFLAGS` or compiler flags
3. `.clang-format`, `compile_commands.json`, or other tooling config
4. If no standard can be determined, ask the user

Rules D3, G1, and G3 require C++17 or later. If the project targets C++14 or earlier, skip those rules and note it in the report.

### Step 2: Format Each File

Apply rule groups A through H in order. For each file:

1. Read the file completely
2. Apply all applicable rules (see below)
3. Write the modified file
4. If rule G2 converted any project-local `"header.h"` to `<header.h>`, note the header for Step 3 (build file updates)

**Safety principle**: When uncertain whether a transformation is safe, skip it and warn the user. Never break compilation for style.

### Step 3: Update Build Files (if needed)

If any G2 conversions turned project-local `"header.h"` into `<header.h>`, search for `Makefile`, `CMakeLists.txt`, or `BUILD` files in the project:

- Add necessary `-I` include paths so the converted angle-bracket includes resolve
- If no build file is found, warn the user that include paths may need manual adjustment

### Step 4: Report Changes

Summarize what was changed:
- Number of files modified
- Which rule groups were applied
- Any rules that were skipped and why
- Any build file modifications

---

## Rule Groups

### Group A: Whitespace

**A1 — No tabs.** Replace all tab characters with spaces.

**A2 — 2-space indent.** All indentation uses 2 spaces per level. Convert any other indent width.

**A3 — Namespace body not indented.** Content inside `namespace` blocks sits at the same indentation level as the namespace declaration itself.

**A4 — 80-character target.** Break lines that exceed 80 characters. Prefer breaking at logical points (after commas, before operators). This is a target, not a hard limit — don't break lines if it would harm readability.

### Group B: Braces

**B1 — Same-line opening brace.** Opening `{` goes on the same line as the declaration:
```cpp
if (x) {        // yes
void foo() {    // yes
class Bar {     // yes
```

**B2 — Multi-line signature exception.** When a function signature spans multiple lines, the opening brace goes on its own line, aligned with the function declaration:
```cpp
void very_long_function(
    int param1,
    int param2)
{
  // body
}
```

### Group C: Preprocessor

**C1 — Header guards, not `#pragma once`.** Use `#ifndef`/`#define`/`#endif` guards. The guard name is the filename in `UPPER_SNAKE_CASE` with `_H` suffix. Replace any existing `#pragma once`.

**C2 — Tidy includes.** Sort `#include` directives in this order, separated by blank lines:
1. Corresponding header (for `.cpp` files)
2. C system headers (`<cstdlib>`, `<cstring>`, etc.)
3. C++ standard library headers (`<string>`, `<vector>`, etc.)
4. Third-party library headers
5. Project headers

Remove duplicate includes.

### Group D: Namespaces

**D1 — No global `using namespace`.** Remove any `using namespace X;` at file/namespace scope. Replace affected unqualified names with qualified names or namespace aliases.

**D2 — Namespace aliases.** When removing `using namespace` or when a qualified name like `std::chrono::seconds` appears 3+ times, introduce a short alias near the top of the function or file scope. Use the predefined alias mapping in [references/namespace-aliases.md](references/namespace-aliases.md). If the project has a shared alias header (in this repo, `src/util/aliases.h`), `#include <util/aliases.h>` and use its namespace and type aliases (`s::`, `str`, `strv`, `vec<>`, `opt<>`, `umap<>`, ...) instead of declaring local aliases; declare a local alias only for a namespace the header does not cover. A local variable or parameter that shares an alias's name (`s`, `st`, `str`, ...) shadows it, so rename the variable first (e.g. `s` → `text`).

**D3 — Nested namespace syntax. (C++17)** When namespace blocks are nested, collapse them into a single declaration using `namespace A::B::C {`. Apply this to all consecutive nested namespaces. Add a closing comment reflecting the full path: `}  // namespace A::B::C`. Skip this rule if the project targets C++14 or earlier.

### Group E: Types and Members

**E1 — `struct` over `class`.** Use `struct` instead of `class`. Adjust access specifiers accordingly — what was implicit `private:` in a class must become explicit if needed.

**E2 — Default access first.** In a `struct`, put `public` members first (since that's the default), then `protected`, then `private`. Within each section, order: types/aliases, static members, constructors/destructor, methods, data members.

**E3 — `protected` over `private` for methods.** Use `protected` instead of `private` for member functions, to allow overriding in derived types. Data members may remain `private`.

**E4 — `typename` in templates.** Use `typename` instead of `class` in template parameter lists:
```cpp
template<typename T>   // yes
template<class T>      // no
```

**E5 — Inline small methods.** Methods with 3 or fewer statements should be defined inline in the struct/class body. Move longer methods out-of-line.

**E6 — Enum underlying type.** Every `enum class`, `enum struct`, and plain `enum` declaration must include an explicit underlying fundamental type. If none is specified, add `: int` as the default. Only use a type other than `int` when the enum values demonstrably require it (e.g., a flags enum needing more than 32 bits uses `: uint64_t`, an enum that must fit in a byte uses `: uint8_t`). If the declaration already has an explicit underlying type, leave it unchanged.

### Group F: Formatting

**F1 — One item per line at 80 chars.** When a function call, declaration, initializer list, or template parameter list exceeds 80 characters, put each item on its own line with a trailing indent.

**F2 — Trailing commas.** Add trailing commas in initializer lists, enum values, and macro argument lists where the language permits. This makes diffs cleaner.

**F3 — `break; case` compound style.** In `switch` statements, place `break;` on the same line as the next `case` or `default` label (e.g., `break; case State::Init:`). The first case after `switch {` has no `break;` prefix. The last case/default has no trailing `break;`. Put a blank line between case blocks. When multiple cases share the same handler, group them: `break; case X: case Y:` — the `break;` joins the first label, additional labels follow on the same line.

**F3a — Mandatory `default` label.** Every `switch` statement must have a `default` label. If the original code has no `default`, add one with `assert(false); // should never get here` as the body.

**F4 — Alternative operators.** Use `not`, `and`, `or`, `xor` instead of `!`, `&&`, `||`, `^`. Do NOT convert `&` (binary), `|`, `~`, `!=`, unary address-of `&`, or pointer dereference `*`.

### Group G: Semantic Transformations

**G1 — `string` to `string_view`. (C++17)** Convert `const std::string&` parameters to `std::string_view` when the function does not:
- Store the string (assign to a member, push to a container)
- Pass it to a function requiring `const std::string&` or `std::string`
- Call `.c_str()` or `.data()` on it and pass to C APIs expecting null-terminated strings

Also convert local `const std::string` variables that are only read to `std::string_view`. Add `#include <string_view>` if not already present. Skip when safety is uncertain.

**G2 — Angle-bracket includes.** Convert all `#include "header.h"` to `#include <header.h>`, including project-local headers. When converting project-local headers, note them for build file updates in Step 3 — the build system needs `-I` paths added so angle-bracket includes resolve.

**G3 — No raw-pointer returns to `optional`. (C++17)** Convert functions returning raw pointers (`T*`) to return `std::optional<T>` (for value types) or `std::optional<std::reference_wrapper<T>>` (when reference semantics are needed). Update all callers to use `.value()` or `.has_value()` instead of null checks.

**Exceptions — do NOT convert when:**
- The function is `virtual`, `override`, or part of a polymorphic hierarchy
- The pointer is to a base class (polymorphic usage)
- The function is a factory returning ownership (should use `unique_ptr` instead)
- The function is an extern "C" interface

Add `#include <optional>` if not already present.

**G4 — No in-out parameters.** A function parameter must not be used as both input and output. That means a non-const reference or pointer parameter whose incoming value the function reads and also overwrites, like `pos` in `decode(std::string_view s, size_t& pos)`. Callers then can't pass a `const` value or a temporary, and can't look ahead without making a copy. Convert it:
- Take the input by value (or `const&` / `std::string_view` for large types) and leave it unmodified.
- Return the new value. When the function already returns something, return a small named aggregate `struct` holding both, e.g. `struct DecodedCodePoint { char32_t code_point; std::size_t next; };`. Prefer this over `std::pair` / `std::tuple`, because named fields say what each value is.
- When the function can fail, wrap the result in `std::optional<...>` (G3) instead of leaving the parameter "unchanged on error".
- Update every caller: `x = f(..., x)` / `x = r->next`. For loops that advanced the parameter, assign it from the result.

**Exceptions — do NOT convert when:**
- The function's purpose is to modify the object in place: mutating algorithms such as `sort(std::vector<T>&)` or `apply(Buffer&)`, `swap`, compound-assignment operators (`+=`), and stream parameters (`std::ostream&`, `std::istream&`).
- Copying the object to return it would be expensive: large containers or buffers modified in place, especially in hot loops.
- The signature is fixed externally: `virtual` / `override`, `extern "C"`, callbacks matching a library-defined type, or third-party code.
- The parameter is output-only (its incoming value is never read). That is not an in-out parameter; leave it unless it is trivially returnable.

### Group H: Naming Conventions

**H1 — Function names: `lower_snake_case`.** Free functions, member functions, and static methods start with a lowercase letter; words are separated by underscores:
```cpp
void this_is_a_function();   // yes
void ParseInput();           // no
void parseInput();           // no
```
**Exceptions — do NOT rename:**
- Constructors and destructors (name is fixed to the class name by the language)
- Operator overloads (`operator+`, `operator==`, etc.)

**H2 — Class names: `UpperCamelCase`, abbreviated when too long.** `struct`, `class`, and `enum` names start with a capital letter; each subsequent word also starts with a capital letter, with no separator characters:
```cpp
struct ClassConcept {};   // yes
struct HttpRequest {};    // yes
struct http_request {};   // no
struct httpRequest {};    // no
```

When the full name would exceed roughly 24 characters, shorten one or more of its constituent words to a well-known abbreviation instead of spelling every word out:
- If the abbreviation cannot be pronounced as a word, write it in ALL CAPITALS: `URL`, `ID`, `DB`.
- If the abbreviation can be pronounced as a word, treat it like any other word in the name — capitalize only its first letter: `JSON` → `Json`, `HTML` → `Html`.

```cpp
// too long (28 chars) — "UniformResourceLocator" -> "URL" (not pronounceable, all caps)
struct UniformResourceLocatorParser {};   // before
struct URLParser {};                      // after

// too long (31 chars) — "JavaScriptObjectNotation" -> "JSON" (pronounceable, becomes a word)
struct JavaScriptObjectNotationParser {}; // before
struct JsonParser {};                     // after
```

Names at or under ~24 characters are left as-is even if they contain a word that has a common abbreviation.

**H3 — Member variables: `mUpperCamelCase`.** Non-static data members declared under a `private:` or `protected:` access specifier are prefixed with a lowercase `m`, followed by the name in `UpperCamelCase`:
```cpp
struct Session {
protected:
  std::string mSessionId;      // yes
  int mRetryCount;             // yes
  std::string session_id_;     // no
  int m_retry_count;           // no
  int mretrycount;             // no
};
```

Unlike H2, abbreviations after the `m` are capitalized like ordinary words rather than written in all capitals — `mStateId`, `mUrlParser`, `mDbHandle`, not `mStateID` or `mURLParser`. The `m` is what signals "member", so the rest of the name stays visually plain.

**Exceptions — do NOT rename:**
- Public data members. Since E1 prefers `struct`, members with no access specifier above them are public and stay as they are — a plain data carrier like `struct ToolResult { bool ok; std::string output; };` reads worse with the prefix, and renaming it churns every call site.
- Static data members and `static constexpr` constants, which follow their existing convention.
- Members of types you do not own (third-party headers, generated code).

When applying this rule, update every reference to the renamed member — in particular constructor member-initializer lists, in-class default initializers, and any place the old name appears inside the type's own methods. Because the members are private or protected, all references are within the type itself or its derived types, so the rename is contained; if a member turns out to be reachable from outside (a friend declaration, a macro), skip it and note it.

### Group I: Comments

**I1 — Delete comments the code already says.** Assume a reader who fully understands C++ syntax and the standard library. If that reader would learn nothing from the comment, remove it. Deletion is the default disposition: a comment survives only by falling under I2 or the exceptions below.

```cpp
// BEFORE
// Loop over the edits and apply each one.
for (const Edit& edit : edits) {
  apply(edit);
}

// AFTER
for (const Edit& edit : edits) {
  apply(edit);
}
```

The structural comments other rules require are not prose and are never removed by this rule: `#endif  // GUARD_H` (C1) and `}  // namespace A::B::C` (D3) stay exactly as those rules specify.

**I2 — What a comment is for.** Keep an existing comment, and write a new one, only when it carries one of these:

- a **magic number or literal** whose value isn't self-explaining
- a **composed formula** — several standard ones combined into something not recognizable at a glance
- a **condition deliberately left unchecked**, because an upstream guarantee makes it impossible
- an **assumption about input** — a precondition the signature can't state
- an **assumption about output** — a postcondition, ownership, lifetime, or invariant
- a **non-obvious optimization**, and what it buys
- **why** a decision was made, where the alternative would look equally reasonable

The last one is the widest and the easiest to abuse. It covers the reason behind a choice, never a description of the choice — the code already shows *what* was done.

```cpp
// Overwrite rather than append: the notes are already in the prompt
// each turn, so the file can't grow without bound.      <- keep (why)
void MemoryTool::rewrite(std::string_view notes);

// Rewrites the notes file.                              <- delete (what)
void MemoryTool::rewrite(std::string_view notes);
```

**I3 — Brief, and only what's missing.** One or two lines. State the fact the code can't and stop — no restating the surrounding code, no narrating what the next lines will do, no hedging. A comment that qualifies under I2 but runs longer than it needs to is **condensed in place**: keep the fact, drop the prose.

**Exceptions — never removed:**
- The structural comments named in I1 (`#endif`, closing `namespace`).
- License and copyright headers.
- `TODO`, `FIXME`, `HACK`, `NOTE` markers, and anything citing an issue or ticket.
- Comments in code the project doesn't own — third-party, vendored, or generated.
- Tool directives such as `// NOLINT` and `// clang-format off` / `on`. These are instructions to other tools, not prose.
- Commented-out code is outside this rule's scope: report it in Step 4, don't silently delete it.

---

## Error Handling

- **Parse uncertainty**: If you cannot determine whether a transformation is safe, skip it and note it in the report
- **No build file found**: Warn the user but continue formatting; include paths may need manual adjustment
- **Template-heavy code**: Be extra cautious with G1 and G3 in heavily templated code; prefer skipping over breaking
- **C++17 required**: Rules D3, G1, and G3 require C++17 or later. If the standard cannot be detected, ask the user before applying these rules
- **Never break compilation**: If applying a rule would introduce a compile error, skip it
- **Callers out of reach (G4)**: If a function with an in-out parameter is called from code you cannot see or update (a public library API, other repositories), skip it and note it in the report rather than breaking those callers
- **Comment uncertainty**: If you cannot tell whether a comment carries information the code doesn't, keep it. A comment kept in error costs a line; one deleted in error destroys the only record of a decision

## Additional Resources

- For the predefined namespace alias mapping, see [references/namespace-aliases.md](references/namespace-aliases.md)
- For before/after examples of every rule, see [references/examples.md](references/examples.md)
