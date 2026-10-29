# CLI11 Pitfalls, Quirks, and Limits

CLI11 2.7.2. Each item below was reproduced by compiling and running a probe program. Where CLI11's
own documentation disagrees with the measured behaviour, that is called out explicitly — trust the
measurement.

## Contents

- [Lifetime: the bug that does not warn](#lifetime-the-bug-that-does-not-warn)
- [parse(vector) wants the vector reversed](#parsevector-wants-the-vector-reversed)
- [fallthrough is inherited at creation time](#fallthrough-is-inherited-at-creation-time)
- [capture_default_str snapshots immediately](#capture_default_str-snapshots-immediately)
- [operator\[\] cannot configure an option](#operator-cannot-configure-an-option)
- [Unbound options have different arity than bound ones](#unbound-options-have-different-arity-than-bound-ones)
- [expected_max_vector_size is 1<<29, not 1<<30](#expected_max_vector_size-is-129-not-130)
- [Unknown config keys are ignored by default](#unknown-config-keys-are-ignored-by-default)
- [Config \[sub\] sections need ->configurable()](#config-sub-sections-need--configurable)
- [Dot notation does not count as "got subcommand"](#dot-notation-does-not-count-as-got-subcommand)
- [A configurable subcommand can fire final_callback twice](#a-configurable-subcommand-can-fire-final_callback-twice)
- [Config keys use dashes, and ignore_underscore does not help](#config-keys-use-dashes-and-ignore_underscore-does-not-help)
- [CheckedTransformer needs pairs; IsMember takes a flat set](#checkedtransformer-needs-pairs-ismember-takes-a-flat-set)
- [type_name vs option_text](#type_name-vs-option_text)
- [Option has no operator\* and no operator<<](#option-has-no-operator-and-no-operator)
- [get_option is nodiscard and throws](#get_option-is-nodiscard-and-throws)
- [Two different extra-validator gates](#two-different-extra-validator-gates)
- [ConstructionError escapes CLI11_PARSE](#constructionerror-escapes-cli11_parse)
- [--help is an exception with exit code 0](#--help-is-an-exception-with-exit-code-0)
- [Compile time](#compile-time)
- [What CLI11 will not do](#what-cli11-will-not-do)

## Lifetime: the bug that does not warn

`add_option(name, var, desc)` stores a lambda that captures `var` **by reference**. CLI11 never
copies the variable. If it dies before `parse()` runs, the write is undefined behaviour and there is
no compile-time or run-time diagnostic.

Reproduced under AddressSanitizer: `stack-use-after-scope`, `WRITE of size 4`, with the reported
frame inside the `add_option` lambda at `App.hpp:567`.

```cpp
// WRONG
void setup(CLI::App &app) {
    int n = 0;
    app.add_option("-n", n);     // n dies here; parse() writes to dead stack
}

// RIGHT
struct Config { int n = 0; };
void setup(CLI::App &app, Config &cfg) { app.add_option("-n", cfg.n); }
```

The habit that eliminates this whole class of bug: put every destination in **one struct declared in
`main`** (or owned by whatever object owns the `App`), and pass it by reference to any setup helper.
Every example in [examples.md](examples.md) does this.

Run new CLI11 code once under `-fsanitize=address` before trusting it.

## parse(vector) wants the vector reversed

```cpp
void parse(std::vector<std::string> &args);   // header: "Expects a reversed vector."
```

It also *mutates* the vector. Passing natural order silently misparses:

```cpp
std::vector<std::string> v{"-v", "1", "2", "3"};
app.parse(v);      // ArgumentMismatch: -v: 1 required INT missing
```

For tests and synthetic input use the `std::string` overload, which tokenizes normally:

```cpp
app.parse(std::string("-v 1 2 3"));
```

Reserve `parse(argc, argv)` for real `main`.

## fallthrough is inherited at creation time

`fallthrough_` is copied from the parent into each subcommand **when `add_subcommand` is called**.
Setting it on the parent afterwards has no effect on existing children.

```cpp
// WORKS
app.fallthrough();
auto *sub = app.add_subcommand("sub", "s");

// SILENTLY BROKEN — this is the version people write
auto *sub = app.add_subcommand("sub", "s");
app.fallthrough();

// WORKS
auto *sub = app.add_subcommand("sub", "s");
sub->fallthrough();
```

Verified: cases 1 and 3 accept `./app sub --flag -g 5` and print `flag=1 g=5`; case 2 throws
`sub: The following arguments were not expected: -g 5`.

The same creation-time inheritance applies to other App-level settings copied onto subcommands. As a
rule, configure the parent App fully *before* adding subcommands.

## capture_default_str snapshots immediately

`capture_default_str()` reads the bound variable **at the moment you call it** and stores the string
for help output only. It does not assign anything at parse time.

```cpp
int n = 0;
app.add_option("-n", n)->capture_default_str();
n = 99;                                     // help still shows [0]

int m = 77;
app.add_option("-m", m)->capture_default_str();   // help shows [77]
```

Both measured. Set your defaults first, register options second. If you want CLI11 to actually
*assign* a default, use `->default_val(x)` — verified to leave `n == 55` when the option is absent.

## operator[] cannot configure an option

`CLI::App::operator[]` has only `const` overloads returning `const Option *`:

```cpp
const Option *operator[](const std::string &) const;
const Option *operator[](const char *) const;
```

So `app["-e"]->required()` fails to compile: *"passing 'const CLI::Option' as 'this' argument
discards qualifiers"*. Use `app.get_option("-e")->required()`. Use `operator[]` only to read, e.g.
`app["-e"]->count()`.

## Unbound options have different arity than bound ones

Measured:

| Option | `expected` | `allow_extra_args` |
|--------|-----------|--------------------|
| `add_option("--v", std::vector<int>&)` | `[1, 536870912]` | `1` |
| `add_option("--multi")` (no storage) | `[1, 1]` | `0` |

To make an unbound option take unlimited values, only one thing works:

```cpp
app.add_option("--multi")->expected(CLI::detail::expected_max_vector_size);
```

Measured failures of the obvious alternatives:

- `->expected(0, -1)` → `ExtrasError: The following arguments were not expected: 1 2 3`
- `->allow_extra_args()` → `ArgumentMismatch: --multi: At most 1 required but received 3`

The CLI11 book states that either `expected(expected_max_vector_size)` *or* `allow_extra_args()`
suffices. For an unbound option that is not true.

## expected_max_vector_size is 1<<29, not 1<<30

`book/chapters/options.md` says *"The default for a vector is (1<<30)"*. The header disagrees:

```cpp
// StringTools.hpp:48
CLI11_MODULE_INLINE constexpr int expected_max_vector_size{1 << 29};
```

Printed at runtime: `536870912` = `1 << 29`. Use the named constant, never a literal.

## Unknown config keys are ignored by default

```cpp
// App.hpp:136
ConfigExtrasMode allow_config_extras_{ConfigExtrasMode::Ignore};
```

A typo in a config file is **not** an error out of the box. Verified: a program defining only `-g`
loaded a config containing `name`, `count` and `vec` and exited 0. For a tool where the config file
is the primary interface this hides real misconfiguration. Opt in to strictness:

```cpp
app.allow_config_extras(CLI::config_extras_mode::error);
```

Then a bad key gives `INI was not able to parse hostt` with exit code **110**.

`CLI::config_extras_mode` values: `error`, `ignore`, `ignore_all`, `capture`.

## Config [sub] sections need ->configurable()

Given

```toml
[sub]
flag = true
```

| Subcommand | `flag` | `got_subcommand("sub")` |
|------------|--------|-------------------------|
| plain | `1` | **`0`** |
| `->configurable()` | `1` | `1` |

Both measured. Without `configurable()` the section's keys are applied as *defaults* to the
subcommand's options, but the subcommand is not considered given — so its `->callback()` **never
fires**. If your design is "every subcommand has a callback" and you also support config files,
every configurable subcommand must be marked.

## Dot notation does not count as "got subcommand"

`./app --sub.flag` sets the subcommand's option without entering the subcommand. Measured:
`flag=1 got_sub=0`. Convenient for scripting and config round-trips, but the subcommand's
`->callback()` does not run. Do not treat dot notation as equivalent to `./app sub --flag`.

## A configurable subcommand can fire final_callback twice

Repro: a `->configurable()` subcommand that has **both** a `parse_complete_callback` and a
`final_callback`, activated from a config file rather than the command line.

```
$ ./q16 --config sub.toml          # [sub] flag = true
  sub.parse_complete #1
  sub.final #1
app.parse_complete
  sub.final #2
pc=1 fc=2

$ ./q16 sub --flag                 # same program, command line
  sub.parse_complete #1
app.parse_complete
  sub.final #1
pc=1 fc=1
```

With no `parse_complete_callback` set, the config path fires `final` once. So the duplicate is
specific to config activation plus a parse-complete callback.

Consequence: a subcommand callback that performs a side effect (writes a file, sends a request)
can run twice. Make such callbacks idempotent, or guard with a flag, or avoid combining
`parse_complete_callback` with `configurable()`.

## Config keys use dashes, and ignore_underscore does not help

The config key is the option's long name with leading dashes stripped — so `--log-level` is
`log-level`, not `log_level`. Measured: `log_level = "warn"` fails with
`INI was not able to parse log_level` (rc=110), **and the error is identical with
`app.ignore_underscore()` set**.

To accept both spellings, declare both names on the option:

```cpp
app.add_option("--log-level,--log_level", level, "Verbosity");
```

Verified: `log_level = "warn"` then loads.

Related: an option you do not want appearing in `config_to_str()` output (such as `--save-config`
itself) needs `->configurable(false)`. Verified both with and without.

## CheckedTransformer needs pairs; IsMember takes a flat set

```cpp
// COMPILE ERROR: static assertion failed: mapping must produce value pairs
->transform(CLI::CheckedTransformer(std::vector<std::string>{"fast","slow"}, CLI::ignore_case))

// Correct for a flat allow-list (validate + normalize case)
->transform(CLI::IsMember({"fast","slow"}, CLI::ignore_case))

// Correct for a mapping (string -> value)
->transform(CLI::CheckedTransformer(std::map<std::string,int>{{"fast",0},{"slow",1}},
                                    CLI::ignore_case))
```

Also note `Transformer` vs `CheckedTransformer`: `Transformer` passes unknown values **through**
unchanged (`-m a` → `alpha`, `-m zz` → `zz`), while `CheckedTransformer` rejects them
(`Check zz value in {a->alpha} OR {alpha} FAILED`). For an allow-list you almost always want the
checked form.

And `check` vs `transform` on the same validator: with `IsMember({"low","high"}, ignore_case)`,
`check` leaves input `HIGH` as `HIGH`, while `transform` rewrites it to `high`. If downstream code
compares the string, you want `transform`.

## type_name vs option_text

`->type_name(s)` replaces only the *base type* token; a validator's description is still appended
after a colon. `->option_text(s)` replaces the whole type column.

Measured on an enum option with a `CheckedTransformer`:

| Setting | Help column |
|---------|-------------|
| (none) | `--format ENUM:value in {csv->1,json->0,table->2} OR {1,0,2}` |
| `type_name("json\|csv\|table")` | `--format json\|csv\|table:value in {csv->1,json->0,table->2} OR {1,0,2}` |
| `option_text("json\|csv\|table")` | `--format json\|csv\|table` |

Neither affects validation, and neither changes the *error* message, which still prints the raw map.
Keep enum key names short for that reason.

## Option has no operator* and no operator<<

`*app.get_option("-u")` dereferences the pointer to an `Option`, which has no stream insertion
operator — the result does not compile (44 candidate `operator<<` overloads, none viable). Use:

```cpp
app.get_option("-u")->as<std::string>();     // first value, converted
std::vector<int> v; app.get_option("-u")->results(v);   // all values
```

Note `as<T>()` on a multi-value option returns the **first** value: measured `as<int>() == 4` for
`-u 4 5 6`, while `results(v)` filled all three.

## get_option is nodiscard and throws

```cpp
app.get_option("--nope");          // -Wall: warning, result discarded (CLI11_NODISCARD)
app.get_option("--nope");          // throws CLI::OptionNotFound if the name is unknown
app.get_option_no_throw("--nope"); // returns nullptr instead
```

`CLI::OptionNotFound` derives from `CLI::Error`, **not** from `ParseError`, so `CLI11_PARSE` will not
catch it.

## Two different extra-validator gates

`include/CLI/ExtraValidators.hpp` has two tiers, and they behave differently:

**Tier 1 — on by default, removable.** `IsMember`, `Transformer`, `CheckedTransformer`, `Bound`,
`Number`, `ValidIPV4`, `AsNumberWithUnit`, `AsSizeValue`.

Compiling with `-DCLI11_DISABLE_EXTRA_VALIDATORS=1` makes these vanish:
`error: 'IsMember' is not a member of 'CLI'; did you mean 'IsMemberType'?` (verified).

**Tier 2 — off by default, opt-in.** `ReadPermissions`, `WritePermissions`, `ExecPermissions`,
`NonEmptyFile`. These are inside a `#if defined(CLI11_ENABLE_EXTRA_VALIDATORS) && ... != 0` block.

Verified: `CLI::ReadPermissions` gives `error: 'ReadPermissions' is not a member of 'CLI'` with no
flags, and compiles cleanly with `-DCLI11_ENABLE_EXTRA_VALIDATORS=1`.

So if a validator name does not resolve, check which tier it is in before concluding it was removed
from the library.

## ConstructionError escapes CLI11_PARSE

```
CLI::Error
├── CLI::ConstructionError      <- NOT caught by CLI11_PARSE
└── CLI::ParseError             <- caught
```

`OptionAlreadyAdded` (exit code 102), `BadNameString`, and `IncorrectConstruction` are
`ConstructionError`s thrown while you are *building* the parser, before any user input is seen. That
they escape `CLI11_PARSE` is intentional: they signal a bug in your own setup code, not bad input.
Registering `"-n"` twice terminates the process (measured, exit **134**):

```
terminate called after throwing an instance of 'CLI::OptionAlreadyAdded'
  what():  added option matched existing option name: n is already added
```

This is a crash at startup, so it shows up the first time you run the binary — but only if that code
path executes. Options registered inside a conditional branch can hide the duplicate until later.

## --help is an exception with exit code 0

`--help` throws `CLI::CallForHelp`, a subclass of `CLI::Success`, which is a `ParseError`. A bare
`app.parse(argc, argv)` with no handler therefore aborts on `--help` (measured, exit **134**):

```
terminate called after throwing an instance of 'CLI::CallForHelp'
  what():  This should be caught in your main function, see examples
```

This is the single most common reason to always use `CLI11_PARSE`.

`app.exit(e)` prints help to **stdout** (not stderr) and returns **0** for `CallForHelp`,
`CallForAllHelp` and `CallForVersion` — verified. Treat those exit codes as success in scripts and
tests.

## Compile time

Header-only CLI11 measured **~10.6 s** per translation unit at `-O1` with GCC. For a project with
several TUs including `CLI/CLI.hpp`, build it precompiled instead:

```cmake
set(CLI11_PRECOMPILED ON)
```

and compile consumers with `-DCLI11_COMPILE`. Same measurement: **~2.0 s** per TU. That is how every
probe in this guide was built.

`CLI11_PRECOMPILED` is mutually exclusive with `CLI11_SINGLE_FILE`. The single-file `CLI11.hpp` from
the release page carries the full header-only cost.

Practical alternative for a large codebase: confine CLI11 to one `cli.cpp` that fills a plain
options struct, and let the rest of the project include only that struct's header.

## What CLI11 will not do

These are explicit non-goals from the project's README, not oversights:

- **Shell autocompletion.** Not implemented, with no committed plan. If you need bash/zsh/fish
  completion you must generate the script yourself — walk `app.get_options()` (returns all
  registered options; measured size 2 for two registrations) and emit a completion file. There is no
  built-in helper. Watch out: the no-argument `app.get_subcommands()` returns
  `parsed_subcommands_`, i.e. only what the *current* invocation matched — useless before parsing.
  Use the filtered overload `get_subcommands(std::function<bool(App*)>)` to enumerate all of them.
- **Option abbreviation.** CLI11 will not accept `--verb` for `--verbose`; the README's reasoning is
  *"It's better not to guess."* The only opt-in prefix feature is
  `allow_subcommand_prefix_matching()`, which applies to **subcommand names only** and requires the
  prefix to be unambiguous. Verified: `./app rem add origin` resolves to `remote add origin`.
- **Close-match suggestions** are not automatic. `examples/close_match.cpp` in the repo shows how to
  build "did you mean …?" yourself.

Other gaps worth knowing before you commit:

- **No i18n.** Error strings and help labels are English literals. `formatter->label(key, value)`
  retranslates help labels; error messages require catching each exception type and rewriting the
  text yourself.
- **Non-standard option names** (`-option`, single dash with a long name) need an explicit
  `app.allow_non_standard_option_names()`. Windows `/opt` style needs
  `app.allow_windows_style_options()`.
- **No built-in man page or docs generation.** Derive from `CLI::Formatter` if you want roff or
  Markdown output.
- **Thread safety is not claimed.** Build and parse the `App` on one thread before handing the
  resulting options struct to workers.
