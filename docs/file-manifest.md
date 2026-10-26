# The file manifest

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

A file a runtime module is written to (a region's `file:`) carries
`"runtime-module": "<name>"`. Its id is settled with every other file's, so a
generator can name it like any other, but the module is linked after the
manifest is written: until then the file is an empty placeholder, and its `size`
is 0.

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

## Members of an edited archive

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
    { "id": 43, "path": "menu/title/USA/vs.bmg", "size": 1344, "action": "modified",
      "source": "modules/message/nitrofs/fr/ARCHIVE/menu_title_narc/menu/title/USA/vs.bmg",
      "module": "message", "from-variant": "fr" } ] }
```

A member's `id` is a file id like any other -- in the archive's table rather
than the ROM's. A `.narc` carries the same File Name Table structure a ROM
does and numbers its members the same way, and game code loads one by that id
exactly as it loads a loose file by its own. What differs is only which table
the number belongs to, so an `id` inside `members` is read together with the
container's path and is meaningless without it, just as the enclosing entry's
`id` is meaningless inside the archive. A member's `action` is never `created`,
because archives are [edited in place and never added to](nitrofs.md#editing-a-file-inside-a-nitro-archive).

Only the members the run edited are listed. Enumerating every member of every
archive would dwarf the rest of the document, and a consumer with the ROM open
can list them itself; what it cannot work out on its own is where the bytes came
from, so that is what this carries.

Still `ncpatcher.files/1`: `members` arrived in this schema and has never
shipped a release spelling that field anything else, and a consumer that reads
only the container entry is unaffected either way.

## Planning a build

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

## The invariant a consumer can rely on

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
