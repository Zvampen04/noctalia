#include "ipc/cli.h"

#include <clocale>
#include <vector>

int main(int argc, char* argv[]) {
  std::setlocale(LC_ALL, "");
  std::setlocale(LC_NUMERIC, "C");

  // Share the existing `noctalia msg` parser and wire protocol without linking
  // the shell, its global initializers, or its graphics/service dependencies.
  char command[] = "msg";
  std::vector<char*> forwarded;
  forwarded.reserve(static_cast<std::size_t>(argc) + 2);
  forwarded.push_back(argv[0]);
  forwarded.push_back(command);
  for (int i = 1; i < argc; ++i)
    forwarded.push_back(argv[i]);
  forwarded.push_back(nullptr);
  return noctalia::ipc::runCli(argc + 1, forwarded.data());
}
