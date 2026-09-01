#pragma once

// Asking a build to stop, and having it stop somewhere sensible.
//
// A ROM editor that starts a build needs a Cancel button, and until now there
// was nothing documented to send: Ctrl-C killed the process through the default
// disposition, part way through whatever it happened to be doing, and the
// caller learned only that it had died. Nothing said what to send, when it would
// take effect, or what the process would return.
//
// So: SIGINT (POSIX) or CTRL_BREAK_EVENT (Windows) to the process group asks for
// a stop. Compiler children are in that group and stop with it. The build
// notices at its next checkpoint, unwinds the ordinary way, writes its --result
// document with `status: "cancelled"`, and exits 11.
//
// Cancelling is safe by construction rather than by care. BackupStore holds the
// pristine binaries, so an interrupted build's next run starts from those rather
// than from its own half-written output; and the container backend holds every
// write until commit(), so a .nds is either written whole or not written at all.
// An extracted directory is the one that can be left mid-patch, which is the
// same thing that is true of a build that fails, and is what the backup exists
// for.

namespace ncp::cancel {

// Installs the handlers. Once, at startup, before anything long-running.
void install();

// Whether a stop has been asked for. Cheap enough for a loop condition.
[[nodiscard]] bool requested();

// Throws ncp::cancelled when one has.
//
// The checkpoints are chosen rather than sprinkled: between build phases, and
// between compilations, which are the places where stopping leaves nothing
// half-done. Nothing checks in the middle of writing a ROM.
void checkpoint();

// Forgets a request. For tests, and for a caller that has dealt with one.
void reset();

} // namespace ncp::cancel
