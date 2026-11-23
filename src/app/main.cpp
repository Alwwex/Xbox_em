#include <cstdio>
#include <cstring>
#include <exception>
#include <string>
#include <vector>

#include "app/commands.h"
#include "common/log.h"

namespace {

void print_usage() {
  std::puts(
      "xbem - experimental Xbox Series X|S compatibility layer (research prototype)\n"
      "\n"
      "usage: xbem [--log-level trace|debug|info|warn|error] <command> [args]\n"
      "\n"
      "analysis (work on any host):\n"
      "  inspect <file.exe|dll>          PE headers, sections, TLS, PDB, imports with HLE coverage\n"
      "  coverage <dir> [--json F] [--markdown F]\n"
      "                                  aggregate import coverage of every PE in a game folder\n"
      "  package <dir>                   read MicrosoftGame.config and summarise a loose layout\n"
      "  xvd <file>                      read-only XVD/XVC container header info (no decryption)\n"
      "  cpu                             check the host CPU against the Xbox Series (Zen 2) ISA\n"
      "  hle [module]                    list HLE modules / implemented functions\n"
      "  hexdump <file> <rva> [len]      bytes of a PE file at an RVA (hex), e.g. code to disassemble\n"
      "\n"
      "execution (Linux x86-64 / WSL2):\n"
      "  run <dir|file.exe> [options] [-- guest args]\n"
      "      --strict                    stop at the first call to an unimplemented import\n"
      "      --trace-hle                 log calls to emulated functions; crash reports and Ctrl+C\n"
      "                                  dumps show each thread's last 64 calls\n"
      "      --device series-x|series-s|devkit\n"
      "      --scratch DIR               host directory for the writable T:\\ drive\n"
      "      --mount X=DIR               mount an extra writable drive letter\n");
}

}  // namespace

int main(int argc, char** argv) {
  std::vector<std::string> args(argv + 1, argv + argc);

  // Global options before the command.
  size_t i = 0;
  while (i < args.size() && args[i].rfind("--", 0) == 0) {
    if (args[i] == "--log-level" && i + 1 < args.size()) {
      xbem::log::Level lv;
      if (!xbem::log::parse_level(args[i + 1], lv)) {
        std::fprintf(stderr, "unknown log level: %s\n", args[i + 1].c_str());
        return 1;
      }
      xbem::log::set_level(lv);
      i += 2;
    } else if (args[i] == "--help") {
      print_usage();
      return 0;
    } else {
      break;
    }
  }
  if (i >= args.size()) {
    print_usage();
    return 1;
  }
  const std::string command = args[i];
  const xbem::app::Args rest(args.begin() + static_cast<long>(i) + 1, args.end());

  try {
    if (command == "inspect") return xbem::app::cmd_inspect(rest);
    if (command == "coverage") return xbem::app::cmd_coverage(rest);
    if (command == "package") return xbem::app::cmd_package(rest);
    if (command == "xvd") return xbem::app::cmd_xvd(rest);
    if (command == "cpu") return xbem::app::cmd_cpu(rest);
    if (command == "hle") return xbem::app::cmd_hle(rest);
    if (command == "run") return xbem::app::cmd_run(rest);
    if (command == "hexdump") return xbem::app::cmd_hexdump(rest);
    if (command == "help" || command == "-h") {
      print_usage();
      return 0;
    }
  } catch (const std::exception& e) {
    std::fprintf(stderr, "error: %s\n", e.what());
    return 1;
  }
  std::fprintf(stderr, "unknown command: %s (see xbem --help)\n", command.c_str());
  return 1;
}
