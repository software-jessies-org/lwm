// tests.cc
//
// Glue between `-test` (main() in lwm.cc) and the self-registering
// testing:: framework in test.h. Test suites themselves live in `*_test.cc`
// next to what they test.

#include "test.h"

// RunAllTests runs all registered tests, then returns true on success.
bool RunAllTests() {
  return testing::RunAll();
}
