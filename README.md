# jbi — Just Build It

`jbi` is a small C99 command-line tool and library that detects a project's build
system from marker files, ranks likely candidates using build commands mentioned
in project documentation, and tries configured build commands until one succeeds.

> **Important:** build configurations execute shell commands. Review project-local
> `.jbi/*.jbi` files before running `jbi` on an untrusted project.

## Features

- C99 CLI plus static (`libjbi.a`) and shared (`libjbi.so` / `libjbi.dylib`) libraries.
- Project-local configuration in `.jbi/`, overriding system configurations.
- Parallel marker-file and documentation scanning using POSIX threads.
- Stable ranking by documentation evidence, marker-file evidence, then configured
  `order` (lower values are tried first).
- Fallback attempts across all configured build systems.
- Build-system-specific install/uninstall commands; the successful build system is
  recorded in `.jbi/state` so later lifecycle commands use the same configuration.
- Dry-run mode and an inspection command (`list`).

## Build

Requirements: a C99 compiler, POSIX threads, and `make`.

```sh
make
make check
```

Build outputs are `jbi`, `libjbi.a`, and the platform shared library.

## Use

```sh
jbi [build|install|uninstall|list] [-n] [-C directory]
```

- `build` is the default command.
- `-C DIR` selects a project directory (default: current directory).
- `-n` prints the first selected command without executing it.
- `list` prints the ranked plan and configured commands.
- `-h` / `--help` prints usage; `-V` / `--version` prints the version.

`jbi` searches for configuration files in `PROJECT/.jbi/` first and then in the
system configuration directory. The system directory defaults to
`/usr/local/share/jbi`; set `JBI_SHAREDIR` in the environment to override it at
runtime.

## Configuration format

Each `NAME.jbi` file is a simple line-oriented file:

```ini
# required: whitespace-separated project marker names
files = Makefile GNUmakefile

# optional: lower values are tried first (default 100)
order = 50

# required: one or more shell commands, tried in order
build = make
build = make build

# optional lifecycle commands
install = make install
uninstall = make uninstall
```

Blank lines and lines beginning with `#` are ignored. `files` and at least one
`build` command are required. Each `build`, `install`, or `uninstall` line adds
one command. Project-local configurations with the same filename stem override
system configurations.

The bundled configurations cover shell scripts, CMake, Meson, Autoconf, Make,
Cargo, and Go. Add a `.jbi/NAME.jbi` file to support another build system or to
customize a project's commands.

## Install

```sh
sudo make install
```

The default prefix is `/usr/local`. Customize paths with `PREFIX`, `DESTDIR`,
`BINDIR`, `LIBDIR`, `INCDIR`, and `SHAREDIR`:

```sh
make install PREFIX=/opt/jbi DESTDIR=/tmp/package
```

Remove installed files with `make uninstall` using the same path variables.

## Library API

Include `jbi.h` and link with `-ljbi -pthread` (or link the static archive).
The public API provides context creation/destruction, detection, build/install/
uninstall operations, and ranked-plan output. See `jbi.h` for declarations.

## Limitations

- POSIX platform required; Windows is not currently supported.
- Detection is intentionally heuristic: marker files and simple command mentions
  in top-level documentation are used, not full build-file parsing.
- The dry-run mode prints the first command in the selected configuration; it does
  not simulate subsequent fallback attempts.
- Install/uninstall commands may require elevated privileges and should be reviewed
  before execution.
