# NitroFS files

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

Variants can swap a destination's source; see
[Build variants](configuration.md#build-variants).

This works directly on `.nds` inputs and on complete extracted layouts that
include `fnt.bin`, `fat.bin`, and the configured `data-dir`.

## Reserving the first new file ID

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

## Claiming an existing file ID

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

## Editing a file inside a Nitro archive

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
its file id -- the archive's own name table numbers its members exactly as the
ROM's FNT numbers loose files -- so inserting one would renumber every member
after it, the same reason NitroFS file IDs are never renumbered. The archive must already exist
and must already hold the member named; a missing member is an error rather
than a warning, because the way that ships is a ROM with the translation still
in the original language.

A replacement of a different size is fine. The allocation table and the data
chunk are laid out again around it, and every member keeps its id. An
archive nothing edited is written back byte for byte.

`id:` cannot be combined with an archive destination, since it renames a loose
file and a member of an archive is not one.

The manifest records what happened inside: see
[Members of an edited archive](file-manifest.md#members-of-an-edited-archive).

### Compressed archives

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
