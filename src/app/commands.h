#pragma once

#include <string>
#include <vector>

namespace xbem::app {

using Args = std::vector<std::string>;

int cmd_inspect(const Args& args);
int cmd_coverage(const Args& args);
int cmd_package(const Args& args);
int cmd_xvd(const Args& args);
int cmd_cpu(const Args& args);
int cmd_hle(const Args& args);
int cmd_run(const Args& args);

}  // namespace xbem::app
