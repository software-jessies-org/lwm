#ifndef STRINGS_H_included
#define STRINGS_H_included

#include <string>
#include <vector>

// Splits in on every occurrence of split, keeping empty pieces (so "a;;b"
// split on ";" gives {"a", "", "b"}, and splitting "" or a string with no
// separator gives a single-element result).
std::vector<std::string> Split(const std::string& in,
                               const std::string& split);

// Truncates a UTF-8 string to at most maxChars characters (not bytes),
// appending "..." if truncation happened. maxChars counts Unicode
// characters, not bytes, so multi-byte sequences aren't split.
std::string TruncateUtf8(const std::string& s, int maxChars);

#endif
