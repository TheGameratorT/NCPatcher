# NCPatcher
A universal Nintendo DS code maker/patcher.

NCPatcher is a program that modifies the executable binaries of a Nintendo DS ROM. \
It was created because of the need to have more flexible patching features that other patchers did not have.

## Credits
This program was made with the help of the [Mamma Mia Team](https://github.com/MammaMiaTeam) members. \
NCPatcher was heavily inspired by Fireflower.

## Installing

Grab a release, or build from source. Either way the binary can live anywhere:
it locates `ncp.h` and its companions relative to itself, so there is no longer
anything to add to `PATH` beyond the binary, and nothing to reboot for.

**Windows**: run the installer and tick *Add NCPatcher to the system PATH*, or
unzip the portable archive anywhere and put that directory on `PATH` yourself.
With [Scoop](https://scoop.sh): `scoop install ncpatcher`.

**Linux and macOS**: unpack the release archive, or install from source:

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
sudo cmake --install build              # /usr/local/bin + /usr/local/share/ncpatcher
```

`--prefix` puts it somewhere else; the lookup is relative, so a prefix under
`$HOME` works without any further configuration. Packagers will want
`-DNCP_USE_SYSTEM_DEPS=ON`, which turns a missing yaml-cpp into a configure
error rather than a download. See [packaging/README.md](packaging/README.md).

### Where the SDK files go

`ncp.h`, `ncp_ide.h` and `ncprt.c` (the SDK) are compiled into your ARM code,
not into ncpatcher, so they are installed as program data rather than as host
headers, in `/usr/share/ncpatcher` or beside the binary on Windows. They are
looked for in:

1. `$NCPATCHER_DATA_DIR`, if set
2. `<directory of the binary>/../share/ncpatcher`
3. the directory of the binary, and its `include` subdirectory

The version of `ncp.h` found is checked against the one the binary expects. A
mismatch is an error rather than a warning: a stale header compiles perfectly
well, emits sections nothing is looking for any more, and hands you a ROM with
the patches silently missing.

## Building

CMake 3.20 and a C++20 compiler. yaml-cpp is used if the system has it and
fetched if not.

```sh
git clone https://github.com/TheGameratorT/NCPatcher.git
cd NCPatcher
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

On Windows, name the generator instead of the build type:
`cmake -B build -G "Visual Studio 17 2022" -A x64`, then
`cmake --build build --config Release`. The binary lands in `build`, with the
SDK files copied beside it so it can be run from there without installing.

Useful switches:

| Switch | Meaning |
|---|---|
| `-DNCP_BUILD_TESTS=ON` | Build the unit tests; run them with `ctest --test-dir build` |
| `-DNCP_PORTABLE_LAYOUT=ON` | Install everything into one flat directory (the default on Windows) |
| `-DNCP_USE_SYSTEM_DEPS=ON` | Require yaml-cpp from the system rather than fetching it |

## Running

Configure the project as described below, then run `ncpatcher build` in the directory
holding its configuration file, or from anywhere with `ncpatcher -C <that
directory>`.

NCPatcher patches a `.nds` directly:

```yaml
rom:
  file: roms/nsmb.nds     # patched in place
  output: build/out.nds   # ...unless you say where to put the result
  backup: backup
```

Without a `files:` section it changes only the header, ARM binaries, overlay
tables and overlay files. A `files:` section may deliberately replace or add
NitroFS files. The ROM is edited in place when its existing layout has room;
when an ARM binary or table outgrows that room, NCPatcher lays the container out
again without changing unrelated file contents.

An extracted directory works too, and is what a level editor hands it:

```yaml
rom:
  dir: __tmp
  backup: backup
```

`ncpatcher rom extract <dir>` writes that directory out of a `.nds`, and
`ncpatcher rom pack <dir>` folds it back in, which is the job most projects
currently do with a script of their own. Neither one takes the NitroFS apart;
`ndstool`, or `nds-extract` from
[Fireflower](https://github.com/MammaMiaTeam/Fireflower/releases/latest), is
the tool for that.

Patching a directory does not rewrite its `header.bin`: the header is an input
the patcher has never owned, and every tool that repacks one of these
directories works the sizes out for itself. `ncpatcher rom pack` does too, and
takes the header from the ROM it is packing into and updates it there.

If your extraction names the files differently, say so rather than renaming
them:

```yaml
rom:
  dir: dump
  layout: ndstool            # or a mapping of names
  # layout: { preset: ndstool, arm7-ovt: y7.bin }
```

The presets are `ncpatcher` (the default, and what a level editor writes) and
`ndstool`. The names a mapping may set are `header`, `arm9`, `arm7`,
`arm9-ovt`, `arm7-ovt`, `overlay9-dir`, `overlay7-dir`, `overlay9-name`,
`overlay7-name`, `fnt`, `fat`, `banner` and `data-dir`; the two `*-name` keys
take `{id}` for the overlay number, or `{id:4}` to zero-pad it.

### Growing ARM9

An `append` region adds code to the end of ARM9, and ARM9 cannot be moved: the
secure area is encrypted against its being at the offset the header names. So
the first build that outgrows the gap before the overlay table lays the ROM out
again, leaving `rom.arm9-slack` bytes (64 KiB by default) of room behind it.
Because every build re-applies its patches to the *pristine* ARM9 rather than
to the last build's output, the patched size is a function of the code and not
of how many times you have built, so that happens once, and later builds write
in place and produce a byte-identical ROM.

## Command line

Running `ncpatcher` with no subcommand prints its version and points at the
help. Building writes into the ROM, so it is asked for by name.

```
ncpatcher build [--variant NAME | --all-variants]
                                      compile and patch
          init [--template NAME]      create a v2 project (default or nsmb)
          clean [--backups]          delete the build directories
          restore                    put the ROM binaries back and drop the backups
          config dump [--explain] [--json]
          config validate
          config path
          migrate [--write]          convert ncpatcher.json to ncpatcher.yaml
          modules list               show which modules are enabled
          modules dump [-o PATH]     print the resolved module graph as JSON
          modules explain NAME       say why a module or component is where it is
          rom info                   print what the ROM header says
          rom files [--json]         list the ROM's NitroFS files and their ids
          rom extract DIR            write the ROM's code binaries into DIR
          rom pack DIR               fold a directory of code binaries into the ROM
          files plan [--json] [-o PATH]
                                     report what a build would place, without building
          version
```

Options, which may be written before or after the subcommand:

| Option | Meaning |
|---|---|
| `-C, --project PATH` | Project directory, or the configuration file itself |
| `--rom PATH` | ROM to patch: a `.nds` or an extracted directory |
| `--out PATH` | Write the patched ROM here instead of patching in place |
| `build --variant NAME` | Build one configured define/file variant |
| `build --all-variants` | Build every configured variant |
| `-D, --define NAME[=VAL]` | Define a preprocessor macro |
| `--var NAME=VAL` | Override a `vars:` entry |
| `--toolchain PREFIX` | Cross-compiler prefix |
| `-j, --jobs N` | Compile jobs; 0 means one per hardware thread |
| `-v, --verbose` / `--verbose-tag TAG` | Verbose output, all of it or one category |
| `--color auto\|always\|never` | Console styling. `NO_COLOR` is honored |
| `--log PATH` / `--no-log` | Where the log file goes, or that there is none |
| `--message-format human\|json` | See below |
| `--result PATH` | Write a JSON summary of the run |

`-C` is what removes the "must be launched from the project directory"
constraint, so a ROM editor or a build script no longer has to `cd` first.

Three settings also read the environment, which the command line still beats:
`NCPATCHER_TOOLCHAIN`, `NCPATCHER_JOBS` and `NCPATCHER_LOG`. The precedence is
command line, then environment, then the project file, then the built-in
default, and `ncpatcher config dump --explain` says which of them won for each
setting. `NCPATCHER_DATA_DIR` is separate from that chain: it says where
`ncp.h` lives, which is a property of the installation rather than of the
project.

The log goes to `<buildDir>/ncpatcher.log` (the ARM9 target's build directory,
or the only enabled one's) so `clean` takes it away with everything else it
made. Only a build writes one; `config dump` and friends have no build
directory to write into and are not worth creating one for. `--log` puts it
somewhere else, `--no-log` turns it off.

### Machine-readable output

`--message-format json` writes one JSON object per line on stdout and moves the
human log to stderr, so a caller never has to match on English:

```json
{"type":"progress","phase":"compile","current":43,"total":210,"item":"source/Coop.cpp"}
{"type":"artifact","kind":"overlay","proc":"arm9","id":58,"action":"modified","size":4788,"ram-address":"0x021726C0","file-id":117,"name":"overlay9/overlay9_58.bin"}
{"type":"diagnostic","level":"error","code":"NCP0001","message":"...","location":{"file":"ncpatcher.yaml","line":88,"col":9,"path":"targets.arm9.regions[12].maxsize"}}
{"type":"result","status":"error","exit-code":3,"duration-ms":8123,"errors":1,"warnings":3}
```

`--result PATH` writes the same run as a single JSON object, including every
diagnostic and artifact. Both work with or without `--message-format json`.
In JSON message mode GCC diagnostics are emitted individually with their
severity and source location; the human rendering remains on stderr.

`config dump --json` writes an `ncpatcher.config/1` document: the settings as
the build understands them, with every path already resolved against the
project directory. Besides the targets and their flags it carries what a
consumer needs to reach the same answers this program does without reading
`ncpatcher.yaml` itself —

| Field | Meaning |
|---|---|
| `rom-banner` | The file replacing the ROM banner, or empty. Not a NitroFS file, so no `files:` entry would ever name it |
| `variants.<name>.banner` | That variant's banner override, or empty |
| `variants.<name>.module-variants` | Which layer of a module's own tree this variant selects, for modules that name their layers differently from the project's variants |

All three keys are always present, so nothing has to test for a key's existence
to learn that a project or a variant overrides nothing.

### Exit codes

| Code | Meaning |
|---|---|
| 0 | Success |
| 1 | Internal error |
| 2 | The command line did not parse |
| 3 | Configuration |
| 4 | Module resolution |
| 5 | Toolchain or SDK header not found |
| 6 | Compilation |
| 7 | Linking |
| 8 | Patching |
| 9 | ROM file I/O |
| 10 | A hook command failed |
| 11 | Cancelled |


### Cancelling a build

A ROM editor that starts a build needs a Cancel button, and it needs to know
what to send and what will come back.

**Send `SIGINT` (POSIX) or `CTRL_BREAK_EVENT` (Windows) to the process group.**
Compiler children are in that group and stop with it. The build notices at its
next checkpoint, unwinds the ordinary way, and exits **11**. A `--result`
document is still written, with `status: "cancelled"`, so a caller learns what
had finished before the stop rather than nothing at all.

Checkpoints are between build phases and between compilations. A compilation
already running finishes -- its object file is complete and correct, and the
next build will not redo it -- while everything still queued is dropped, so a
cancelled build drains instead of compiling its way to the exit.

Stopping is safe by construction rather than by care. `BackupStore` holds the
pristine binaries, so the next run starts from those and not from half-written
output, and the `.nds` backend holds every write until the end, so a ROM is
written whole or not at all. Cancel a build, run it again, and you get the ROM
you would have got without the interruption.


## Configuration

New projects use one `ncpatcher.yaml` with `version: 2`. At least one ARM target
must be enabled. Existing version 1 `ncpatcher.json` projects remain supported;
`ncpatcher migrate` prints their v2 equivalent and `ncpatcher migrate --write`
saves it after verifying that the resolved build is unchanged.

Create a project in a new directory with the portable default or the built-in
New Super Mario Bros. setup:

```sh
ncpatcher -C my-project init
ncpatcher -C my-nsmb-mod init --template nsmb
```

Both create `ncpatcher.yaml` and `source/`, and the default also creates
`include/`. The generated config uses direct ROM input and carries the published
schema URL for editor completion. The default expects `game.nds`; `nsmb` expects
`NSMB.nds` and reads the converted SDK headers, the NSMB-Code-Reference headers
and its `symbols9.c` through the `NSMB_NITRO_ROOT` and `NSMBREF_ROOT`
environment variables. Existing project configuration files are never
overwritten; use `migrate` for a v1 project.

This project builds ARM9 code from `code/`, patches a direct ROM, and appends
code to both the main binary and overlay 9:

```yaml
version: 2

rom:
  file: roms/game.nds
  output: build/game.nds
  backup: backup

toolchain: arm-none-eabi-
build:
  threads: 0

defines: [SDK_GCC, SDK_FINALROM]
flags:
  common: [-mno-unaligned-access, -mfloat-abi=soft, -mabi=aapcs, -fno-builtin]
  c: [-Os, -fomit-frame-pointer]
  cpp: [-Os, -fomit-frame-pointer, -fno-rtti, -fno-exceptions, -std=c++20]
  asm: [-Os, "-x assembler-with-cpp"]
  ld: [-lgcc, -lc, -lstdc++, --use-blx]

targets:
  arm9:
    build: build/arm9
    workdir: code
    symbols: code/symbols9.x
    includes: [include]
    defines: [SDK_ARM9]
    flags:
      common: [-march=armv5te, -mtune=arm946e-s, -marm]
    # arena-lo: 0x02065F10  # optional override; normally detected
    regions:
      - dest: main
        sources: ["source/**", "!source/overlays/**"]
      - dest: ov9
        mode: append
        maxsize: 0x56400
        compress: true
        defines: [OVERLAY_ID=9]
        sources: source/overlays/ov9/**
```

Relative ROM, backup, build and symbols paths start at the project directory.
`workdir` changes the compiler working directory and the base for that target's
source and include globs; it defaults to the project directory. In the example,
`include` and `source/**` therefore mean `code/include` and `code/source/**`,
while `build/arm9` and `code/symbols9.x` remain project-relative.

Project settings are inherited by each target, and target settings by each
region. A scalar or list appends to an inherited `includes`, `defines`,
`sources`, or flag list. Use the mapping form when a level must replace or
subtract entries:

```yaml
flags:
  cpp:
    remove: [-Os]
    append: [-O2]
```

The operations run in `set`, `remove`, `append` order. `flags.common` is passed
to C, C++, and assembly; `c`, `cpp`, and `asm` add language-specific options;
`ld` is target-wide linker input.

### Regions

| Key | Meaning |
|---|---|
| `dest` | Required: `main` or `ovNN`. |
| `mode` | `append` (default), `replace`, or `create`. `replace` and `create` apply to overlays. |
| `address` | Required for `create`; optional for `replace` when relocating the overlay. |
| `maxsize` | Refuse a region that grows beyond this size. The default is 1 MiB. |
| `compress` | BLZ-compress an overlay when that makes it smaller. A main region is written uncompressed. |
| `sources` | Source paths and globs. `*`, `?`, character classes, `{a,b}`, whole-segment `**`, and leading `!` exclusions are supported. |
| `defines`, `flags` | Compile settings inherited from the target and optionally adjusted here. |
| `overwrites` | Original-binary address ranges this region may reuse, written as `[start, end]` pairs. |

ARM7 and ARM9 normally locate their ArenaLo pointer automatically. Set the
target's `arena-lo` only when using a game or binary whose initialization code
the finder does not recognize. A `create` region must use the next contiguous
overlay ID and provide its load address.

### Variables

`vars:` declares lazily expanded project values. Use `${vars.NAME}` to read
one and `--var NAME=VALUE` to override it. `${env.NAME}` reads the environment;
`${env.NAME:-fallback}` supplies a default. Built-in references include
`${project.root}`, `${config.dir}`, `${rom.dir}`, `${target.name}` and
`${target.build}`. `${ncp.moduleDump}` is available when `modules.dump` is set,
and `${ncp.fileDump}` when `files-dump` is.
`${variant.name}` and `${rom.output}` are available in hooks; see below for why
they are only resolved there. Write `$$` for a literal dollar sign.

### The environment file

An environment variable belongs to a shell, and a shell has one of each. A
machine building two projects against two revisions of the same shared header
tree has nowhere to say so: whichever value the profile exports applies to both.

So a project may carry a `.ncpatcher.env` beside its configuration, and
`${env.NAME}` reads it before the real environment:

```
# The reference this project builds against, written by `nsmbtool reference sync`.
NSMBREF_ROOT=/home/you/.local/share/nsmbtool/reference/ac82391
```

The file is the project's own. A tool that manages one of those trees keeps its
own entry in it, and leaves the rest to you.

The format is `NAME=VALUE`, one per line, with `#` comments and optional
surrounding quotes. There is no expansion, no substitution and no `export`
keyword, because a configuration file that can run commands is one that
cannot be validated safely.

**These entries override the ambient environment**, which is the opposite of the
usual `.env` convention and is the entire point: a stale value left in a shell
profile is the failure this exists to prevent, so letting it win would defeat
it. What still outranks the file is the command line, because that is a caller
deliberately overriding the project for one invocation. Hook child processes
inherit these entries too, so a generator resolving the same variable agrees
with the configuration it was handed; a hook's own `env:` still wins.

Pass `--no-env-file` to ignore the file for one invocation.

## Build hooks

Version 2 projects use one named `hooks:` list. `when` chooses the build phase
a hook runs in:

| `when` | Runs | Sees |
|---|---|---|
| `pre-build` | before NitroFS insertion | the module graph |
| `post-files` | after insertion, before compilation | the file ids this build assigned |
| `post-build` | after the patched ROM is committed | the finished ROM |

`post-files` exists because a generator that turns file ids into source code has
nowhere else to run: `pre-build` is too early, since nothing has been inserted
yet, and `post-build` is too late, since the code referring to those ids has
already been compiled.

```yaml
modules:
  dump: build/generated/modules.json

hooks:
  - name: Generate module headers
    run: python3 scripts/module_gen.py --graph "${ncp.moduleDump}"
    cwd: tools
    env:
      GENERATED_DIR: "${project.root}/build/generated"
    when: pre-build

  - name: Generate file id header
    run: python3 scripts/fid.py
    when: post-files

  - name: Summarize build
    run: python3 scripts/report.py
    when: post-build
```

`cwd` is relative to the project directory and defaults to that directory.
`env` overrides variables for the child process only; all other environment
variables are inherited. Hook `name`, `run`, `cwd`, `env` values, and `when`
support the same `${vars.NAME}`, `${env.NAME}`, `${project.root}`, and other
configuration references as the rest of the file. `${ncp.moduleDump}` is the
absolute path configured by `modules.dump`, and `${ncp.fileDump}` the one
configured by `files-dump`; each is available when its setting is present.

Two references are resolved when the hook runs rather than when the file is
read, because they are not known any earlier: `${variant.name}` is the variant
being built, empty for a project with none, and `${rom.output}` is the ROM this
build wrote, including the `_<variant>` suffix that `--all-variants` derives.
That is what lets one post-build hook produce a patch per variant:

```yaml
  - name: Patch
    run: xdelta3 -e -f -s rom.nds "${rom.output}" build/xdelta/${variant.name}.xdelta
    when: post-build
```

The old `pre-build` and `post-build` string arrays remain accepted in both
configuration versions. Do not combine those compatibility keys with `hooks:`
in one version 2 file.

## NitroFS files

`files:` maps each path inside NitroFS to a source file. Sources are resolved
from the project directory after pre-build hooks run, so a hook may generate
them. Files are inserted before target resolution and compilation:

```yaml
files:
  sp/demo/readme.txt: nitrofs/sp/demo/readme.txt
  z_new/coop/SE_VOC_MA_SHOT.nwav: build/generated/SE_VOC_MA_SHOT.nwav
```

A destination outside `z_new/` must already exist in the ROM; a missing one is
an error rather than an accidental new file ID. Missing paths under `z_new/`
are added to the FNT and FAT. Existing `z_new/` paths are replaced, making
repeated builds stable.

This works directly on `.nds` inputs and on complete extracted layouts that
include `fnt.bin`, `fat.bin`, and the configured `data-dir`.

### Reserving the first new file ID

Some games hold arrays of file IDs in compiled code, ended by a sentinel value.
The sentinel a compiler picked is typically the ID one past the last file the
retail ROM shipped with, which is exactly the ID NCPatcher gives to the first
file a build adds. Put real content there and the game has a loadable file at
an ID its own code reads as *stop*.

`files-reserve:` spends that ID on nothing:

```yaml
files-reserve: z_new/reserved
```

The named path is created as an empty file before any other addition, so it
takes the first new ID and the project's own files start after it. It may not
also appear in `files:` or be supplied by a `file-trees:` entry, because the
placeholder has to stay empty, and a build that filled it would be undoing the
reservation.

Nothing is reserved unless the key says so. Whether a game needs this, and
which ID is affected, is a fact about the game rather than about NitroFS.

> **New Super Mario Bros. needs it.** `ncpatcher init --template nsmb` writes
> `files-reserve: z_new/reserved`. An existing NSMB project that adds `z_new/`
> files must set it too: without the key every one of those files shifts down
> by one ID, and a level or save that refers to them by number will not survive
> the change.

### Claiming an existing file ID

Sometimes a project needs a ROM path the retail game never had, and cannot
afford a new file ID. Adding one appends to the end of the FAT, which is fine
under `z_new/` and impossible anywhere else. The alternative is to take over a
file that is known to be unused: it keeps its ID, and only its name changes.

Write that with the mapping form of a `files:` entry:

```yaml
files:
  demo/boot_sub_bg_ncg.bin:
    source: nitrofs/demo/boot_sub_bg_ncg.bin
    id: 1209
```

File 1209 is renamed to `demo/boot_sub_bg_ncg.bin` and its data replaced.
Nothing else moves, which is the whole point.

A file ID belongs to its directory's consecutive range, so it can be renamed
but not moved: the ID named here must already exist and must live in the
destination's parent directory. Two entries may not claim one ID, and an entry
may not claim an ID whose current name another entry targets by path, since
both would otherwise resolve by insertion order.

### Editing a file inside a Nitro archive

Most of a DS game's assets are not loose files. They live in `.narc`
containers (a FAT, a name table and a blob of data, the ROM's own filesystem
in miniature) and reaching one of them means opening the container.

A destination can name two coordinates instead of one: the archive's ROM path,
`!`, then the path within it. It is the separator `jar:` and `zip:` URIs use
for the same job.

```yaml
files:
  ARCHIVE/menu_title.narc!menu/title/USA/vs.bmg: nitrofs/fr/vs.bmg
```

Archives are edited in place and never added to. Game code reads a member by
its index, so inserting one would renumber every member after it, the same
reason NitroFS file IDs are never renumbered. The archive must already exist
and must already hold the member named; a missing member is an error rather
than a warning, because the way that ships is a ROM with the translation still
in the original language.

A replacement of a different size is fine. The allocation table and the data
chunk are laid out again around it, and every member keeps its index. An
archive nothing edited is written back byte for byte.

`id:` cannot be combined with an archive destination, since it renames a loose
file and a member of an archive is not one.

The manifest records what happened inside: see *Members of an edited archive*
below.

#### Compressed archives

Some games store their archives compressed. Mario Kart DS stores 286 of them
and names them `.carc`, but the name is that game's convention rather than a
format: what is inside the wrapper is an ordinary NARC, and the wrapper is plain
Nitro LZ77.

An archive is recognised by its bytes, never by its extension. The test is
"does this decompress to something beginning with `NARC`", so a game shipping
compressed archives named `.narc`, or plain ones named `.carc`, needs no special
case — and Mario Kart's `dwc/utility.bin`, which begins with `0x10` and is not
compressed at all, is not mistaken for one. Everything else is unchanged: the
same `!` destinations, the same folder convention, the same replace-only rule.

**A container goes back in the wrapper it arrived in**, even when compressing it
makes it larger. That is not an oversight: a game that reads an archive through
its decompressor will not accept a raw one in its place, so the wrapper is part
of the file's identity rather than a size optimisation to re-decide. Mario
Kart's own `GeneralMenu_es.carc` is 85 bytes stored for 72 raw, and the game
loads it.

The later `0x11` LZ form is read and not written. The two are not decoded by the
same routine, so substituting `0x10` for it would leave the game unpacking the
archive with the wrong one — quietly, into whatever the misparse produced. No
game NCPatcher has been used on ships one; editing such an archive is refused
with a message saying exactly this.

### Replacing the ROM banner

The icon and title shown on the console's menu are not a NitroFS file: they are
a region of their own that the header points at, so no ROM path would name it.
It gets its own key:

```yaml
rom:
  file: rom.nds
  backup: backup
  banner: nitrofs/banner.bin
```

The replacement must be exactly as long as the ROM's own banner. A banner's
length is fixed by the version word it starts with, so a different length is a
different format rather than a bigger banner, and the build says so instead of
laying the container out again around it.

A variant may override it with its own `banner:`, though one banner normally
serves every build, because the region carries a title in all six console
languages at once.

## NitroFS trees

`files:` is one line per file, which stops scaling around the point a project
ships a filesystem rather than patching three files. `file-trees:` sweeps a
directory instead: what is under it is what the ROM gets, at the same relative
path.

```yaml
file-trees:
  - dir: nitrofs
    layered: true
    base-variant: en
```

With `layered`, the first path segment is a variant name rather than part of
the ROM path, so `nitrofs/fr/ARCHIVE/x.bin` is `ARCHIVE/x.bin` for the French
build and nothing at all for the German one. `base-variant` is applied
underneath, which is what lets a project translate eight files out of two
thousand: the base supplies everything the variant does not override. A
variant may supply a path the base never had, and a variant directory that
does not exist contributes nothing rather than failing. `into:` prefixes every
destination the tree produces.

Modules declare their own the same way, with `nitrofs:` in `module.yaml`:

```yaml
nitrofs:
  dir: nitrofs
  layered: true
  base-variant: en
```

### Which copy wins

Two rules decide a destination more than one tree provides, in this order.

**Layering first.** A file chosen for the built variant outranks one that
applies to every variant. A module translating `enemy/w3_sign.nsbmd` into
French beats a module supplying the English original for all languages.

**Then module order.** Within one layer, the first tree to claim a destination
keeps it, and `modules.enabled` is that order. This is precedence rather than a
tiebreak: a module that replaces a piece of artwork wholesale (because the
replacement has its own text baked in) has to outrank one that only translates
the stock version, and listing it first is how the project says so.

Explicit `files:` entries win over anything a tree swept, since they are the
project overruling the sweep by name.

### When a module names its layers differently

A module is written without knowing which project will use it. Its tree may be
split by region where the project splits by language, or say `french` where the
project says `fr`. Nothing connects the two names, so the module quietly
contributes nothing, the worst available outcome.

A variant can say which layer of a given module it means:

```yaml
variants:
  fr:
    defines: GAME_LANGUAGE_FR
    module-variants:
      thirdparty: french
```

Keys are module names, values are that module's own layer name. Only the
variant layer is redirected; the module's `base-variant` is its own
declaration and keeps filling the gaps as before.

A layer named here must exist. That is the opposite of the unmapped case, where
a missing variant directory is ordinary, but naming a layer is an assertion,
and honoring a typo by silently falling back to the base layer is how a build
ships without its translations. Mapping a module that is not enabled, or one
that declares no tree, is an error for the same reason.

### Turning a component's assets off with it

A component's `files:` patterns name the ROM destinations it owns. While the
component is enabled they only record ownership, which is what the file
manifest reports. When it is disabled they subtract those files from its own
module's tree, so switching a feature off removes its assets along with its
code:

```yaml
components:
  - CustomWorldUnlock:
      target: arm9
      sources: "source9/WorldUnlock.cpp"
      files:
        - "enemy/w3_sign.nsbmd"
        - "enemy/w6_sign.nsbmd"
```

Patterns are globs matched against the ROM destination with the variant
segment already stripped.

### Archives in a tree

A directory cannot also be a file, so an archive a tree edits has to be spelled
as a folder. The convention is the archive's name with its dot turned into an
underscore, and everything below it is a path inside the archive:

```
modules/message/nitrofs/fr/ARCHIVE/menu_title_narc/menu/title/USA/vs.bmg
                                   └──────┬──────┘ └────────┬──────────┘
                          ARCHIVE/menu_title.narc    menu/title/USA/vs.bmg
```

The extension is whatever the game calls its archives, not a literal `narc`:
Mario Kart DS stores compressed ones and names them `.carc`, so
`data/Main2D_carc/menu/icon.NCGR` reaches inside `data/Main2D.carc` there.

A segment is read as an archive **when the ROM holds an archive at the path it
names**, and as an ordinary directory name otherwise. That is why the rule needs
no configuration and has no exceptions to remember: `z_new/` is not the archive
`z.new` because no such archive exists, and a ROM that genuinely holds a
directory called `*_narc` simply works, rather than having to be written out in
`files:` as it used to.

Only the outermost matching directory is read that way; an archive inside an
archive is not something this opens.

## The file manifest

A DS game loads a file by number, not by path. So any code that reads one needs
a constant, and that constant has to be regenerated whenever the table changes,
which is what a `post-files` hook is for. `files-dump` gives that hook something
to read:

```yaml
files-dump: build/generated/files.json

hooks:
  - name: Generate file ids
    run: nsmbtool glue --manifest "${ncp.fileDump}"
    when: post-files
```

It is written after NitroFS insertion, so the ids in it are the ones this build
assigned, and before the `post-files` hooks, so a generator can turn them into
a header that the compilation after it picks up.

The manifest lists **every** file in the ROM, not only the ones the build wrote.
A generator naming files by id needs the two thousand it did not touch just as
much as the thirteen it did, and there is nowhere else to get them: most of the
table is whatever the retail ROM already had.

```jsonc
{ "schema": "ncpatcher.files/1",
  "variant": "fr",
  "count": 1971,
  "files": [
    { "id": 1500, "path": "uiStudio/title.bin", "size": 2031, "action": "unchanged" },
    { "id": 2101, "path": "z_new/message/msg_data.bin", "size": 6144,
      "action": "created",
      "source": "modules/message/nitrofs/fr/z_new/message/msg_data.bin",
      "module": "message", "component": "Vanilla", "from-variant": "fr" }
  ] }
```

`action` says what this run did to the file relative to the ROM that came in:
`unchanged`, `modified`, or `created`, and `created` only ever happens under
`z_new/`, because existing file ids are never renumbered. The provenance fields
are present only for files the build wrote, and each is omitted when it is
empty rather than emitted as `""`.

Ids are raw, exactly as the FAT stores them. A game that offsets file ids at run
time (NSMB subtracts its overlay count) applies that itself; it is a property
of that game, not of the ROM, and NCPatcher does not know about it.

Those last four fields are also what makes an editor possible: every entry is
vanilla, replaced by a module, or added by one, and grouping by path across
variants answers which languages translate a file. The schema is
`schema/files.schema.json`.

`ncpatcher rom files` prints the same table for a ROM already on disk, as a
list or, with `--json`, as the same document. Nothing there has provenance,
since it is reading a build's result rather than performing one, so every
entry is `unchanged` and there is no `variant`.

It reads the project's ROM by default, and `--rom` points it at any other,
including one a build just produced, which is how you check what actually landed
in it:

```sh
ncpatcher rom files --rom build/nds/rom_fr.nds --json | jq '.files[] | select(.id > 2087)'
```

`rom files` and `rom info` put their output on stdout and the log on stderr, so
that pipeline needs no filtering. Every command whose product *is* stdout does
the same.

### Members of an edited archive

The ROM's table has one entry for a `.narc`, however many of its members a
build replaced, and that entry can only carry the provenance its members agree
on. One member gives the whole answer; five from three modules give the honest
one, which is that no single source stands behind the file. That is the right
summary, but on its own it is also the *only* record, and the per-member truth
would be thrown away -- leaving an editor that wants to show the inside of an
archive to re-derive it from the module trees, which is precisely the
duplication `files plan` exists to remove.

So an archive this run edited also carries `members`:

```jsonc
{ "id": 152, "path": "ARCHIVE/menu_title.narc", "size": 53600, "action": "modified",
  "source": "modules/message/nitrofs/fr/ARCHIVE/menu_title_narc/menu/title/USA/vs.bmg",
  "module": "message", "from-variant": "fr",
  "members": [
    { "index": 43, "path": "menu/title/USA/vs.bmg", "size": 1344, "action": "modified",
      "source": "modules/message/nitrofs/fr/ARCHIVE/menu_title_narc/menu/title/USA/vs.bmg",
      "module": "message", "from-variant": "fr" } ] }
```

`index`, never `id`. A member index is not a NitroFS file id: the ROM's table
does not name members at all, so nothing outside the container can address one
by number, and a consumer that treated the two alike would be one confusion away
from replacing the wrong file. A member's `action` is never `created` either,
for the reason above -- the codec is replace-only.

Only the members the run edited are listed. Enumerating every member of every
archive would dwarf the rest of the document, and a consumer with the ROM open
can list them itself; what it cannot work out on its own is where the bytes came
from, so that is what this carries.

Additive, so the document is still `ncpatcher.files/1`, and a consumer that
reads only the container entry is unaffected.

### Planning a build

`ncpatcher files plan` answers the same question for a build that has not
happened:

```sh
ncpatcher files plan --variant fr --json -o build/generated/plan.json
```

It sweeps the file trees, folds in `files:`, classifies every destination and
assigns the ids the additions would get -- and writes nothing at all, not into
the ROM and not into an extracted directory. No toolchain is needed, exactly as
`modules dump` needs none.

The document is `ncpatcher.files/1` with one extra root field, `"planned":
true`. A consumer must never mistake a prospective id for a settled one, so the
flag is on the document rather than left to be inferred from the command that
produced it. Everything else means what it means after a build: `action` is what
a build *would* do, and the provenance fields say which module, component and
variant layer would supply the bytes.

This is what lets an editor show a file as *pending* with the id it is going to
get, instead of showing it as pending with no id and waiting for a build to say.

The prediction is produced by running the real insertion pass against a ROM
accessor that holds every write in memory, so a plan and the build that confirms
it come out of the same code rather than out of two implementations of the same
rules. That is the property worth testing, and it is exact:

```sh
ncpatcher files plan --variant en --json > plan.json
ncpatcher build --variant en
diff <(jq -S .files plan.json) <(jq -S .files build/generated/files.json)
```

One caveat, and it is visible in the document rather than silent. A build reads
its NitroFS sources, and a project may generate some of them in a `pre-build`
hook, which a plan does not run. A destination whose source is not on disk yet is
still planned -- with its id, its action and its provenance -- and its entry
carries `"source-missing": true`, because the one thing that cannot be known is
its size.

### The invariant a consumer can rely on

**Existing file ids are never renumbered. Only `z_new/` additions may move.**

This is not a policy that could be relaxed later. A game stores file ids in
compiled code, in save data, and in level data that has already been published;
renumbering one file invalidates all of it at once, silently, with no build
failure to notice. `NitroFs::addFile` refuses rather than renumbering, and
`files-reserve` exists so that even the first added id is predictable.

What follows for anything built on the manifest:

- An id read from one build means the same file in the next one, as long as the
  file was already in the ROM. Ids under `z_new/` are stable too, but only while
  the set of additions is, since adding a file that sorts earlier shifts the
  ones after it.
- **Adding a file is a build, not an edit.** There is no way to append to a ROM's
  table from outside; only insertion assigns an id. A tool that wants a new file
  in the ROM puts it in a module tree and lets a build place it, which is why
  an editor shows such a file as *pending* rather than writing into the ROM --
  and `files plan` is how it knows which id that file is going to be given.
- Identity that has to survive should not be an id. Where something must be
  referred to across builds (a level naming an object it places, say) the
  durable name is a string or a hash of one, and the id is looked up from it.

## Build variants

`variants:` names the define and NitroFS-file changes that distinguish builds
of the same project. Project `files:` are the base; a variant entry with the
same destination replaces its source:

```yaml
files:
  sp/message/title.bin: nitrofs/en/title.bin

variants:
  en:
    defines: GAME_LANGUAGE_EN
  fr:
    defines: [GAME_LANGUAGE_FR, MESSAGE_LANGUAGE=2]
    files:
      sp/message/title.bin: nitrofs/fr/title.bin
```

Build one with `ncpatcher build --variant fr`. When a project declares
variants, an ordinary build requires an explicit selection so it cannot
silently produce the wrong language. Command-line `-D` values come after the
variant definitions and therefore retain precedence.

`ncpatcher build --all-variants` builds entries in declaration order from the
original `rom.file`. Each output adds `_<variant>` before the extension: a base
output of `build/game.nds` produces `build/game_en.nds` and
`build/game_fr.nds`. Without `rom.output` or `--out`, the input ROM name is the
base. All-variant builds require direct `.nds` input because an extracted
directory has only one in-place destination.

## Modules

A module is a self-contained feature (its own sources, include directories,
defines and filesystem entries) that a project switches on by name. It exists
so a project stops having to say *where* every file goes: the module says that
once, and the project says only whether it wants it.

Modules live in one directory, one subdirectory each, and each carries a
`module.yaml`:

```yaml
id: Coop                 # becomes MODULE_COOP, and names it in `requires`
name: NSMB Co-op
description: Adds co-op multiplayer support to the game.
authors: [TheGameratorT, Shadey21]
repo: https://github.com/ShaneDoyle/nsmb-coop/

targets:                 # arm7, arm9, arm7(N), arm9(N)
  - arm9:
      includes: include
      sources: "source9/**"

components:
  - SpikeBassFix:
      target: arm9(58)
      sources: "source9/fixes/SpikeBass.cpp"
      defines:
        - COOP_FIX_SPIKE_BASS_ADD_ZONE_ID_FIELD=1
        - COOP_FIX_SPIKE_BASS_ZONE_ID_FIELD_OFFSET=0x4B8

  - SpikeBassSpawnerFix:
      target: arm9(58)
      requires: SpikeBassFix
      sources: "source9/fixes/SpikeBassSpawner.cpp"

  - PauseMenuFix:        # no target: a switch, and nothing else
      target: arm9
      defines: COOP_FIX_PAUSE_MENU
```

`targets:` is the catch-all, a directory swept into one region. `components:`
carve exceptions out of it: a component's sources go where *it* says, and are
removed from whatever the catch-all would have done with them. Switch a
component off and its sources leave the build entirely.

The project side:

```yaml
modules:
  dir: modules                        # default
  dump: build/generated/modules.json  # optional; see below
  enabled:
    - coop
    - mini-hacks
    - dsimodewarn:
        components:
          DSiModeScene: { target: arm9(9) }   # move it into an overlay
    - debug: false                             # listed, switched off
    - nitrosdk: { optional: true }             # may simply not be installed
```

A component override takes `enabled`, `target`, and `defines`, the last being
values for defines the component already declares, not a place to invent new
ones. A target the module wrote with a leading `!` is locked, and an override of
it is reported rather than quietly dropped.

`MODULE_<ID>` is defined for every processor a module reaches, so code can ask
whether a module is present without the project having to say so twice.

### Regions

A component targeting `arm9(58)` needs the ARM9 target to have an `ov58` region.
It is an error if it does not: a mistyped overlay id would otherwise become an
overlay full of code the game never loads, and an invented region has no size
limit worth the name, so the first thing it would do is let that code run past
the end of its overlay into the next one.

Declaring one region per overlay a module might ever reach is not the answer
either. That is what `region-catalog` is for.

## Region catalogs

Which overlays a game has, and how far each one may grow before it runs into
whatever the game placed after it, is a property of the game, not of your
project. One table serves every project built against that game, so it lives
outside the project and is referenced rather than copied. A copied table goes
stale silently, and a stale ceiling is an overlay that overruns its neighbor.

```yaml
targets:
  arm9:
    region-catalog: ${env.NSMBREF_ROOT}/overlays9.yaml
    regions:
      - dest: main
        overwrites: [[0x02026CE0, 0x02039170]]
      - dest: ov58
        maxsize: 0x4000      # this project knows better than the catalog
```

The catalog itself holds nothing but the overlays:

```yaml
version: 1
regions:
  - dest: ov0
    maxsize: 0x33C00
  - dest: ov1
    maxsize: 0x56400
```

Only `dest`, `address`, `maxsize` and `compress` are accepted there. Sources,
flags and defines are the project's business, so a catalog cannot smuggle them
in.

**A catalog entry is an offer, not a declaration.** An overlay nothing is built
into is dropped rather than written out, which is what makes listing all 131 of
them cost nothing. Name a region in the target and it is yours: whatever you say
wins, and whatever you leave out still comes from the catalog, so naming an
overlay to put sources in it does not mean restating a size limit you have no
opinion about.

An overlay in neither the catalog nor the target is still an error, naming the
overlay and the catalog that failed to list it.

For a project that uses modules, a region the target declared itself and that
nothing ended up in is dropped too, but only when it does nothing but append.
A `replace` region reserves space and an `overwrites` region blanks code, and
both mean something with no sources at all.

### The dump

`ncpatcher modules dump` prints the resolved graph as JSON, and
`modules.dump:` writes the same file before the pre-build commands run. That is
the boundary: everything game-specific (object id allocation, profile tables,
filesystem maps) belongs to a tool that reads this file, not to NCPatcher.
Component keys NCPatcher does not know are kept verbatim under `extra` and
re-emitted for exactly that reason.

```
ncpatcher modules list                     # what is on, and what each contributes
ncpatcher modules explain Coop.SpikeBassFix
ncpatcher modules explain coop
```

`explain` answers where a component's target came from, why it is disabled, and
which files and defines it accounts for.

The schema is `schema/modules.schema.json`.

### Keys NCPatcher does not define

`extra` is the extension point, and it exists at two levels. Component keys have
always been kept; module root keys are kept the same way:

```yaml
# module.yaml
id: Glue

level-data:                  # NCPatcher has no idea what this is
  stageObjects: Glue::StageObject::ModuleStageObject[?]

components:
  - Vanilla:
      objects:               # nor this
        - name: CoopFlagActor
          type: actor
```

Both come back out of the dump under `extra`, with YAML's scalar types resolved
the way YAML would resolve them, so a number stays a number and `yes` stays a
string. Nothing else happens to them. That is the whole of how a game-specific generator
extends a module without NCPatcher acquiring any knowledge of the game:

```jsonc
{ "id": "Glue",
  "extra": { "level-data": { "stageObjects": "Glue::StageObject::ModuleStageObject[?]" } },
  "components": [] }
```

A root key that NCPatcher *does* define is validated as usual, so `extra` is not
an escape from spelling `targets` correctly, only a place for keys that were
never NCPatcher's to check.


## Patches

Some patch types come defined to allow the programmer to define how the code should be modified. \
Such patches are defined by using one of the following patterns on a source file. \
*NOTE: The word "hook" can be used to refer both to any patch that causes a jump to your code and to the hook patch type.*

Patches are divided into 2 types, static and dynamic patches. \
**Static patches**: Patches that allow you to change the instructions at "compile time". These are directly applied to the binary. \
**Dynamic patches**: Patches that can be stored in the binary unapplied and only be applied at runtime when requested by the code.

### Static Patching

**To replace instructions in the binary with jumps, calls or hooks use:**

Note: \
If the code you want to hook from is THUMB then for `ncp_jump` and `ncp_call`,
you can prefix them with a "t" (eg. `ncp_tjump`, `ncp_thook`) or use the address+1. \
This is valid for the `ncp_set` variants as well (eg. `ncp_set_tjump`, `ncp_set_tcall`). \
"hook" patch type only supports hooking from ARM mode.

〇 Function tags (not stackable - only 1 hook per function)

C/C++:

```
ncp_jump(int address, [int overlay])
ncp_call(int address, [int overlay])
ncp_hook(int address, [int overlay])
```

Example:
```
ncp_jump(0x02000000)
void MyFunction1() {
    // A jump to this function will be placed at 0x02000000
}

ncp_call(0x02010000, 0)
void MyFunction2() {
    // A call to this function will be placed at 0x02010000 in overlay 0
}
```

〇 Labels (stackable - multiple hooks per function)

C/C++:

```
ncp_set_jump(int address, [int overlay], void* function)
ncp_set_call(int address, [int overlay], void* function)
ncp_set_hook(int address, [int overlay], void* function)
```

Example:
```
void MyFunction() {}

ncp_set_jump(0x02000000, MyFunction)    // A jump to MyFunction will be placed at 0x02000000
ncp_set_jump(0x02000004, MyFunction)    // A jump to MyFunction will be placed at 0x02000004
ncp_set_call(0x02010000, 0, MyFunction) // A call to MyFunction will be placed at 0x02010000 in overlay 0
ncp_set_call(0x02010004, 0, MyFunction) // A call to MyFunction will be placed at 0x02010004 in overlay 0
```

Assembly:

```
ncp_jump(int address, [int overlay])
ncp_call(int address, [int overlay])
ncp_hook(int address, [int overlay])
```

Example:
```
ncp_jump(0x02000000)    // A jump to MyFunction will be placed at 0x02000000
ncp_jump(0x02000004)    // A jump to MyFunction will be placed at 0x02000004
ncp_jump(0x02010000, 0) // A call to MyFunction will be placed at 0x02010000 in overlay 0
MyFunction:
    BX      LR
```

**To overwrite chunks of the binary with raw data or setting the destination address of a function use:**

C/C++:

```
ncp_over(int address, [int overlay])
ncp_repl(int address, [int overlay], char assembly[])
```

Example:
```
ncp_over(0x02000000)
void MyFunction() {} // This function will be placed at 0x02000000

ncp_over(0x02010000, 4)
int MyArray[] = {}; // This array will be placed at 0x02010000 in overlay 4

ncp_repl(0x02000000, "MOV R0, R0") // This instruction will be placed at 0x02000000

ncp_repl(0x02010000, 0, R"(
    MOV     R0, R0
    BX      LR
    .int    0
    .int    0
)") // This assembly code will be placed at 0x02010000 in overlay 0
```

Assembly:

```
ncp_over(int address, [int overlay])
ncp_endover()
```

Example:
```
ncp_over(0x02000000) // Places the following code at 0x02000000
    MOV     R0, #1
    MOV     R1, R0
    BX      LR
ncp_endover():
```

### Dynamic Patching

The `ncprt_*` names stand for the NCPatcher Runtime: unlike the static patches
above, which are spliced into the binary at build time, these write to memory
while the ROM is running.

C/C++:
```
void ncprt_set(int address, int value);
void ncprt_set_jump(int address, void* function);
void ncprt_set_call(int address, void* function);
void ncprt_repl(int address, char name[]);

ncprt_repl_type(name)
```
	
Example:
```
class MyClass
{
public:
    void MyFunction(); // -Wno-pmf-conversions flag must be set to be used in ncprt_set_jump/call/hook
    static void MyStaticFunction();
};

void MyFunction() {}
void MyClass::MyFunction() {}
void MyClass::MyStaticFunction() {}

ncprt_repl_type(patch0) // Must be a unique name for each patch
void MyPatch()
{
asm(R"(
    MOV     R0, #0
    BX      LR
)");
}

void MyPatcher()
{
    ncprt_set(0x02000000, 0);                              // Sets the value at 0x02000000 to 0
    ncprt_set_jump(0x02000004, MyFunction);                // Sets the value at 0x02000004 to a jump to MyFunction
    ncprt_set_call(0x02000008, &MyClass::MyFunction);      // Sets the value at 0x02000008 to a jump to MyClass::MyFunction
    ncprt_set_call(0x0200000C, MyClass::MyStaticFunction); // Sets the value at 0x0200000C to a jump to MyClass::MyStaticFunction
    ncprt_repl(0x02000010, patch0);                        // Writes the contents of patch0 (MyPatch) to 0x02000010
}
```

### Technical Details

These are the sizes occupied by branch type patches: \
The number of bytes is how many bytes are overwritten at the specified address, the ones after
the "+" sign are bridge bytes. Those are automatically generated instructions from which the
address to patch will branch to.

| Patch Type | ARM->ARM                  | ARM->THUMB                | THUMB->ARM | THUMB->THUMB |
|------------|---------------------------|---------------------------|------------|--------------|
| jump       | 4 bytes                   | 4 bytes + 8 bridge bytes  | 8 bytes    | 8 bytes      |
| call       | 4 bytes                   | 4 bytes                   | 4 bytes    | 4 bytes      |
| hook       | 4 bytes + 20 bridge bytes | 4 bytes + 20 bridge bytes | -          | -            |

**IMPORTANT NOTE** \
This means that all hooks made from ARM mode to any target will always be safe because it only
ever overwrites one instruction, but when hooking from THUMB, 4 or 8 bytes are always
overwritten depending on the hook type used and not just 2 bytes. So be careful because
you might accidentally overwrite more instructions than you intended to!

A jump patch is equivalent to a branch instruction (`B srcAddr`). \
A call patch is equivalent to a linked branch instruction (`BL srcAddr`).

If the patch type is a ARM->THUMB jump, the instruction at
`destAddr` becomes a jump to a ARM->THUMB jump bridge generated
by NCPatcher and it should look as such:

```
arm2thumb_jump_bridge:
    LDR   PC, [PC,#-4]
    .int: srcAddr+1
```

If the patch type is a hook, the instruction at
`destAddr` becomes a jump to a hook bridge generated
by NCPatcher and it should look as such:

```
hook_bridge:
    PUSH {R0-R3,R12}
    BL   srcAddr        @ BLX if srcAddr is THUMB
    POP  {R0-R3,R12}
    @<unpatched destAddr's instruction>
    B    (destAddr + 4)
```
