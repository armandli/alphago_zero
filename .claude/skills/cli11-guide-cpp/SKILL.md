---
name: cli11-guide-cpp
description: Expert reference guide for the CLI11 C++ command-line parser. Auto-activates when writing or reviewing C++ code that parses command-line arguments, flags, options, positionals, subcommands, or argv; when CLI::App, CLI11_PARSE, add_option, add_flag, add_subcommand, or CLI11.hpp appear in code; or when the user asks how to parse argv in C++, add a --flag or subcommand, validate option values, read options from environment variables or a TOML/INI config file, customize help output, or bind options to vectors, enums and custom types. Covers correct usage patterns, the lifetime and ordering rules that cause most CLI11 bugs, CMake setup, error handling and exit codes, and features CLI11 does not provide (shell autocompletion, option abbreviation) that need custom code. Do NOT use for argument parsing in other languages, for getopt/Boost.Program_options/argparse code with no CLI11 angle, or for general C++ string parsing unrelated to argv.
---

# CLI11 C++ Guide

CLI11 is a header-only command-line parser. C++11 minimum; `std::optional` support needs C++17.

Current release: **2.7.2**. Always pin a release tag, never `main`.

Include exactly one header: `#include <CLI/CLI.hpp>`. Everything else under `CLI/` is private.

## Is CLI11 the Right Choice?

- **Any non-trivial CLI** (subcommands, validation, config files, env fallback) → CLI11 is an
  excellent fit and has no dependencies.
