# Configuration

New projects use one `ncpatcher.yaml` with `version: 2`. Existing version 1
`ncpatcher.json` projects remain supported; `ncpatcher migrate` prints their v2
equivalent and `ncpatcher migrate --write` saves it after verifying that the
resolved build is unchanged.

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

## A project with no code

`targets:` is optional. Most of a DS game is assets, and a project that only
replaces some of them compiles nothing, so there is no ARM binary for it to
name:

```yaml
version: 2

rom:
  file: rom.nds
  output: build/rom.nds
  backup: backup

file-trees:
  - dir: nitrofs
```

That is a complete project. It needs no toolchain — none is looked for when
there is nothing to compile — and it leaves the ROM's code untouched, byte for
byte. Everything else still works the same: [`files:`, `file-trees:`, archive
members](nitrofs.md), [the banner](rom.md#replacing-the-rom-banner), variants,
[hooks](hooks.md), [`files plan` and the file manifest](file-manifest.md).

Modules work too, and an assets-only project can enable a module that ships only
a `nitrofs:` tree. What it cannot do is enable a module that also ships code:
that code reaches the ROM by being folded into a target's regions, so with no
target declared there is nowhere for it to land, and NCPatcher says so rather
than building a ROM the patch is quietly missing from.

A project with no targets *and* nothing to place is refused. A build that read a
ROM and wrote it back unchanged would be reporting success for doing nothing.

## Regions

| Key | Meaning |
|---|---|
| `dest` | Required: `main`, `ovNN`, or `rt:<name>` for a runtime module. |
| `mode` | `append` (default), `replace`, or `create`. `replace` and `create` apply to overlays. |
| `address` | Required for `create`; optional for `replace` when relocating the overlay. |
| `maxsize` | Refuse a region that grows beyond this size. The default is 1 MiB. |
| `compress` | BLZ-compress an overlay when that makes it smaller. A main region is written uncompressed. |
| `sources` | Source paths and globs. `*`, `?`, character classes, `{a,b}`, whole-segment `**`, and leading `!` exclusions are supported. |
| `defines`, `flags` | Compile settings inherited from the target and optionally adjusted here. |
| `overwrites` | Original-binary address ranges this region may reuse, written as `[start, end]` pairs. |
| `file` | Runtime modules only: NitroFS path, or list of paths, to write the module's image to. |
| `output` | Runtime modules only: host path, or list of paths, to write the module's image to. Relative paths start at the target's workdir. |

ARM7 and ARM9 normally locate their ArenaLo pointer automatically. Set the
target's `arena-lo` only when using a game or binary whose initialization code
the finder does not recognize. A `create` region must use the next contiguous
overlay ID and provide its load address.

### Runtime modules

A region whose `dest` is `rt:<name>` links into a relocatable image of its own
instead of into the binary or an overlay. The game loads and unloads that image
while it runs, and the module's patches apply when it loads and revert when it
unloads. The name may contain only letters, digits and `_`.
[The NCP Runtime](runtime.md) describes how the game loads one.

```yaml
regions:
  - dest: rt:water4C
    sources: rcm/Course/4C/**
    output: ../rom/root/data/Course/4C_arc/code.rcm
  - dest: rt:nwav
    sources: modules/nwav/rt/**
    file: z_new/ncprt/nwav.ncrt
```

A runtime module needs at least one `file` or `output`. `mode`, `address`,
`maxsize`, `compress` and `overwrites` are refused, since the image is relocated
to wherever the game loads it. `sources`, `defines` and `flags` work as they do
for any other region. A runtime module cannot be listed in a region catalog.

A module links on its own, after the target's main link. It can call the
game's functions and the project's own code, and the main link keeps whatever
project code a module uses even when the main binary never calls it. A symbol
that both the module and the project define is a link error. The image is
written to every `file` in NitroFS and every `output` path, and a copy is kept
as `<build>/rt/<name>/<name>.ncrt`.

A `file` gets its id when the other NitroFS files do, before anything is
compiled, so the [file manifest](file-manifest.md) lists it, marked with
`runtime-module`, and code can refer to the module by id. It cannot be a member
of an archive. A game that keeps modules inside its archives, as Mario Kart DS
keeps one per course, writes them to `output` paths and packs the archives with
its own tools.

A [module](modules.md#runtime-modules) can put code in a runtime module with a
target of `arm9(rt:<name>)`, and create one the project does not declare. A
project region with the same `dest` takes that code, and its `file` or
`output`, when it gives one, replaces the module's.

A target may also take its overlay regions from a shared table; see
[Region catalogs](modules.md#region-catalogs).

## Variables

`vars:` declares lazily expanded project values. Use `${vars.NAME}` to read
one and `--var NAME=VALUE` to override it. `${env.NAME}` reads the environment;
`${env.NAME:-fallback}` supplies a default. Built-in references include
`${project.root}`, `${config.dir}`, `${rom.dir}`, `${target.name}` and
`${target.build}`. `${ncp.moduleDump}` is available when `modules.dump` is set,
and `${ncp.fileDump}` when `files-dump` is.
`${variant.name}`, `${rom.output}` and `${rom.file}` are available in hooks; see
[Build hooks](hooks.md) for why they are only resolved there. Write `$$` for a literal dollar sign.

## The environment file

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
original `rom.file`. Each variant's output adds `_<variant>` before the
extension, whether it is built alone or with the rest: a base output of
`build/game.nds` produces `build/game_en.nds` and `build/game_fr.nds`, so one
variant never overwrites another. Without `rom.output`, the input ROM name is
the base. A single build given `--out` writes exactly there, with no suffix.
All-variant builds require direct `.nds` input because an extracted directory
has only one in-place destination, which a single variant build patches as it
is.
