#pragma once

#include "session.h"

#include <string>
#include <vector>

namespace oclgdb {

class Cli {
public:
    explicit Cli(Session &s) : s_(s) {}

    /* Execute one command line. Returns false when the session should end. */
    bool execute(const std::string &line);
    void banner() const;
    void prompt() const;
    bool echo = false;          /* print commands as they run (script mode) */

private:
    void cmd_help(const std::vector<std::string> &a);
    void cmd_list(const std::vector<std::string> &a);
    void cmd_break(const std::string &rest);
    void cmd_info(const std::vector<std::string> &a);
    void cmd_print(const std::string &rest);
    void cmd_examine(const std::string &spec, const std::string &rest);
    void cmd_select(const std::string &rest);
    void cmd_set(const std::vector<std::string> &a);

    void report_stop();
    void show_source_line(unsigned line, bool arrow) const;
    void show_work_item_table() const;
    void show_locals(bool params_only, bool all) const;

    Session &s_;
};

} // namespace oclgdb
