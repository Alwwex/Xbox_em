#pragma once

// run --log-reads PATTERN: file operations (CreateFile, SetFilePointer(Ex),
// ReadFile, DirectStorage requests) on files whose guest path contains
// PATTERN (case-insensitive) are logged with offsets, sizes, destinations
// and the calling code - to see how a title reads an archive.

#include <string>

namespace xbem::hle {

void set_file_read_log(const std::string& pattern);
bool file_reads_logged(const std::string& guest_path);
// Counts one logged operation; false once the log limit is reached.
bool take_read_log_slot();

}  // namespace xbem::hle
