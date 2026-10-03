// Regnum — консольный инструмент regnum-cli: реестр команд.
// Команда регистрируется в своём .cpp статическим объектом:
//   static rg::cli::Command reg("validate", "<папка мира>", "Проверить целостность мира", &runValidate);
#pragma once
#include <string>
#include <vector>

namespace rg::cli {

using Fn = int (*)(const std::vector<std::string>& args);  // код возврата процесса

struct Command {
  Command(const char* name, const char* usage, const char* help, Fn fn);
};

struct Entry { const char* name; const char* usage; const char* help; Fn fn; };
std::vector<Entry>& commands();

// Простой разбор опций: --key=value и --flag.
bool hasFlag(const std::vector<std::string>& args, const std::string& flag);
std::string option(const std::vector<std::string>& args, const std::string& key, const std::string& def = {});
std::vector<std::string> positional(const std::vector<std::string>& args);

}  // namespace rg::cli
