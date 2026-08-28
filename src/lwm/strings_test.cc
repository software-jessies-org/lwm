#include "strings.h"
#include "test.h"

TEST(Split, Basic) {
  const std::vector<std::string> got = Split("a;bb;ccc", ";");
  const std::vector<std::string> want = {"a", "bb", "ccc"};
  EXPECT_EQ(got, want);
}

TEST(Split, NoSeparatorPresent) {
  const std::vector<std::string> got = Split("solo", ";");
  const std::vector<std::string> want = {"solo"};
  EXPECT_EQ(got, want);
}

TEST(Split, EmptyString) {
  const std::vector<std::string> got = Split("", ";");
  const std::vector<std::string> want = {""};
  EXPECT_EQ(got, want);
}

TEST(Split, LeadingTrailingAndAdjacentSeparators) {
  const std::vector<std::string> got = Split(";a;;b;", ";");
  const std::vector<std::string> want = {"", "a", "", "b", ""};
  EXPECT_EQ(got, want);
}

TEST(Split, MultiCharSeparator) {
  const std::vector<std::string> got = Split("a::b::c", "::");
  const std::vector<std::string> want = {"a", "b", "c"};
  EXPECT_EQ(got, want);
}

TEST(TruncateUtf8, ShorterThanLimitIsUnchanged) {
  EXPECT_EQ(TruncateUtf8("short name", 100), "short name");
}

TEST(TruncateUtf8, ExactlyAtLimitIsUnchanged) {
  const std::string s(100, 'x');
  EXPECT_EQ(TruncateUtf8(s, 100), s);
}

TEST(TruncateUtf8, AsciiOverLimitGetsEllipsis) {
  // Note the off-by-one: the cutoff character itself (the 100th) is dropped
  // along with the rest, so an over-limit string keeps only maxChars-1
  // original characters before the "...". That's existing behaviour,
  // preserved here rather than "fixed" as part of a pure code move.
  const std::string s(150, 'x');
  const std::string got = TruncateUtf8(s, 100);
  EXPECT_EQ(got, std::string(99, 'x') + "...");
}

TEST(TruncateUtf8, FourByteCharacterCountsAsOneCharacterNotFour) {
  // U+1F600 (grinning face), a 4-byte UTF-8 sequence: F0 9F 98 80.
  const std::string emoji = "\xF0\x9F\x98\x80";
  const std::string s = emoji + std::string(150, 'a');
  const std::string got = TruncateUtf8(s, 100);
  // 99 kept characters (see the off-by-one note above) = the emoji plus 98
  // 'a's; the emoji's bytes are intact in the output, proving the
  // continuation bytes were skipped rather than each counted as a separate
  // character (if they had been, the emoji alone would have exhausted the
  // budget almost 4x faster).
  EXPECT_EQ(got, emoji + std::string(98, 'a') + "...");
}

TEST(TruncateUtf8, FourByteSequenceAtTheCutoffIsNotSplitMidBytes) {
  // The cutoff (character 100) lands exactly on the emoji's lead byte; a
  // byte-oriented substr(0, 100) would keep that single lead byte and
  // produce invalid UTF-8. TruncateUtf8 must drop it whole instead.
  const std::string emoji = "\xF0\x9F\x98\x80";
  const std::string s = std::string(99, 'a') + emoji + std::string(50, 'b');
  const std::string got = TruncateUtf8(s, 100);
  EXPECT_EQ(got, std::string(99, 'a') + "...");
}
