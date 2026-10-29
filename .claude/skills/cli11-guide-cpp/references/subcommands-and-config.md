# CLI11 Subcommands, Callbacks, Environment and Config Files

CLI11 2.7.2. Every output block below is measured from a compiled program.

## Contents

- [Subcommands](#subcommands)
- [Nested subcommands](#nested-subcommands)
- [Requiring subcommands](#requiring-subcommands)
- [fallthrough: the inheritance trap](#fallthrough-the-inheritance-trap)
- [Prefix matching and pass-through](#prefix-matching-and-pass-through)
- [Subcommand modifiers](#subcommand-modifiers)
- [Callbacks and ordering](#callbacks-and-ordering)
- [Environment variables](#environment-variables)
- [Config files](#config-files)
- [Writing a config file out](#writing-a-config-file-out)
- [Customizing the config format](#customizing-the-config-format)

## Subcommands

A subcommand is a full `CLI::App` with its own options, help, and callback.

```cpp
CLI::App app{"git-like tool", "tool"};
auto *add = app.add_subcommand("add", "Add files");
std::string path;
add->add_option("path", path, "What to add")->required();
add->callback([&]{ std::cout << "adding " << path << "\n"; });
```

`add_subcommand` returns a non-owning `App *`; the parent owns the subcommand. There is also
`add_subcommand(CLI::App_p)` taking a `shared_ptr<App>` if you want to build it elsewhere.

Checking what ran, after `parse()`:

```cpp
app.got_subcommand("add");        // bool, by name
app.got_subcommand(add);          // bool, by pointer
static_cast<bool>(*add);          // App has an explicit operator bool == parsed_ > 0
app.get_subcommands();            // std::vector<App*> of the ones that were parsed, in order
```

Prefer a `->callback()` per subcommand over a chain of `if (app.got_subcommand(...))` — the callback
runs with the subcommand's own options already populated and validated.

## Nested subcommands

Nest arbitrarily deep by calling `add_subcommand` on a subcommand.

```cpp
auto *remote = app.add_subcommand("remote", "manage remotes");
remote->require_subcommand(1);
auto *radd = remote->add_subcommand("add", "add a remote");
std::string rn;
radd->add_option("name", rn)->required();
radd->callback([&]{ std::cout << "remote add " << rn << "\n"; });
```

```
$ ./q12 remote add origin
remote add origin
```

## Requiring subcommands

```cpp
app.require_subcommand();          // exactly 1
app.require_subcommand(1);         // exactly 1
app.require_subcommand(0, 1);      // at most 1  (verified OK with zero given)
app.require_subcommand(1, 3);      // between 1 and 3
app.require_subcommand(-2);        // at most 2
```

Measured failure with `require_subcommand(1)` and nothing given:

```
A subcommand is required
Run with --help for more information.
rc=106
```

## fallthrough: the inheritance trap

By default an option declared on the *parent* cannot appear after the subcommand name. This fails:

```
$ ./app sub --flag -g 5
sub: The following arguments were not expected: -g 5
```

`fallthrough()` fixes it — but **the flag is read on the subcommand and is copied from the parent at
`add_subcommand()` time**. Calling `app.fallthrough()` after the subcommands already exist does
nothing. Both of these work; the broken middle case is the one people write.

```cpp
// WORKS — parent set before children are created
app.fallthrough();
auto *sub = app.add_subcommand("sub", "s");

// BROKEN — children already copied fallthrough_ = false
auto *sub = app.add_subcommand("sub", "s");
app.fallthrough();

// WORKS — set directly on the child, any time
auto *sub = app.add_subcommand("sub", "s");
sub->fallthrough();
```

Verified: the first and third print `flag=1 g=5`; the middle one throws `ExtrasError`.

Related: `subcommand_fallthrough(bool)` controls whether *sibling subcommand names* on the parent are
recognized while inside a subcommand.

Two other ways to get back to the parent's options without fallthrough:

- `++` — explicit subcommand terminator. `./app sub --flag ++ -g 9` gave `flag=1 got_sub=1 g=9`.
- Dot notation — `./app --sub.flag` sets the subcommand's option **without entering it**. Measured:
  `flag=1 got_sub=0`. Useful for config/scripting; note `got_subcommand` stays false, so a
  `->callback()` will not fire.

## Prefix matching and pass-through

```cpp
app.allow_subcommand_prefix_matching();
```

Any unambiguous prefix of a subcommand name is accepted. Verified: `./q12 rem add origin` resolved
to `remote add origin`. Ambiguous prefixes are an error, not a guess — CLI11 deliberately does not do
argparse-style option abbreviation, only this opt-in subcommand form.

To hand the rest of the command line to another program:

```cpp
auto *exec = app.add_subcommand("exec", "run a command")->prefix_command();
exec->callback([&]{ for (const auto &s : exec->remaining()) /* ... */ ; });
```

```
$ ./q12 exec ls -la /tmp
exec remaining: ls -la /tmp
```

`prefix_command()` stops parsing at the first unrecognized token. `remaining()` returns everything
from there on. Use `remaining(true)` to recurse into subcommands.

## Subcommand modifiers

| Modifier | Effect |
|----------|--------|
| `require_subcommand(...)` | See above. |
| `fallthrough(bool)` | Unrecognized options fall through to the parent. Inherited at creation. |
| `subcommand_fallthrough(bool)` | Sibling subcommand names are recognized from inside. |
| `prefix_command(bool)` | Stop parsing at the first unrecognized token. |
| `allow_subcommand_prefix_matching(bool)` | Accept unambiguous name prefixes. |
| `silent(bool)` | Parsed but excluded from `get_subcommands()` — for "modifier" subcommands. |
| `configurable(bool)` | Allow the config file to *enter* this subcommand. See below. |
| `disabled(bool)` | Remove from parsing and help. |
| `disabled_by_default()` / `enabled_by_default()` | State a nested subcommand starts in. |
| `immediate_callback(bool)` | Alias for running the callback at parse-complete rather than final. |
| `group("Name")` / `group("")` | Help section; `""` hides it. |
| `alias("name")` | Additional name for the same subcommand. |
| `set_help_flag("")` | Remove the subcommand's own `--help`. |

## Callbacks and ordering

There are three App-level callbacks:

| Setter | Fires |
|--------|-------|
| `preparse_callback(fn)` | `void(std::size_t)` — before parsing, given the remaining arg count. |
| `parse_complete_callback(fn)` | After this App's args are parsed, before requirement checks on parents. |
| `final_callback(fn)` / `callback(fn)` | Last. `callback()` is an alias for `final_callback()`. |

Measured order for `./q8 --name x sub --flag`:

```
app.preparse(4)
  sub.preparse(1)
  sub.parse_complete
app.parse_complete
  sub.final
app.final
```

So: all `preparse` outside-in, then the child's `parse_complete`, then the parent's
`parse_complete`, then `final` outside-in again. The subcommand's final callback runs **before** the
parent's — which is what makes the per-subcommand-callback pattern work.

With no subcommand given, only `app.preparse(2)` / `app.parse_complete` / `app.final` fire — a
subcommand's callbacks do not run at all.

For option-level ordering use `->trigger_on_parse()` (run the option callback the moment it is seen)
and `->callback_priority(CLI::CallbackPriority::First)` and friends.

## Environment variables

```cpp
std::string token;
app.add_option("--token", token, "API token")->envname("MY_TOOL_TOKEN");
```

Verified: the env var is read when the option is absent, and a command-line value **overrides** it.
Combine with `->required()` and the requirement is satisfied by the env var.

## Config files

```cpp
app.set_config("--config",          // flag name ("" disables)
               "",                  // default file name
               "Read a TOML config",// help text
               false);              // required?
```

The format is TOML-ish (also reads INI). Given this file:

```toml
name = "from-config"
count = 7
vec = [1, 2, 3]

[sub]
flag = true
```

Measured results:

```
$ ./q8 --config q8.toml
name=from-config count=7 vec=3 flag=1 got_sub=1

$ ./q8 --config q8.toml --name cli-wins
name=cli-wins count=7 vec=3 flag=1 got_sub=1
```

Precedence is: **command line > config file > environment variable > your variable's initial value.**

### Two config gotchas

**1. `[sub]` does nothing unless the subcommand is `->configurable()`.**

Without it, the keys under `[sub]` are applied as *defaults* to the subcommand's options but the
subcommand is not considered given. Measured on the same file: `flag=1 got_sub=0` for a plain
subcommand, `flag=1 got_sub=1` once `->configurable()` is set. So a `->callback()` on a
non-configurable subcommand will not fire from a config file.

**2. Unknown config keys are silently ignored by default.**

`allow_config_extras_` defaults to `ConfigExtrasMode::Ignore`. A typo'd key in the config file is
not an error — verified: a config containing `name`/`count`/`vec` loaded into a program that defines
none of them exited 0. To catch typos:

```cpp
app.allow_config_extras(CLI::config_extras_mode::error);
```

`CLI::config_extras_mode` values: `error`, `ignore`, `ignore_all`, `capture`.

### Loading a config from inside a subcommand

Mark the subcommand `->configurable()` *and* give the subcommand its own `set_config` if it should
read a separate file. An option can be excluded from config round-tripping with
`->configurable(false)`.

## Writing a config file out

```cpp
std::string text = app.config_to_str(CLI::ConfigOutputMode::Active);
std::string all  = app.config_to_str(CLI::ConfigOutputMode::AllDefaults, /*write_description=*/true);
```

`CLI::ConfigOutputMode`: `Active` (only what was given), `AllDefaults` (everything),
`ActiveSubcommandDefaults`.

Measured `Active` output after loading the file above:

```
name="from-config"
count=7
vec=[1, 2, 3]
[sub]
flag=true
```

A `--save-config` style flag is the usual pairing:

```cpp
app.add_flag_callback("--save-config", [&]{
    std::ofstream out{"tool.toml"};
    out << app.config_to_str(CLI::ConfigOutputMode::AllDefaults, true);
});
```

Note the output is produced *after* parsing, so register this as a `final_callback` or a
`->trigger_on_parse()`-free flag you check yourself if you need the fully-populated state.

## Customizing the config format

`app.get_config_formatter_base()` returns a `ConfigBase *` with these chainable setters:

| Setter | Purpose |
|--------|---------|
| `comment(char)` | Comment character (default `#`). |
| `arrayBounds(char start, char end)` | Default `[` `]`. |
| `arrayDelimiter(char)` | Default `,`. |
| `valueSeparator(char)` | Default `=`. |
| `quoteCharacter(char str, char literal)` | Default `"` and `'`. |
| `maxLayers(uint8_t)` | How deep section nesting may go. |
| `parentSeparator(char)` | Default `.` for `sub.opt` keys. |
| `commentDefaults(bool)` | Comment out default-valued entries when writing. |
| `section(const std::string&)` | Read only one section of the file. |
| `index(int16_t)` | Read the *n*-th occurrence of that section. |
| `allowDuplicateFields(bool)` | Accumulate rather than reject repeated keys. |

For a wholly different format (JSON, YAML), derive from `CLI::Config` and implement its two pure
virtuals, then `app.config_formatter(std::make_shared<MyConfig>())`:

```cpp
virtual std::string to_config(const App *, bool default_also, bool write_description,
                              std::string prefix) const = 0;
virtual std::vector<CLI::ConfigItem> from_config(std::istream &) const = 0;
```

There is also a non-pure `to_config(const App*, ConfigOutputMode, bool, std::string)` overload you
can override instead if you want the three-way output mode; the `bool` form is the one that must be
defined. `examples/json.cpp` in the CLI11 repo is a working JSON implementation.
