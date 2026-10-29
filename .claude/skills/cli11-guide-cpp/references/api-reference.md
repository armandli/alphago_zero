# CLI11 API Reference

CLI11 2.7.2. Everything here was read from `include/CLI/{App,Option,Validators,Error,StringTools}.hpp`
and confirmed by compiling and running.

## Contents

- [App construction](#app-construction)
- [Registering options](#registering-options)
- [Flags](#flags)
- [Positionals](#positionals)
- [Supported types](#supported-types)
- [Option modifiers](#option-modifiers)
- [Arity: expected, type_size, allow_extra_args](#arity-expected-type_size-allow_extra_args)
- [Multi-option policy](#multi-option-policy)
- [Validators](#validators)
- [Custom types](#custom-types)
- [Custom validators](#custom-validators)
- [Option groups](#option-groups)
- [Parse overloads](#parse-overloads)
- [Reading results after parse](#reading-results-after-parse)
- [App-level modifiers](#app-level-modifiers)
- [Help and formatting](#help-and-formatting)
- [Errors](#errors)

## App construction

```cpp
CLI::App app;                                 // no description, no name
CLI::App app{"Description shown atop help"};
CLI::App app{"Description", "progname"};      // name also used in the Usage: line
```

If the name is omitted the usage line reads `Usage: [OPTIONS]` with no program name — verified.
`argv[0]` is **not** used to fill it in automatically.

## Registering options

| Overload | Use |
|----------|-----|
| `add_option(name, var, desc="")` | The workhorse. Binds `var` by reference. |
| `add_option(name)` | Declares the option with no storage. Read results via `->results()`. |
| `add_option(name, desc)` where `desc` is a `const char*` lvalue | Name + description, no storage. |
| `add_option<AssignTo, ConvertTo>(name, var, desc)` | Parse as `ConvertTo`, assign to `AssignTo`. Used for enums and narrowing. |
| `add_option_function<ArgType>(name, fn, desc="")` | `fn` is `void(const ArgType&)`, called once per parsed set. |
| `add_option(name, callback_t cb, desc, defaulted, str_fn)` | Raw form; `cb` is `bool(const std::vector<std::string>&)`. |
| `add_option_no_stream(name, var, desc)` | Same as `add_option` but never calls `operator<<` on `var`. Use for types with no stream output. |

Names are comma-separated: `"-n,--num"`. Any number of short and long forms. Order does not matter;
the *longest* name is used in help.

```cpp
int n{};
app.add_option("-n,--num,--number", n, "A number");
```

`add_option` returns `Option *`. Registering a name that already exists throws
`CLI::OptionAlreadyAdded` **at registration time** (exit code 102) — this is a `ConstructionError`,
not a `ParseError`, so `CLI11_PARSE` will not catch it.

## Flags

A flag takes no value. Bind it to `bool`, any integer, or a container.

```cpp
bool b{};      app.add_flag("-q,--quiet", b);
int  v{};      app.add_flag("-v,--verbose", v);   // -vvv => 3 (verified)
bool t{true};  app.add_flag("--tri,!--no-tri", t);  // --no-tri sets false (verified)
app.add_flag("--standalone", "just a description, no storage");
app.add_flag_function("--loud", [](std::int64_t n){ /* n = repeat count */ });
app.add_flag_callback("--now", []{ /* void() */ });
```

The `!` prefix marks a negating alias. `add_flag_function`'s callback signature is
`void(std::int64_t)` — the count, verified as `1` for a single `--loud`.

Flags can accept an explicit value with `=`: `--verbose=3`. Binding a flag to a `std::string` or
non-counting type and passing `-vv` is an error.

## Positionals

A name with no leading dash is positional. Declaration order is matching order.

```cpp
std::string in, out;
std::vector<std::string> rest;
app.add_option("input", in, "input file")->required();
app.add_option("output", out, "output file");
app.add_option("extras", rest, "everything else");
```

Produces this usage line and help block (measured):

```
q2 [OPTIONS] input [output] [extras...]

POSITIONALS:
  input TEXT REQUIRED         input file
  output TEXT                 output file
  extras TEXT ...             the rest
```

`--` ends option processing; everything after it becomes positional. Verified: with one scalar and
one vector positional, `prog a -- -b --c` yields `first=a rest=[-b,--c]`.

## Supported types

Conversion is done by `detail::lexical_cast` / `lexical_conversion`. All of the following were
compiled and run successfully:

| Category | Types |
|----------|-------|
| Scalars | `bool`, all integer types, `float`, `double`, `std::string` |
| Optional | `std::optional<T>` (C++17), `std::nullopt` when absent |
| Sequences | `std::vector<T>`, `std::set<T>` (deduplicates — `-s 3 -s 1 -s 3` gave size 2), `std::array<T,N>` (fixed arity: `-a 7 8 9`), most standard containers |
| Associative | `std::map<K,V>`, `std::unordered_map<K,V>` — consumes two tokens per entry |
| Tuples | `std::pair<A,B>`, `std::tuple<...>` — consumes `sizeof...` tokens |
| Enums | via a `CLI::CheckedTransformer` map (preferred), or `add_option<Enum, int>` |
| Custom | any type with `std::istream &operator>>` |

Enums, verified end to end:

```cpp
enum class Lvl : int { Low = 0, High = 1 };
Lvl lvl{Lvl::Low};
app.add_option("--lvl", lvl)->transform(
    CLI::CheckedTransformer(std::map<std::string, Lvl>{{"low", Lvl::Low}, {"high", Lvl::High}},
                            CLI::ignore_case));
// --lvl HIGH  =>  lvl == Lvl::High
```

Measured round-trip with `--opt 5 --map a 1 --map b 2 --pair k 9 --tup 1 2.5 hi`:

```
opt=5
map: a=1 b=2
pair=k/9
tup=1,2.5,hi
```

Note `--map` repeated twice *accumulated* into the map, whereas a repeated `std::vector` option
**replaces** (see the cardinal rules in SKILL.md).

## Option modifiers

Every modifier returns `Option *`, so they chain. Grouped by purpose.

### Requirement and dependency

| Modifier | Effect |
|----------|--------|
| `required(bool = true)` | Missing it throws `RequiredError` (106). |
| `needs(Option*)` / `needs("name")` | This option requires the other to also be present. |
| `excludes(Option*)` / `excludes("name")` | Mutually exclusive. |
| `envname("VAR")` | Falls back to the environment variable. A CLI arg overrides it — verified. |
| `ignore_case()` / `ignore_underscore()` | Name matching leniency, per-option. |

### Defaults

| Modifier | Effect |
|----------|--------|
| `capture_default_str()` | Snapshot the bound variable's current value **as help text only**. Call it *after* setting the default. |
| `default_str(std::string)` | Set the help-text default manually. |
| `default_val(T)` | Actually assign the value if the option is absent. Verified: reads `55`. |
| `default_function(fn)` | Supply the default-string generator. |
| `force_callback()` | Run the option's callback even when the option is absent. |
| `run_callback_for_default()` | Internal-ish; set automatically by `add_option`. |

### Values and conversion

| Modifier | Effect |
|----------|--------|
| `check(Validator)` | Validate without modifying. |
| `transform(Validator)` | Validate and rewrite the value. |
| `each(fn)` | Declared `void(std::string)` (by value). Called once per value *during* parse — verified firing per `--tag`. |
| `delimiter(char)` | Split a single token: `->delimiter(',')` turns `-v 1,2,3` into 3 values (verified). |
| `join()` / `join(char delim)` | `MultiOptionPolicy::Join`; the one-arg form also sets the delimiter. |
| `type_name(std::string)` | Override the type shown in help. |
| `type_size(min, max)` | Tokens consumed per occurrence. |

### Cardinality and repetition

See [Arity](#arity-expected-type_size-allow_extra_args) and
[Multi-option policy](#multi-option-policy).

### Presentation

| Modifier | Effect |
|----------|--------|
| `group("Name")` | Put the option under a named help section. |
| `group("")` | **Hide** the option from help entirely — verified. |
| `option_text("N (count)")` | Replace the type string in help. Verified output: `--custom N (count)  custom text`. |
| `description(std::string)` | Change the description after the fact. |

### Lifecycle

| Modifier | Effect |
|----------|--------|
| `trigger_on_parse()` | Run the callback immediately when parsed, not at the end. |
| `callback_priority(CLI::CallbackPriority)` | Order relative to other callbacks. Values: `FirstPreHelp`, `First`, `PreRequirementsCheckPreHelp`, `PreRequirementsCheck`, `NormalPreHelp`, `Normal`, `LastPreHelp`, `Last`. |

## Arity: expected, type_size, allow_extra_args

Three separate knobs; confusing them is the most common CLI11 bug.

- **`type_size(min, max)`** — tokens consumed by *one* occurrence. Derived from the bound type.
- **`expected(min, max)`** — how many *occurrences* are allowed.
- **`allow_extra_args()`** — permits an occurrence to swallow more tokens than `type_size`.

For a bound `std::vector<int>`, `add_option` sets `expected=[1, 536870912]` and
`allow_extra=1` automatically (measured). For an *unbound* `add_option("--multi")`,
it sets `expected=[1,1]`, `allow_extra=0`.

That asymmetry matters. To make an **unbound** option take unlimited values:

```cpp
app.add_option("--multi")->expected(CLI::detail::expected_max_vector_size);
```

`expected_max_vector_size` is `1 << 29` = **536870912** (verified; `StringTools.hpp:48`).
Measured failures on the way to that line:

- `->expected(0, -1)` → `ExtrasError: The following arguments were not expected: 1 2 3`
- `->allow_extra_args()` alone → `ArgumentMismatch: --multi: At most 1 required but received 3`

The CLI11 book claims either `expected(expected_max_vector_size)` *or* `allow_extra_args()` works.
For an unbound option only the former does.

The fastest way to confirm arity is `--help`: it prints `INT`, `INT ...`, or `INT x 2`.

## Multi-option policy

What happens when an option appears more than once. Default for scalars is **Throw**.

```cpp
app.add_option("-n", n)->take_last();   // or take_first(), take_all(), join()
app.add_option("-n", n)->multi_option_policy(CLI::MultiOptionPolicy::Sum);
```

`CLI::MultiOptionPolicy` values: `Throw` (default), `TakeLast`, `TakeFirst`, `Join`, `TakeAll`,
`Sum`, `Reverse`.

Measured on `-n 1 -n 2` (and `-n 1 -n 2 -n 4` for Sum):

| Policy | Result |
|--------|--------|
| default (`Throw`) | `ArgumentMismatch: -n: At most 1 required but received 2`, exit **114** |
| `take_last()` | `n == 2` |
| `Sum` | `n == 7` |

## Validators

`check()` runs the validator and rejects on a non-empty return. `transform()` additionally writes
the validator's modified string back.

### File system

`CLI::ExistingFile`, `CLI::ExistingDirectory`, `CLI::ExistingPath`, `CLI::NonexistentPath`,
`CLI::FileOnDefaultPath("/some/dir")`.

### Numeric

`CLI::Range(lo, hi)` (inclusive), `CLI::Range(hi)` (0..hi), `CLI::PositiveNumber`,
`CLI::NonNegativeNumber`.

### Set membership and mapping

```cpp
app.add_option("--lvl", lvl)->check(CLI::IsMember({"low", "high"}));
app.add_option("--lvl", lvl)->transform(CLI::IsMember({"low","high"}, CLI::ignore_case));
app.add_option("--mode", m)->transform(CLI::Transformer({{"a","alpha"}}));
app.add_option("--mode", m)->transform(CLI::CheckedTransformer({{"a","alpha"}}));
```

Measured behaviour:

- `IsMember` sets the help type string to `TEXT:{low,high}`.
- Failure message: `--lvl: mid not in {low,high}`.
- With `ignore_case`: `check` leaves input `HIGH` unchanged; `transform` normalizes it to `low`/`high`.
- `Transformer` maps known keys and **passes unknown values through** → `-m a` gives `alpha`.
- `CheckedTransformer` rejects unknown values:
  `-m: Check zz value in {a->alpha} OR {alpha} FAILED`.

### String handling

`CLI::EscapedString` — a transformer that decodes escape sequences in the value.

### Combining

```cpp
->check(CLI::Range(1, 10) & !CLI::Range(5, 6))
```

`&` = both, `|` = either, `!` = negate. Measured on the above: `-n 5` throws
`-n: check INT in [5 - 6] succeeded improperly`; `-n 7` passes.

Also available: `->application_index(i)` on a Validator restricts it to the *i*-th value of a
multi-value option (`-1` = all).

Some validators are behind `CLI11_ENABLE_EXTRA_VALIDATORS` / excluded by
`CLI11_DISABLE_EXTRA_VALIDATORS`. If a validator name does not resolve, check that gate before
assuming it was removed.

## Custom types

Provide `operator>>`. Set the failbit to signal a parse error — CLI11 turns that into a
`ConversionError`.

```cpp
struct Point { int x{0}, y{0}; };

std::istream &operator>>(std::istream &is, Point &p) {
    char comma{};
    is >> p.x >> comma >> p.y;
    if (comma != ',') is.setstate(std::ios::failbit);
    return is;
}
std::ostream &operator<<(std::ostream &os, const Point &p) { return os << p.x << ',' << p.y; }

Point p;
app.add_option("--pt", p, "a point as X,Y");   // --pt 3,4  =>  p = {3,4}
```

Verified: `--pt 3,4` gives `3,4`. The help type string is `TEXT` — override with
`->type_name("X,Y")` for clarity. `operator<<` is needed only if you call `capture_default_str()`;
otherwise use `add_option_no_stream`.

## Custom validators

A `CLI::Validator` is a `std::string(std::string&)` — return an empty string for OK, an error
message otherwise. Second constructor argument is the type-name shown in help.

```cpp
auto Even = CLI::Validator(
    [](std::string &s) -> std::string {
        int v{};
        if (!CLI::detail::lexical_cast(s, v)) return "not an integer";
        return (v % 2 == 0) ? std::string{} : std::string("must be even");
    },
    "EVEN");

app.add_option("--even", even)->check(Even);
```

Measured: help shows `--even INT:EVEN`; `--even 7` throws `--even: must be even`.

Mutating the `std::string&` is how a *transformer* works. Call `->non_modifying()` on the Validator
if it must never rewrite the input.

## Option groups

An option group is an `App` with no name — it participates in help layout and requirement counting
but is not a subcommand.

```cpp
auto *grp = app.add_option_group("mode", "exactly one mode");
grp->add_flag("--fast", fast);
grp->add_flag("--slow", slow);
grp->require_option(1);          // exactly 1
```

Measured failure: `Exactly 1 option from [--fast,--slow] is required`, exit code **106**.

`require_option` forms: `require_option()` (exactly 1), `require_option(n)` (exactly n),
`require_option(min, max)`, `require_option(-n)` (**at most** n). Measured for `require_option(-1)`
with two options supplied: `Requires at most 1 options be given from [-x,-y]`, exit code 106.

Help renders the group as:

```
[Option Group: mode]
  exactly one mode
  [Exactly 1 of the following options are required]
```

## Parse overloads

```cpp
void parse(int argc, const char *const *argv);   // real main — use this
void parse(std::string commandline, bool program_name_included = false);
void parse(std::vector<std::string> &args);      // REVERSED, and mutated
void parse(std::vector<std::string> &&args);
```

The vector overload is documented in the header as *"Expects a reversed vector. Changes the vector
to the remaining options."* Use the `std::string` overload for tests — it tokenizes normally and was
used for every measurement in this guide.

`app.clear()` resets parse state so the same `App` can parse again (verified).

## Reading results after parse

```cpp
app.count("--opt");                     // occurrences
app.get_option("--opt")->count();
app.get_option("--opt")->as<int>();     // convert; for a multi-value option returns the FIRST
std::vector<int> v; app.get_option("--opt")->results(v);   // all values
app.get_option("--opt")->empty();
app.remaining();                        // leftover args (needs allow_extras())
app.remaining_size();
```

Measured on `-u 4 5 6` with an unbound `expected(expected_max_vector_size)` option:
`count=3`, `as<int>()=4`, `results(v)` filled 3 elements.

`CLI::Option` has **no** `operator*` and no `operator<<` — `*app.get_option("-u")` does not yield a
string, it fails to compile. Use `as<std::string>()` or `results()`.

`get_option` throws `CLI::OptionNotFound` for an unknown name; `get_option_no_throw` returns
`nullptr`. `get_option` is `[[nodiscard]]` — discarding the return warns under `-Wall`.

`operator[]` is **const-only**: use it to *read* (`app["--opt"]->count()`), never to configure.

## App-level modifiers

| Method | Effect |
|--------|--------|
| `allow_extras(bool = true)` | Don't throw on unknown args; retrieve via `remaining()`. Verified: `[leftover,--unknown,5]`. |
| `prefix_command()` | Stop parsing at the first unrecognized token; the rest goes to `remaining()`. |
| `ignore_case()` / `ignore_underscore()` | Lenient name matching for this App and its options. |
| `allow_windows_style_options(bool)` | Accept `/opt` style. |
| `allow_non_standard_option_names()` | Accept `-option` (single dash, long name). |
| `allow_config_extras(CLI::config_extras_mode)` | How to treat unknown config-file keys. |
| `positionals_at_end()` | Positionals may only appear after all options. |
| `validate_positionals()` | Try validators when deciding which positional a token belongs to. |
| `require_option(...)` / `require_subcommand(...)` | Counting requirements. |
| `footer(std::string)` | Text appended after help. Verified. |
| `name(std::string)` / `description(std::string)` | Mutate after construction. |

## Help and formatting

```cpp
app.set_help_flag("-h,--help", "Print help");     // "" removes it
app.set_help_all_flag("--help-all", "all help");  // includes subcommand options
app.set_version_flag("-V,--version", "1.2.3");    // or a std::function<std::string()>
app.get_formatter()->column_width(28);
app.get_formatter()->label("REQUIRED", "(must)");
app.footer("See docs at example.com");
std::string text = app.help();                    // render without exiting
```

`--version` printed `1.2.3` and exited **0** (verified). `--help` throws `CLI::CallForHelp`, which
`app.exit(e)` renders to `stdout` and returns **0** for — help is *success*.

For full control, derive from `CLI::Formatter` and override `make_option`, `make_group`,
`make_usage`, etc., then `app.formatter(std::make_shared<MyFormatter>())`.

## Errors

```
CLI::Error
├── CLI::ConstructionError         (your setup is wrong — NOT caught by CLI11_PARSE)
│   ├── IncorrectConstruction
│   ├── BadNameString
│   ├── OptionAlreadyAdded         102
│   └── ...
├── CLI::ParseError                (user input is wrong — caught by CLI11_PARSE)
│   ├── Success
│   │   ├── CallForHelp              0
│   │   ├── CallForAllHelp           0
│   │   └── CallForVersion           0
│   ├── RuntimeError
│   ├── FileError
│   ├── ConversionError            104
│   ├── ValidationError            105
│   ├── RequiredError              106
│   ├── ArgumentMismatch           114
│   ├── RequiresError
│   ├── ExcludesError
│   ├── ExtrasError                109
│   ├── ConfigError                110
│   ├── InvalidError
│   └── HorribleError
└── CLI::OptionNotFound            (thrown by get_option; sibling of ParseError)
```

Exit codes above were measured with `echo $?`. Read `e.get_exit_code()` for the numeric value and
`e.get_name()` for the class name. `app.exit(e, out, err)` lets you redirect the two streams.
