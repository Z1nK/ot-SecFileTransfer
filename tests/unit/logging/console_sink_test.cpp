#include <logging/tech_log.hpp>

#include <gtest/gtest.h>

#include <iostream>
#include <sstream>
#include <streambuf>

using confide::logging::ConsoleSink;
using confide::logging::LogLevel;

namespace {

// Redirects std::cout into a string for the lifetime of the object.
class CoutCapture {
public:
  CoutCapture() : old_(std::cout.rdbuf(buf_.rdbuf())) {}
  ~CoutCapture() { std::cout.rdbuf(old_); }

  CoutCapture(const CoutCapture&) = delete;
  CoutCapture& operator=(const CoutCapture&) = delete;
  CoutCapture(CoutCapture&&) = delete;
  CoutCapture& operator=(CoutCapture&&) = delete;

  std::string str() const { return buf_.str(); }

private:
  std::ostringstream buf_;
  std::streambuf* old_;
};

}  // namespace

TEST(ConsoleSink, WritesEachRecordOnItsOwnLine) {
  CoutCapture cap;
  ConsoleSink sink;
  sink.write(LogLevel::info, R"({"msg":"a"})");
  sink.write(LogLevel::error, R"({"msg":"b"})");

  EXPECT_EQ(cap.str(), "{\"msg\":\"a\"}\n{\"msg\":\"b\"}\n");
}
