#pragma once

// run --log-reads PATTERN: file operations (CreateFile, SetFilePointer(Ex),
// ReadFile, DirectStorage requests) on files whose guest path contains
// PATTERN (case-insensitive) are logged with offsets, sizes, destinations
// and the calling code - to see how a title reads an archive.

#include <string>

namespace xbem::hle {

void set_file_read_log(const std::string& pattern);
bool file_reads_logged(const std::string& guest_path);
// Counts one logged operation; false once its limit is reached (file
// operations and DirectStorage requests have separate limits, so a flood of
// requests does not hide the reads).
bool take_read_log_slot();
bool take_dstorage_log_slot();

// "VirtualAlloc region ..." for a destination address, or "" (for the log).
std::string describe_destination(const void* address);

}  // namespace xbem::hle
