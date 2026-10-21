// SPDX-FileCopyrightText: 2026 HANsoA-KevinO
// SPDX-License-Identifier: MIT
#pragma once
// The panel hotkey's settings file, <data>\settings\panel-hotkey-v1.json, for
// the processes that may read and write files: the in-game host, the desktop
// program, the Steam launcher and overglaze_games. The format and every rule
// about which bindings are allowed live in lab_panel_hotkey.hpp, shared with
// the root proxy, which reads the same file without this library.
#include "lab_panel_hotkey.hpp"
#include <filesystem>
#include <memory>
#include <string>

namespace lab::hotkey {
// Where the binding came from.
enum class Source {
    defaulted,  // no file: Insert
    file,       // the file, valid
    malformed,  // a file that is not a valid document, or names a refused binding: Insert
    memory,     // no data root (fixtures, the viewer's regression modes): this process only
};
const char* source_name(Source) noexcept;
// <data_root>\settings\panel-hotkey-v1.json
std::filesystem::path settings_file(const std::filesystem::path& data_root);
// format() as a string; the default's name when the key has none.
std::string name(const Binding&);
// One read, never throws. A missing file is the default; a malformed one is the
// default with `note` saying why; a file that cannot be opened right now (held
// by another program) is also the default here, with the reason in `note` --
// the live Settings below keeps its last value in that case instead.
struct ReadResult {Binding binding=kDefault;Source source=Source::defaulted;std::string note;bool transient=false;};
ReadResult read(const std::filesystem::path& data_root);
// Writes the document atomically (a temporary file in the same directory, then
// a replacing rename). Creates <data_root>\settings when missing. Throws on a
// refused binding or a failed write; the old file is then left as it was.
void write(const std::filesystem::path& data_root,const Binding&);

// The live setting. Read once at construction; afterwards a worker thread looks
// at the file twice a second and re-reads it when it changed, and performs the
// writes set() asks for -- so a game's window or render thread never waits on
// the disk. Every reader of current() sees the newest binding at once.
class Settings final {
    struct Impl;std::unique_ptr<Impl> p_;
public:
    // An empty data_root: no file, no thread; set() changes this process only.
    explicit Settings(std::filesystem::path data_root={});
    ~Settings();
    Settings(const Settings&)=delete;Settings& operator=(const Settings&)=delete;
    Binding current() const noexcept;
    std::string current_name() const;
    Source source() const noexcept;
    // Increments whenever current() changes (a set(), or the file changed).
    std::uint64_t revision() const noexcept;
    // A refused binding changes nothing and is returned. Otherwise current()
    // changes now and the file follows on the worker.
    Refusal set(const Binding&) noexcept;
    // The last read or write problem in words (empty when none): a malformed
    // file, a file that could not be read, or a write that failed.
    std::string error() const;
    // Waits for a pending write, then stops the worker. Idempotent.
    void close() noexcept;
};
}
