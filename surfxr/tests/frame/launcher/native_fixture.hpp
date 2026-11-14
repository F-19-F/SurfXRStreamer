#pragma once
#include "controller.hpp"
namespace surfxr::launcher_test {
int child_fixture(int argc, char** argv);
void lifecycle_checks(surfxr::launcher::Paths paths, const surfxr::launcher::Json& network);
void surfxr_checks(surfxr::launcher::Paths paths, const surfxr::launcher::Json& network);
void ipc_checks(surfxr::launcher::Paths paths, const surfxr::launcher::Json& network);
void deployment_checks(const surfxr::launcher::fs::path& home);
}
