// Checks.h -- the simulator's own tests, run from the command line.

#pragma once

namespace Kestrel {

// --self-test: builds small circuits and checks the simulator and the file format.
int RunSelfTest();

// Counts a failure (and says what) unless condition holds.
void Check(bool condition, const char* what);
extern int testFailures;

}  // namespace Kestrel
