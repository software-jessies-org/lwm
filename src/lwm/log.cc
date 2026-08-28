#include "log.h"

#include <iostream>
#include <sstream>

#include <cstring>
#include <ctime>

Log::Log(const char* level,
         const char* file,
         const int line,
         int exit_code,
         bool do_it)
    : exit_code_(exit_code), do_it_(do_it) {
  if (!do_it_) {
    return;
  }
  time_t t = time(nullptr);
  struct tm* tm = localtime(&t);
  char time_buf[100];
  strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S", tm);
  buf_ << level << " " << time_buf << " " << file << ":" << line << ": ";
}

Log::~Log() {
  if (!do_it_) {
    return;
  }
  std::cerr << buf_.str() << "\n";
  if (exit_code_) {
    exit(exit_code_);
  }
}

Log& Log::operator<<(const Errno& e) {
  const char* es = strerror(e.Num());
  buf_ << "errno=" << e.Num() << " (" << es << ")";
  return *this;
}

Log& Log::operator<<(const ExitCode& e) {
  exit_code_ = e.Code();
  return *this;
}
