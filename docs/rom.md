# The ROM

The `rom:` section names the ROM a project patches, where the result goes, and
a few things about how it is written.

NCPatcher patches a `.nds` directly:

```yaml
rom:
  file: roms/nsmb.nds     # patched in place
  output: build/out.nds   # ...unless you say where to put the result
  backup: backup
```

Without a `files:` section it changes only the header, ARM binaries, overlay
tables and overlay files. A [`files:` section](nitrofs.md) may deliberately replace
or add NitroFS files. The ROM is edited in place when its existing layout has room;
when an ARM binary or table outgrows that room, NCPatcher lays the container out
again without changing unrelated file contents.

An extracted directory works too, and is what a level editor hands it:

```yaml
rom:
  dir: __tmp
  backup: backup
```

A project may also name neither, and leave the ROM to `--rom`: a tool that
keeps the clean ROM's location to itself, as an editor does per machine, then
drives a project whose configuration is the same everywhere. `backup` and the
rest of `rom:` still apply. Without `--rom`, such a project is refused.

```yaml
rom:
  output: build/out.nds   # with ncpatcher --rom roms/nsmb.nds build
  backup: backup
```

`ncpatcher rom extract <dir>` writes that directory out of a `.nds`, and
`ncpatcher rom pack <dir>` folds it back in, which is the job most projects
currently do with a script of their own.

An extraction is the whole ROM: the header, the two ARM binaries, the overlay
tables and overlays, the name and allocation tables, the banner, and the NitroFS
tree below `data/`. That set is chosen so the directory is one NCPatcher can be
pointed straight back at — `rom info`, `rom files` and `rom: dir:` all read it —
because an extraction that stopped at the code binaries was only ever half of a
ROM, and every tool reading one had to know which half. `--code-only` asks for
just the binaries.

Alongside them goes an `extraction.json`, which answers the two questions the
loose files cannot: which layout their names follow, and what form each overlay
was stored in. Its schema is `schema/extraction.schema.json`.

Overlays are written exactly as the ROM stores them, compression and all, and a
plain extract/pack round trip gives back the ROM byte for byte. That default is
not an oversight: a directory whose bytes disagree with its own overlay table is
a trap for everything that repacks one.

`--decompress-overlays` unpacks them **and** clears the compression flag in the
emitted `arm9ovt.bin`, because the two only make sense together — decompressed
bytes under a row still saying `compressed, compressedSize = N` is the one
combination that turns a naive repack into a ROM that hangs. `extraction.json`
records what the ROM had, so `rom pack` compresses the overlay again and
restores its flags.

What that round trip guarantees is the contents: the overlay comes back
compressed, with the ROM's flags, unpacking to exactly the bytes it did before.
It also happens to come back byte for byte on every overlay of a retail NSMB
ROM, because this encoder makes the same choices as the tooling that wrote them
and pads the footer slack with `0xFF` the way every other BLZ tool does.

That is worth having, and worth testing, but it is not a promise. Mario Kart DS
is the counter-example: its overlay 0 comes back identical, and overlays 1 and 3
come back as different streams of the same length holding the same contents,
because whatever compressed them split the raw head somewhere else. Byte
identity is a property of the encoder that happened to write the ROM; the
contents are the part this guarantees.

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

## Growing ARM9

An `append` region adds code to the end of ARM9, and ARM9 cannot be moved: the
secure area is encrypted against its being at the offset the header names. So
the first build that outgrows the gap before the overlay table lays the ROM out
again, leaving `rom.arm9-slack` bytes (64 KiB by default) of room behind it.
Because every build re-applies its patches to the *pristine* ARM9 rather than
to the last build's output, the patched size is a function of the code and not
of how many times you have built, so that happens once, and later builds write
in place and produce a byte-identical ROM.

## Replacing the ROM banner

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

## Making the ROM DSi-enhanced

`rom.dsi-enhanced: true` writes the ROM with a DSi (TWL) header, so a DSi-mode
loader keeps the console in DSi mode for a game built with the DS SDK:

```yaml
rom:
  file: rom.nds
  output: build/game.nds
  backup: backup
  dsi-enhanced: true
  ds-copy: build/game-ds.nds   # optional
```

The unit code becomes 02h, the DSi fields past 0x180 are filled in (all
NWRAM to the ARM9, SCFG access kept, touchscreen/sound controller left in DS
mode, region free) and two 1 KiB inert ARM9i/ARM7i stubs are placed after the
DS data on a 512 KiB boundary. The game code is unchanged, so saves and cheats
keep working. The result is unsigned: Unlaunch, TWiLight Menu++ and emulators
boot it in DSi mode, the stock DSi menu does not. It needs `rom.file`, and
refuses a ROM whose 0x1000-0x3FFF is not empty, a DSi-only game, and a game
that is already DSi-enhanced with ARM9i/ARM7i binaries of its own.

`ds-copy` (which needs `output`) also saves the ROM with its original DS
header. nds-bootstrap treats any DSi unit code as a DSi-SDK title, so that is
the file to give it, with the game set to run in DSi mode.

## Trimming the ROM

`rom.trim: true` writes the ROM without the padding past its used size, the
way ndspy builds it. A dumped cartridge is padded with 0xFF up to its chip
size; trimmed, it ends at the header's total used size, followed by the
download play signature when the ROM has one. On a DSi ROM the TWL area is
kept as well. It needs `rom.file`.
