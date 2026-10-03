// Regnum — точка входа regnum-cli.
#include <cstdio>

#include "base/base.h"
#include "cli/cli.h"

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>  // CommandLineToArgvW (WIN32_LEAN_AND_MEAN исключает его из windows.h)
#endif

namespace rg::cli {

std::vector<Entry>& commands() { static std::vector<Entry> list; return list; }

Command::Command(const char* name, const char* usage, const char* help, Fn fn) { commands().push_back({name, usage, help, fn}); }

bool hasFlag(const std::vector<std::string>& args, const std::string& flag) {
  for (auto& a : args) if (a == flag) return true;
  return false;
}

std::string option(const std::vector<std::string>& args, const std::string& key, const std::string& def) {
  std::string p = key + "=";
  for (auto& a : args) if (a.rfind(p, 0) == 0) return a.substr(p.size());
  return def;
}

std::vector<std::string> positional(const std::vector<std::string>& args) {
  std::vector<std::string> r;
  for (auto& a : args) if (a.rfind("--", 0) != 0) r.push_back(a);
  return r;
}

}  // namespace rg::cli

static int usage() {
  std::printf("regnum-cli — инструменты редактора Regnum\n\nКоманды:\n");
  for (auto& c : rg::cli::commands()) std::printf("  %-14s %s\n                 %s\n", c.name, c.usage, c.help);
  return 2;
}

static int runMain(std::vector<std::string> args) {
  if (args.empty() || args[0] == "help" || args[0] == "--help") return usage();
  std::string name = args[0];
  args.erase(args.begin());
  for (auto& c : rg::cli::commands()) {
    if (name == c.name) {
      try {
        return c.fn(args);
      } catch (const rg::UserError& e) {
        std::fprintf(stderr, "Ошибка: %s\n", e.what());
        return 1;
      } catch (const std::exception& e) {
        std::fprintf(stderr, "Внутренняя ошибка: %s\n", e.what());
        return 3;
      }
    }
  }
  std::fprintf(stderr, "Неизвестная команда: %s\n", name.c_str());
  return usage();
}

#ifdef _WIN32
int main() {
  SetConsoleOutputCP(CP_UTF8);
  int argc = 0;
  wchar_t** wargv = CommandLineToArgvW(GetCommandLineW(), &argc);
  std::vector<std::string> args;
  for (int i = 1; i < argc; i++) args.push_back(rg::utf8::fromWide(wargv[i]));
  LocalFree(wargv);
  return runMain(std::move(args));
}
#else
int main(int argc, char** argv) {
  std::vector<std::string> args;
  for (int i = 1; i < argc; i++) args.push_back(argv[i]);
  return runMain(std::move(args));
}
#endif
