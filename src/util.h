#pragma once

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace oclgdb {

std::string format(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

std::vector<std::string> split(const std::string &s, char sep);
std::vector<std::string> tokenize(const std::string &s);
std::string trim(const std::string &s);
bool starts_with(const std::string &s, const std::string &p);
bool ends_with(const std::string &s, const std::string &p);
std::string basename_of(const std::string &path);
std::string dirname_of(const std::string &path);

/* Parse an integer, accepting 0x / 0b / decimal. Returns false if not fully consumed. */
bool parse_int(const std::string &s, int64_t &out);

/* Read a whole file; returns false if it could not be read. */
bool read_file(const std::string &path, std::string &out);

/* ANSI styling, suppressed when stdout is not a tty or --no-color was given. */
namespace color {
extern bool enabled;
const char *bold();
const char *dim();
const char *red();
const char *green();
const char *yellow();
const char *blue();
const char *cyan();
const char *reset();
}

} // namespace oclgdb
