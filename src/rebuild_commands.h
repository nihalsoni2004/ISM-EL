#pragma once

#include "recovery/rebuild_job.h"

int runSpareCommand(const std::vector<std::string>& args);
int runRebuildCommand(const std::vector<std::string>& args);
void printRebuildStatus(const raid::RebuildJob& job);
