// test.h
//
// A minimal in-tree unit test framework. No external dependencies, so it
// can be linked into either the full lwm binary or a future X11-free tier-0
// test target.
//
// Usage:
//
//   TEST(Geometry, RectParse) {
//     EXPECT_EQ(Rect::Parse("10x10+1+1"), (Rect{1, 1, 11, 11}));
//     ASSERT_TRUE(!name.empty()) << "extra context on failure";
//   }
//
// EXPECT_* records a failure and keeps running the test; ASSERT_* records a
// failure and returns from the current test function immediately. Both can
// have extra context streamed on with operator<<; it's only evaluated (and
// only appears in the output) if the check failed.
//
// Table-driven tests should wrap each case's body in a
// `testing::Context ctx(tc.name);` so failures are labelled with which case
// produced them.

#ifndef TEST_H_included
#define TEST_H_included

#include <ostream>
#include <sstream>
#include <string>
#include <vector>

namespace testing {

// Fallback so EXPECT_EQ/NE can report a readable diff for a
// std::vector<T> even when T only has its own operator<<, not the vector.
// A more specific operator<< (e.g. for std::vector<Rect>) always wins over
// this template in overload resolution.
template <typename T>
std::ostream& operator<<(std::ostream& os, const std::vector<T>& v) {
  os << "{";
  for (size_t i = 0; i < v.size(); i++) {
    if (i) {
      os << ", ";
    }
    os << v[i];
  }
  os << "}";
  return os;
}

// While alive, prefixes this label onto any Result failure reported. Nest
// freely (e.g. one per table-driven case); labels stack.
class Context {
 public:
  explicit Context(const std::string& label);
  ~Context();

  Context(const Context&) = delete;
  Context& operator=(const Context&) = delete;
};

// What the EXPECT_/ASSERT_ macros expand to. Reports the failure (if any) to
// stderr when it's destroyed, so extra context can be streamed in first.
class Result {
 public:
  Result(bool ok, const char* file, int line, const std::string& what);
  ~Result();

  Result(Result&&) = default;
  Result(const Result&) = delete;
  Result& operator=(const Result&) = delete;

  explicit operator bool() const { return ok_; }

  template <typename T>
  Result& operator<<(const T& t) {
    if (!ok_) {
      extra_ << t;
    }
    return *this;
  }

 private:
  bool ok_;
  const char* file_;
  int line_;
  std::string what_;
  std::ostringstream extra_;
};

template <typename A, typename B>
Result CheckEq(const A& a, const B& b, const char* file, int line,
               const char* expr) {
  std::ostringstream os;
  const bool ok = (a == b);
  if (!ok) {
    os << "got " << a << ", want " << b;
  }
  return Result(ok, file, line, expr + (": " + os.str()));
}

template <typename A, typename B>
Result CheckNe(const A& a, const B& b, const char* file, int line,
               const char* expr) {
  const bool ok = !(a == b);
  std::ostringstream os;
  if (!ok) {
    os << "got " << a << ", which should not equal " << b;
  }
  return Result(ok, file, line, expr + (": " + os.str()));
}

inline Result CheckTrue(bool cond, const char* file, int line,
                        const char* expr) {
  return Result(cond, file, line, std::string(expr) + ": is false, want true");
}

inline Result CheckFalse(bool cond, const char* file, int line,
                         const char* expr) {
  return Result(!cond, file, line,
                std::string(expr) + ": is true, want false");
}

template <typename A, typename B, typename Tol>
Result CheckNear(const A& a, const B& b, const Tol& tol, const char* file,
                 int line, const char* expr) {
  const auto diff = a > b ? a - b : b - a;
  const bool ok = diff <= tol;
  std::ostringstream os;
  if (!ok) {
    os << "got " << a << ", want within " << tol << " of " << b;
  }
  return Result(ok, file, line, expr + (": " + os.str()));
}

// Registers a test function under "Suite.Name" at static-init time. Don't
// use directly; the TEST() macro below does this for you.
struct Registrar {
  Registrar(const char* suite, const char* name, void (*fn)());
};

// Runs every registered test whose "Suite.Name" contains filter as a
// substring (or every test, if filter is empty). Prints one line per test
// plus a summary, and returns true iff everything passed.
bool RunAll(const std::string& filter = "");

}  // namespace testing

#define TEST(Suite, Name)                                        \
  static void Suite##_##Name##_Test();                           \
  static testing::Registrar Suite##_##Name##_Registrar(           \
      #Suite, #Name, Suite##_##Name##_Test);                     \
  static void Suite##_##Name##_Test()

#define EXPECT_EQ(a, b) testing::CheckEq(a, b, __FILE__, __LINE__, #a " == " #b)
#define EXPECT_NE(a, b) testing::CheckNe(a, b, __FILE__, __LINE__, #a " != " #b)
#define EXPECT_TRUE(a) testing::CheckTrue(bool(a), __FILE__, __LINE__, #a)
#define EXPECT_FALSE(a) testing::CheckFalse(bool(a), __FILE__, __LINE__, #a)
#define EXPECT_NEAR(a, b, tol) \
  testing::CheckNear(a, b, tol, __FILE__, __LINE__, #a " ~= " #b)

// ASSERT_* variants abandon the current test function (via `return`) on
// failure. Must only be used directly inside a TEST() body (or a void
// function called from one that's happy to be short-circuited).
//
// The trailing `<< ""` is what lets an assertion stand as a statement on its
// own: without it the else branch is a bare expression, which -Werror rejects
// as having no effect. It costs nothing, since the else branch is only
// reached when the assertion passed, and Result only buffers on failure.
#define ASSERT_EQ(a, b)                            \
  if (auto _assert_result = EXPECT_EQ(a, b); !_assert_result) return; \
  else _assert_result << ""
#define ASSERT_NE(a, b)                            \
  if (auto _assert_result = EXPECT_NE(a, b); !_assert_result) return; \
  else _assert_result << ""
#define ASSERT_TRUE(a)                              \
  if (auto _assert_result = EXPECT_TRUE(a); !_assert_result) return; \
  else _assert_result << ""
#define ASSERT_FALSE(a)                              \
  if (auto _assert_result = EXPECT_FALSE(a); !_assert_result) return; \
  else _assert_result << ""
#define ASSERT_NEAR(a, b, tol)                                \
  if (auto _assert_result = EXPECT_NEAR(a, b, tol); !_assert_result) \
    return;                                                   \
  else _assert_result << ""

#endif  // TEST_H_included
