# Command line

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
          rom extract DIR [--code-only] [--decompress-overlays]
                                     write the ROM into DIR
          rom pack DIR               fold an extracted directory back into the ROM
          files plan [--json] [-o PATH]
                                     report what a build would place, without building
          version
```

Options, which may be written before or after the subcommand:

| Option | Meaning |
|---|---|
| `-C, --project PATH` | Project directory, or the configuration file itself |
| `--rom PATH` | ROM to patch: a `.nds` or an extracted directory. Overrides `rom.file`/`rom.dir`, and stands in for them when the project names neither |
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
| `--message-format human\|json` | See [Machine-readable output](#machine-readable-output) |
| `--result PATH` | Write a JSON summary of the run |

`-C` is what removes the "must be launched from the project directory"
constraint, so a ROM editor or a build script no longer has to `cd` first.

[Configuration](configuration.md) describes the project file these options
override.

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

## Output

A default build prints one line per phase milestone plus any warnings or
errors, not a trace of what it did internally. Each target's milestones are
grouped under its own name, so it is always clear whether arm7 or arm9 is
being worked on:

```
    Building  arm7, arm9  (variant hd)
   Inserting  214 files, 3 archives repacked

  arm7
   Compiling  8 files in 1.2s
     Linking  newcode 0x023C8000, 1420 B code + 64 B bss
    Patching  3 hooks, 2 overwrite regions (3.1/4.0 KiB used)

  arm9
   Compiling  143 files in 12.4s
     Linking  newcode 0x02100000, 4.2 KiB code + 1.1 KiB bss
    Patching  37 hooks, 2 overwrite regions (7.9/8.0 KiB used), 3 overlays

     Writing  out.nds  16.4 MiB (+1.2 MiB)
    Finished  18.7s
```

Every line this collapses (which library paths were searched, the ELF patch
tables, one line per inserted file, and so on) still exists; it goes to the
log file, in full, whether or not `-v` was given. `-v` / `--verbose-tag`
brings the same detail to the console as well.

## Machine-readable output

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

To do that, NCPatcher asks the compiler for machine-readable diagnostics rather
than scraping the text it prints for people. Which flag that is depends on the
toolchain, so it is asked rather than assumed: `-fdiagnostics-format=sarif-stderr`
where GCC accepts it (13 and later), `json` on older ones (9 through 15), and
neither on anything else, where the compiler's own output is passed through
unparsed and diagnostics arrive as one block of text rather than as located
messages. The probe compiles an empty file once per build, because GCC validates
the argument only when there is something to compile — GCC 16 accepts
`-fdiagnostics-format=json` on a command line that does no work and then refuses
the same flag when a file is named.

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

## Exit codes

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

## Cancelling a build

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
