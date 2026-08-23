#include "test.h"

#include <vector>

#include "log.h"

namespace testing {

namespace {

std::vector<std::string>& contextStack() {
  static std::vector<std::string>* stack = new std::vector<std::string>();
  return *stack;
}

int& failureCount() {
  static int n = 0;
  return n;
}

std::string contextPrefix() {
  const std::vector<std::string>& stack = contextStack();
  if (stack.empty()) {
    return "";
  }
  std::ostringstream os;
  os << "[";
  for (size_t i = 0; i < stack.size(); i++) {
    if (i) {
      os << " > ";
    }
    os << stack[i];
  }
  os << "] ";
  return os.str();
}

struct TestCase {
  std::string suite;
  std::string name;
  void (*fn)();
};

std::vector<TestCase>& registry() {
  static std::vector<TestCase>* reg = new std::vector<TestCase>();
  return *reg;
}

}  // namespace

Context::Context(const std::string& label) {
  contextStack().push_back(label);
}

Context::~Context() {
  contextStack().pop_back();
}

Result::Result(bool ok, const char* file, int line, const std::string& what)
    : ok_(ok), file_(file), line_(line), what_(what) {}

Result::~Result() {
  if (ok_) {
    return;
  }
  failureCount()++;
  // The separator matters: extra_ holds whatever the caller streamed in after
  // the assertion, and without it the message runs straight into the last
  // value the comparison printed.
  const std::string extra = extra_.str();
  LOGE() << contextPrefix() << file_ << ":" << line_ << ": " << what_
         << (extra.empty() ? "" : "; ") << extra;
}

Registrar::Registrar(const char* suite, const char* name, void (*fn)()) {
  registry().push_back(TestCase{suite, name, fn});
}

bool RunAll(const std::string& filter) {
  int total = 0;
  int passed = 0;
  for (const TestCase& tc : registry()) {
    const std::string full = tc.suite + "." + tc.name;
    if (!filter.empty() && full.find(filter) == std::string::npos) {
      continue;
    }
    total++;
    const int before = failureCount();
    Context ctx(full);
    tc.fn();
    if (failureCount() == before) {
      passed++;
      LOGI() << "PASS " << full;
    } else {
      LOGE() << "FAIL " << full;
    }
  }
  LOGI() << passed << "/" << total << " tests passed";
  return passed == total;
}

}  // namespace testing