- **Two or three flags, no validation** → hand-rolled `argv` loop is fine; CLI11 adds a ~10 s
  per-translation-unit compile cost header-only (measured). If you add it anyway, see
  [Compile Time](#compile-time).
- **Project already uses Boost.Program_options / gflags / getopt** → a port is mechanical but real.
  Mention the cost before migrating.
- **Need shell autocompletion or Python-argparse-style option abbreviation** → CLI11 deliberately
  does not do these. See [references/pitfalls.md](references/pitfalls.md).

## CMake Setup

```cmake
cmake_minimum_required(VERSION 3.14)
include(FetchContent)
FetchContent_Declare(cli11
  GIT_REPOSITORY https://github.com/CLIUtils/CLI11.git
  GIT_TAG        v2.7.2
  GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(cli11)

add_executable(my_app main.cpp)
target_link_libraries(my_app PRIVATE CLI11::CLI11)
```

The target is `CLI11::CLI11` (an `ALIAS`, so it works via `add_subdirectory` too). FetchContent
needs CMake 3.14+. Alternatives: `find_package(CLI11 CONFIG REQUIRED)` for vcpkg/Conan/system
installs, or drop the single-file `CLI11.hpp` from the release page into the tree.

CLI11 requests `cxx_std_11` as an INTERFACE compile feature — it will not downgrade a project
already on a newer standard.

### Compile Time

Header-only CLI11 costs roughly **10 s per translation unit** (measured, `-O1`). If more than one
TU includes it, build it precompiled instead:

```cmake
set(CLI11_PRECOMPILED ON)   # before FetchContent_MakeAvailable
```

and compile consumers with `-DCLI11_COMPILE`. Same measurement dropped to **2 s per TU**.
`CLI11_PRECOMPILED` is mutually exclusive with `CLI11_SINGLE_FILE`.

## Anatomy of a CLI11 Program

```cpp
#include <CLI/CLI.hpp>
#include <iostream>
#include <string>

int main(int argc, char **argv) {
    CLI::App app{"Resize an image", "resize"};   // {description, program_name}
    app.require_subcommand(0, 1);                 // optional; see references

    std::string input;
    std::string output = "out.png";               // set defaults BEFORE capture_default_str()
    int width = 0;

    app.add_option("input", input, "Source image")->required()->check(CLI::ExistingFile);
    app.add_option("-o,--output", output, "Destination")->capture_default_str();
    app.add_option("-w,--width", width, "Target width")->check(CLI::PositiveNumber);

    CLI11_PARSE(app, argc, argv);                 // never call parse() bare in main

    std::cout << input << " -> " << output << " @" << width << "\n";
    return 0;
}
```

Four moving parts, in this order every time:

1. Construct `CLI::App`.
2. Declare the destination variables **in a scope that outlives `parse()`**.
3. Register options/flags/subcommands, chaining modifiers.
4. `CLI11_PARSE(app, argc, argv)`, then read the variables.

`CLI11_PARSE` expands to `try { app.parse(...); } catch(const CLI::ParseError &e) { return app.exit(e); }`
— it is the only correct way to handle `--help`, which is *thrown* as `CLI::CallForHelp`.

## The Five Cardinal Rules

Most CLI11 bugs are lifetime or ordering mistakes, not syntax errors.

### 1. Bound variables must outlive `parse()`

`add_option(name, var, desc)` stores a lambda **capturing `var` by reference**. If `var` dies before
`parse()` runs, the write is use-after-scope. Verified with ASan: `stack-use-after-scope`,
`WRITE of size 4`, frame inside the `add_option` lambda. There is no diagnostic without a sanitizer.

```cpp
// WRONG — `n` is dead by the time parse() writes to it
void setup(CLI::App &app) { int n = 0; app.add_option("-n", n); }

// RIGHT — options are a view onto storage you own
struct Config { int n = 0; };
void setup(CLI::App &app, Config &cfg) { app.add_option("-n", cfg.n); }
```

Make the config struct a member or a `main`-scope local. Never a function-local in a setup helper.

### 2. Absent options leave the variable untouched

CLI11 does not zero or reset anything. "Default value" means "the value you initialized the variable
with". Verified: an unparsed `int n = 42` reads `42` after `parse()`.

Corollary: `capture_default_str()` **snapshots the variable at the moment you call it**, purely for
help text. Set the default first, register second:

```cpp
int n = 77;
app.add_option("-n", n)->capture_default_str();  // help shows [77]
// Calling capture_default_str() before `n = 77` would show [0]. Verified both ways.
```

Use `->default_val(x)` instead when you want CLI11 to actually *assign* the default.

### 3. `app.parse(std::vector<std::string>&)` expects the vector REVERSED

This overload is documented in the header as "Expects a reversed vector" and mutates it. Passing
args in natural order silently misparses — `{"-v","1","2","3"}` threw
`ArgumentMismatch: -v: 1 required INT missing`.

For tests and synthetic input, use the string overload, which tokenizes normally:

```cpp
app.parse(std::string("-v 1 2 3"));   // safe, no reversal
```

Reserve `parse(argc, argv)` for real `main`.

### 4. `app["--opt"]` is read-only; use `get_option` to modify

`operator[]` has only `const` overloads returning `const Option *`. `app["-e"]->required()` fails to
compile with *"passing 'const CLI::Option' as 'this' argument discards qualifiers"*. Use
`app.get_option("-e")->required()`. `get_option` is `[[nodiscard]]`, so don't discard the result.

`get_option` throws `CLI::OptionNotFound` on an unknown name; `get_option_no_throw` returns `nullptr`.

### 5. Repeated options *replace* vectors by default, and scalars *throw*

A `std::vector<int>` bound option pre-filled with `{9,9}` reads `{1,2,3}` after `--v 1 --v 2 --v 3`
— replaced, not appended. A scalar given twice throws `ArgumentMismatch` unless you pick a policy:
`->take_last()`, `->take_first()`, `->take_all()`, `->join()`. See
[references/api-reference.md](references/api-reference.md#multi-option-policy).

## Error Handling and Exit Codes

Everything CLI11 reports is an exception derived from `CLI::Error`. `app.exit(e)` prints the message
to `stderr` (or help to `stdout`) and returns the process exit code.

| Exception | Exit code (measured) | Cause |
|-----------|---------------------|-------|
| `CallForHelp` / `CallForVersion` | `0` | `--help`, `--version`. **Success, not failure.** |
| `OptionAlreadyAdded` | `102` | Duplicate name — thrown at *registration*, not parse |
| `ConversionError` | `104` | Value not convertible to the bound type |
| `ValidationError` | `105` | A `check()` rejected the value |
| `RequiredError` | `106` | Missing `required()` option, or `require_option` unsatisfied |
| `ExtrasError` | `109` | Unrecognized arguments remain |
| `ConfigError` | `110` | Config file could not be parsed / unknown key under `error` mode |
| `ArgumentMismatch` | `114` | Wrong number of values, incl. a scalar option given twice |

`CLI::ParseError` is the base for all of the above. `CLI::ConstructionError` (including
`OptionAlreadyAdded`) is **not** a `ParseError`, so `CLI11_PARSE` will not catch it — that is
intentional, since it signals a bug in your own setup code.

To inspect without exiting, catch explicitly:

```cpp
try {
    app.parse(argc, argv);
} catch (const CLI::ParseError &e) {
    return app.exit(e);                 // normal path
}
```

An `App` can be parsed more than once. `app.clear()` resets the recorded counts; without it the
bound variables still update correctly but stale state (e.g. `count()`) carries no reset point.
Measured: parse `-n 1` then `-n 2` without `clear()` succeeds and `n == 2`; `clear()` drops
`count("-n")` back to 0.

## Core API at a Glance

```cpp
app.add_option("-n,--num", var, "desc");       // one value, any convertible type
app.add_option("pos", var, "desc");            // no dash => positional
app.add_flag("-v,--verbose", count_or_bool);   // -vvv yields 3 on an int
app.add_flag("--tri,!--no-tri", flag);         // `!` prefix sets false
app.add_option_function<int>("-c", callback);  // callback(const int&)
app.add_flag_function("--loud", callback);     // callback(std::int64_t)
auto *sub = app.add_subcommand("add", "desc");
auto *grp = app.add_option_group("mode", "desc");
```

Common modifiers (all chainable, all return `Option *`):

`->required()` `->expected(n)` `->check(v)` `->transform(v)` `->envname("VAR")`
`->default_val(x)` `->capture_default_str()` `->needs(other)` `->excludes(other)`
`->each(fn)` `->group("Name")` `->group("")` (hide) `->option_text("N (count)")`
`->take_last()` `->delimiter(',')` `->allow_extra_args()`

Built-in validators: `CLI::ExistingFile`, `ExistingDirectory`, `ExistingPath`, `NonexistentPath`,
`CLI::Range(lo,hi)`, `CLI::PositiveNumber`, `CLI::NonNegativeNumber`, `CLI::IsMember({...})`,
`CLI::Transformer`, `CLI::CheckedTransformer`. Combine with `&`, `|`, `!`.

`check()` validates without mutating; `transform()` rewrites the value. With
`IsMember({"low","high"}, CLI::ignore_case)`, `check` leaves input `HIGH` as-is while `transform`
normalizes it to `high`. Verified both. If downstream code compares the string, you want `transform`.

`IsMember`, `Transformer` and `CheckedTransformer` live in `ExtraValidators.hpp` — on by default but
removed by `-DCLI11_DISABLE_EXTRA_VALIDATORS=1`. A second tier (`ReadPermissions`, `NonEmptyFile`,
...) is **off** by default and needs `-DCLI11_ENABLE_EXTRA_VALIDATORS=1`. If a validator name does
not resolve, check which tier it is in before assuming it was removed from the library.

## Verify Before You Ship

CLI11's behaviour around arity, policies and config files is easy to get subtly wrong. For anything
beyond a flag or a scalar option, compile and actually run it:

```bash
g++ -std=c++17 -Wall -Wextra -I<cli11>/include main.cpp -o app
./app --help          # confirm arity/type strings in the help are what you intended
./app <bad input>     # confirm the error message and exit code
echo $?
```

Check the generated help text specifically — it prints each option's real expected arity
(`INT`, `INT ...`, `INT x 2`), which is the fastest way to catch an `expected()`/`type_size()` mistake.

## Additional Resources

- Full API surface — every `add_*` overload, every modifier, supported types, validators, help
  formatting: [references/api-reference.md](references/api-reference.md)
- Subcommands, callbacks and ordering, config files, environment variables:
  [references/subcommands-and-config.md](references/subcommands-and-config.md)
- Complete compile-verified programs with measured output:
  [references/examples.md](references/examples.md)
- Footguns, documentation discrepancies, and what CLI11 cannot do:
  [references/pitfalls.md](references/pitfalls.md)
