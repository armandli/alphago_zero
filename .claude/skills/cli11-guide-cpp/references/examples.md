# CLI11 Complete Examples

Every program below was compiled with

```bash
g++ -std=c++17 -O1 -Wall -Wextra -I<cli11>/include ex.cpp -o ex
```

warning-free, and run. All output blocks are the real measured output, including exit codes.

## Contents

1. [Options, positionals, validators](#1-options-positionals-validators)
2. [Flags, environment variables, repeated options](#2-flags-environment-variables-repeated-options)
3. [A git-like subcommand CLI](#3-a-git-like-subcommand-cli)
4. [Layered configuration: file, env, command line](#4-layered-configuration-file-env-command-line)
5. [Custom types, custom validators, option groups](#5-custom-types-custom-validators-option-groups)

## 1. Options, positionals, validators

```cpp
#include <CLI/CLI.hpp>
#include <iostream>
#include <string>

int main(int argc, char **argv) {
    CLI::App app{"Compress a file", "squash"};

    std::string input;
    std::string output = "out.gz";
    int level = 6;
    bool force = false;

    app.add_option("input", input, "File to compress")->required()->check(CLI::ExistingFile);
    app.add_option("-o,--output", output, "Output path")->capture_default_str();
    app.add_option("-l,--level", level, "Compression level")
        ->check(CLI::Range(1, 9))
        ->capture_default_str();
    app.add_flag("-f,--force", force, "Overwrite the output if it exists");

    CLI11_PARSE(app, argc, argv);

    std::cout << "input=" << input << " output=" << output << " level=" << level
              << " force=" << std::boolalpha << force << "\n";
    return 0;
}
```

```
$ ./e1 --help
Compress a file

squash [OPTIONS] input

POSITIONALS:
  input TEXT:FILE REQUIRED    File to compress

OPTIONS:
  -h,     --help              Print this help message and exit
  -o,     --output TEXT [out.gz]
                              Output path
  -l,     --level INT:INT in [1 - 9] [6]
                              Compression level
  -f,     --force             Overwrite the output if it exists

$ ./e1 data.txt -l 9 -f
input=data.txt output=out.gz level=9 force=true

$ ./e1 data.txt -l 42
--level: Value 42 not in range [1 - 9]
Run with --help for more information.
rc=105

$ ./e1 nope.txt
input: File does not exist: nope.txt
Run with --help for more information.
rc=105

$ ./e1
input is required
Run with --help for more information.
rc=106
```

Note how `check(CLI::Range(1,9))` and `capture_default_str()` both show up in the type column:
`INT:INT in [1 - 9] [6]`. That column is your best sanity check on an option's configuration.

## 2. Flags, environment variables, repeated options

```cpp
#include <CLI/CLI.hpp>
#include <iostream>
#include <string>
#include <vector>

struct Options {
    int verbosity = 0;
    bool color = true;
    std::string token;
    std::vector<std::string> includes;
    std::vector<int> ports;
    std::string mode = "balanced";
};

int main(int argc, char **argv) {
    Options opt;                       // outlives parse() — see cardinal rule 1
    CLI::App app{"Flags, env and repeated options", "demo"};

    app.add_flag("-v,--verbose", opt.verbosity, "Repeat for more output");
    app.add_flag("--color,!--no-color", opt.color, "Colorize output");
    app.add_option("--token", opt.token, "API token")->envname("DEMO_TOKEN");
    app.add_option("-I,--include", opt.includes, "Include dir (repeatable)");
    app.add_option("-p,--ports", opt.ports, "Comma-separated ports")->delimiter(',');
    app.add_option("-m,--mode", opt.mode, "Scheduling mode")
        ->transform(CLI::IsMember({"fast", "balanced", "thorough"}, CLI::ignore_case))
        ->capture_default_str();

    CLI11_PARSE(app, argc, argv);

    std::cout << "verbosity=" << opt.verbosity << " color=" << std::boolalpha << opt.color
              << " token='" << opt.token << "' mode=" << opt.mode << "\n";
    std::cout << "includes:"; for (auto &s : opt.includes) std::cout << " " << s; std::cout << "\n";
    std::cout << "ports:";    for (auto p : opt.ports)     std::cout << " " << p; std::cout << "\n";
    return 0;
}
```

```
$ DEMO_TOKEN=from-env ./e2 -vvv --no-color -I a -I b -p 80,443,8080 -m THOROUGH
verbosity=3 color=false token='from-env' mode=thorough
includes: a b
ports: 80 443 8080

$ DEMO_TOKEN=from-env ./e2
verbosity=0 color=true token='from-env' mode=balanced

$ DEMO_TOKEN=from-env ./e2 --token from-cli
verbosity=0 color=true token='from-cli' mode=balanced

$ ./e2 -m sideways
--mode: sideways not in {fast,balanced,thorough}
Run with --help for more information.
rc=105
```

Help output, showing what each construct renders as:

```
  -v,     --verbose [0]       Repeat for more output
          --color, --no-color{false}
                              Colorize output
          --token TEXT (Env:DEMO_TOKEN)
                              API token
  -I,     --include TEXT ...  Include dir (repeatable)
  -p,     --ports INT ...     Comma-separated ports
  -m,     --mode TEXT:{fast,balanced,thorough} [balanced]
                              Scheduling mode
```

Points worth copying:

- The options live in a struct declared in `main` — that keeps rule 1 (lifetime) trivially satisfied
  and makes them easy to pass around after parsing.
- `transform(IsMember(..., ignore_case))` both validates **and** normalizes `THOROUGH` to
  `thorough`. Using `check` instead would validate but leave the input as typed.
- `envname` shows up in help as `(Env:DEMO_TOKEN)`, and a command-line value overrides it.

## 3. A git-like subcommand CLI

```cpp
#include <CLI/CLI.hpp>
#include <iostream>
#include <string>
#include <vector>

struct Global { bool dry_run = false; int verbosity = 0; };
struct AddArgs { std::vector<std::string> paths; bool all = false; };
struct CommitArgs { std::string message; bool amend = false; };

int main(int argc, char **argv) {
    Global g;
    AddArgs a;
    CommitArgs c;

    CLI::App app{"A git-like tool", "vcs"};
    app.fallthrough();                      // BEFORE add_subcommand, so children inherit it
    app.require_subcommand(1);

    app.add_flag("-n,--dry-run", g.dry_run, "Do not write anything");
    app.add_flag("-v,--verbose", g.verbosity, "Repeat for more output");

    auto *add = app.add_subcommand("add", "Stage files");
    add->add_option("paths", a.paths, "Paths to stage")->required();
    add->add_flag("-A,--all", a.all, "Stage everything");
    add->callback([&]{
        std::cout << "add all=" << std::boolalpha << a.all << " n=" << a.paths.size()
                  << " dry=" << g.dry_run << " v=" << g.verbosity << "\n";
    });

    auto *commit = app.add_subcommand("commit", "Record changes");
    commit->add_option("-m,--message", c.message, "Commit message")->required();
    commit->add_flag("--amend", c.amend, "Amend the previous commit");
    commit->callback([&]{
        std::cout << "commit msg='" << c.message << "' amend=" << std::boolalpha << c.amend
                  << " dry=" << g.dry_run << "\n";
    });

    auto *remote = app.add_subcommand("remote", "Manage remotes");
    remote->require_subcommand(1);
    std::string rname, rurl;
    auto *radd = remote->add_subcommand("add", "Add a remote");
    radd->add_option("name", rname)->required();
    radd->add_option("url", rurl)->required();
    radd->callback([&]{ std::cout << "remote add " << rname << " -> " << rurl << "\n"; });

    CLI11_PARSE(app, argc, argv);
    return 0;
}
```

```
$ ./e3 --help
A git-like tool

vcs [OPTIONS] SUBCOMMAND

OPTIONS:
  -h,     --help              Print this help message and exit
  -n,     --dry-run           Do not write anything
  -v,     --verbose [0]       Repeat for more output

SUBCOMMANDS:
  add                         Stage files
  commit                      Record changes
  remote                      Manage remotes

$ ./e3 -vv add src include --all
add all=true n=2 dry=false v=2

$ ./e3 commit -m "hello" --dry-run
commit msg='hello' amend=false dry=true

$ ./e3 remote add origin git@example.com:x.git
remote add origin -> git@example.com:x.git

$ ./e3
A subcommand is required
Run with --help for more information.
rc=106

$ ./e3 commit
--message is required
Run with --help for more information.
rc=106
```

The `--dry-run` after `commit` only works because `app.fallthrough()` was called **before**
`add_subcommand`. Reorder those two lines and that invocation becomes
`commit: The following arguments were not expected: --dry-run`.

Note also that two different subcommands both define `add` (`vcs add` and `vcs remote add`) with no
conflict — names are scoped to their parent.

## 4. Layered configuration: file, env, command line

```cpp
#include <CLI/CLI.hpp>
#include <fstream>
#include <iostream>
#include <string>

struct Settings {
    std::string host = "localhost";
    int port = 8080;
    int workers = 4;
    bool tls = false;
    std::string log_level = "info";
};

int main(int argc, char **argv) {
    Settings s;
    CLI::App app{"A server with layered configuration", "serve"};

    app.set_config("-c,--config", "", "Read settings from a TOML file")
        ->check(CLI::ExistingFile);
    app.allow_config_extras(CLI::config_extras_mode::error);   // catch typo'd keys

    app.add_option("--host", s.host, "Bind address")->envname("SERVE_HOST")->capture_default_str();
    app.add_option("--port", s.port, "Bind port")
        ->envname("SERVE_PORT")->check(CLI::Range(1, 65535))->capture_default_str();
    app.add_option("--workers", s.workers, "Worker threads")
        ->check(CLI::PositiveNumber)->capture_default_str();
    app.add_flag("--tls,!--no-tls", s.tls, "Enable TLS");
    app.add_option("--log-level", s.log_level, "Verbosity")
        ->transform(CLI::IsMember({"debug", "info", "warn", "error"}, CLI::ignore_case))
        ->capture_default_str();

    std::string save_to;
    app.add_option("--save-config", save_to, "Write the resolved settings to a file")
        ->expected(1)
        ->configurable(false);          // keep it out of config_to_str() output

    CLI11_PARSE(app, argc, argv);

    if (!save_to.empty()) {
        std::ofstream out{save_to};
        out << app.config_to_str(CLI::ConfigOutputMode::AllDefaults, true);
        std::cout << "wrote " << save_to << "\n";
    }

    std::cout << "host=" << s.host << " port=" << s.port << " workers=" << s.workers
              << " tls=" << std::boolalpha << s.tls << " log=" << s.log_level << "\n";
    return 0;
}
```

`serve.toml`:

```toml
host = "0.0.0.0"
port = 9000
workers = 16
tls = true
log-level = "warn"
```

```
$ ./e4
host=localhost port=8080 workers=4 tls=false log=info

$ ./e4 -c serve.toml
host=0.0.0.0 port=9000 workers=16 tls=true log=warn

$ SERVE_HOST=env.example ./e4 -c serve.toml --port 7777
host=0.0.0.0 port=7777 workers=16 tls=true log=warn

$ SERVE_HOST=env.example SERVE_PORT=1234 ./e4
host=env.example port=1234 workers=4 tls=false log=info
```

The third run is the precedence proof: `--port` on the command line beat the config file, and the
config file's `host` beat `SERVE_HOST`. **Command line > config file > environment > initial value.**

With `allow_config_extras(error)` a typo'd key is now fatal instead of silently ignored:

```
$ printf 'hostt = "x"\n' > bad.toml && ./e4 -c bad.toml
INI was not able to parse hostt
Run with --help for more information.
rc=110
```

Writing the resolved settings back out:

```
$ ./e4 -c serve.toml --save-config resolved.toml
wrote resolved.toml
host=0.0.0.0 port=9000 workers=16 tls=true log=warn

$ cat resolved.toml
## A server with layered configuration
# Bind address
host="0.0.0.0"

# Bind port
port=9000

# Worker threads
workers=16

# Enable TLS
tls=true

# Verbosity
log-level="warn"
```

Without `->configurable(false)` on `--save-config`, the output would also contain
`save-config="resolved.toml"` — verified both ways. Apply it to any option that should not
round-trip.

Note the TOML key is `log-level`, not `log_level`: the key is the option's long name with the
leading dashes stripped. `log_level = "warn"` in the file fails with
`INI was not able to parse log_level` (rc=110).

`app.ignore_underscore()` does **not** fix this — verified, the error is identical with it set. To
accept both spellings, declare both names on the option:

```cpp
app.add_option("--log-level,--log_level", s.log_level, "Verbosity")
    ->transform(CLI::IsMember({"debug", "info", "warn", "error"}, CLI::ignore_case))
    ->capture_default_str();
```

Verified: `log_level = "warn"` then loads, and help still shows the longest name first.

## 5. Custom types, custom validators, option groups

```cpp
#include <CLI/CLI.hpp>
#include <iostream>
#include <istream>
#include <map>
#include <ostream>
#include <string>

enum class Format : int { Json, Csv, Table };

struct Size { int w{0}, h{0}; };

std::istream &operator>>(std::istream &is, Size &s) {
    char x{};
    is >> s.w >> x >> s.h;
    if (x != 'x' || s.w <= 0 || s.h <= 0) is.setstate(std::ios::failbit);
    return is;
}
std::ostream &operator<<(std::ostream &os, const Size &s) { return os << s.w << 'x' << s.h; }

// A Validator is std::string(std::string&): empty return == OK.
static CLI::Validator PowerOfTwo() {
    return CLI::Validator(
        [](std::string &str) -> std::string {
            int v{};
            if (!CLI::detail::lexical_cast(str, v)) return "not an integer";
            if (v <= 0 || (v & (v - 1)) != 0) return "must be a positive power of two";
            return {};
        },
        "POW2");
}

int main(int argc, char **argv) {
    CLI::App app{"Custom types, custom validators, option groups", "render"};

    Size size{640, 480};
    Format fmt = Format::Table;
    int tile = 16;

    app.add_option("-s,--size", size, "Output size as WxH")
        ->type_name("WxH")
        ->capture_default_str();

    app.add_option("-f,--format", fmt, "Output format")
        ->transform(CLI::CheckedTransformer(
            std::map<std::string, Format>{
                {"json", Format::Json}, {"csv", Format::Csv}, {"table", Format::Table}},
            CLI::ignore_case));

    app.add_option("--tile", tile, "Tile edge length")->check(PowerOfTwo())->capture_default_str();

    // Exactly one destination must be chosen.
    auto *dest = app.add_option_group("Destination", "Exactly one output destination");
    std::string file;
    bool stdout_out = false;
    dest->add_option("-o,--out", file, "Write to a file");
    dest->add_flag("--stdout", stdout_out, "Write to stdout");
    dest->require_option(1);

    CLI11_PARSE(app, argc, argv);

    std::cout << "size=" << size << " fmt=" << static_cast<int>(fmt) << " tile=" << tile
              << " file='" << file << "' stdout=" << std::boolalpha << stdout_out << "\n";
    return 0;
}
```

```
$ ./e5 -s 1920x1080 -f JSON --tile 64 --stdout
size=1920x1080 fmt=0 tile=64 file='' stdout=true

$ ./e5 -s 1920:1080 --stdout
Could not convert: --size = 1920:1080
Run with --help for more information.
rc=104

$ ./e5 --tile 17 --stdout
--tile: must be a positive power of two
Run with --help for more information.
rc=105

$ ./e5 -f xml --stdout
--format: Check xml value in {csv->1,json->0,table->2} OR {1,0,2} FAILED
Run with --help for more information.
rc=105

$ ./e5
Exactly 1 option from [-o,--out,--stdout] is required
Run with --help for more information.
rc=106

$ ./e5 -o f --stdout
Exactly 1 option from [-o,--out,--stdout] is required but 2 were given
Run with --help for more information.
rc=106
```

Help output:

```
  -s,     --size WxH [640x480]
                              Output size as WxH
  -f,     --format ENUM:value in {csv->1,json->0,table->2} OR {1,0,2}
                              Output format
          --tile INT:POW2 [16]
                              Tile edge length
[Option Group: Destination]
  Exactly one output destination
  [Exactly 1 of the following options are required]

  OPTIONS:
    -o,     --out TEXT          Write to a file
            --stdout            Write to stdout
```

Three things to take from this:

- `operator>>` setting the failbit is the whole contract for a custom type. CLI11 turns it into
  `ConversionError` (104) with the message `Could not convert: --size = 1920:1080`. `->type_name("WxH")`
  replaces the otherwise-useless `TEXT`.
- The auto-generated enum type string is noisy:
  `ENUM:value in {csv->1,json->0,table->2} OR {1,0,2}`. Fix it with **`->option_text(...)`**, not
  `type_name`: `type_name("json|csv|table")` only replaces the base type and still appends the
  validator description (`json|csv|table:value in {csv->1,...}`), whereas
  `->option_text("json|csv|table")` replaces the whole column. Verified both; validation is
  unaffected either way, and the *error message* still shows the raw map — so keep the enum names
  short.
- An option group with `require_option(1)` is the idiomatic mutually-exclusive-and-required group.
  It produces better messages than hand-wiring `excludes()` pairs, and the counts scale
  (`require_option(1, 2)`, `require_option(-1)` for at-most-one).
