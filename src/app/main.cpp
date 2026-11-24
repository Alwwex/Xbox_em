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
      "       xbem --version\n"
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
      "  func <file> <rva>... | -        the function an RVA is in (.pdata) and an objdump command for it;\n"
      "                                  several RVAs or objdump lines on stdin (-): one line each\n"
      "  gpu-disasm <file> [--offset N] [--size N] [--all]\n"
      "                                  disassemble RDNA2 (Xbox Series GPU) shader code\n"
      "\n"
      "execution (Linux x86-64 / WSL2):\n"
      "  run <dir|file.exe> [options] [-- guest args]\n"
      "      --strict                    stop at the first call to an unimplemented import\n"
      "      --trace-hle                 log calls to emulated functions; crash reports and Ctrl+C\n"
      "                                  dumps show each thread's last 64 calls\n"
      "      --continue-after-breakpoint log int3/__debugbreak() in the title and keep running\n"
      "      --trace-at ADDR[,ADDR...]   tracepoints: log registers and callers when the title's code\n"
      "                                  reaches ADDR (module!export[+hex], module+hex, or a hex RVA\n"
      "                                  in the executable); crash reports list the last hits.\n"
      "                                  ADDR?reg=hex / ADDR?reg!=hex logs only matching hits;\n"
      "                                  ADDR@reg+off/len or ADDR@[reg+off]+off/len dumps memory\n"
      "                                  with each logged hit (up to 4 ranges, 1024 bytes each)\n"
      "      --trace-log N               hits of each tracepoint logged as they happen (default 8)\n"
      "      --watch ADDR[/LEN][,...]    data watchpoints: log every write into LEN bytes (default 8,\n"
      "                                  at most 64) with the writing code; ADDR is module!export,\n"
      "                                  module+hex, a hex RVA or address, or alloc:CALLER+OFFSET -\n"
      "                                  OFFSET into the memory the function at CALLER allocates\n"
      "      --watch-log N               writes of each watchpoint logged as they happen (default 16)\n"
      "      --log-reads TEXT            log CreateFile/SetFilePointer/ReadFile/DirectStorage on files\n"
      "                                  whose path contains TEXT (offsets, sizes, destinations, callers)\n"
      "      --no-window                 no run window (by default one opens when DISPLAY is set:\n"
      "                                  status and the title's back buffer, WSLg on Windows 11)\n"
      "      --dump-shaders DIR          save the GPU shader binaries the title creates\n"
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
    } else if (args[i] == "--version") {
      std::printf("xbem %s (commit %s)\n", XBEM_VERSION, XBEM_GIT_COMMIT);
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
    if (command == "run") {
      XBEM_INFO("xbem", "xbem {} (commit {})", XBEM_VERSION, XBEM_GIT_COMMIT);
      return xbem::app::cmd_run(rest);
    }
    if (command == "hexdump") return xbem::app::cmd_hexdump(rest);
    if (command == "func") return xbem::app::cmd_func(rest);
    if (command == "gpu-disasm") return xbem::app::cmd_gpu_disasm(rest);
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
