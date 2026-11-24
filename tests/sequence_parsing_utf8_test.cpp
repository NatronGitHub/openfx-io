// SequenceParsing on UTF-8 file names (#357), the cases that SequenceParsing's own test
// (IOSupport/SequenceParsing/tests/SequenceParsingTest.cpp, testUtf8()) doesn't cover: 3-byte characters ("日本"),
// a frame number right after a non-ASCII byte, %v of an empty view name (not a NUL byte), and %V. Returns 0 if all
// checks pass.

#include "SequenceParsing.h"

#include <cstdio>
#include <iostream>
#include <string>
#include <vector>

using namespace SequenceParsing;

namespace {

int g_failures = 0;

// the string with its non-printable and non-ASCII bytes as \xNN, for the failure messages
std::string
escaped(const std::string& s)
{
    std::string ret;
    for (const char c : s) {
        const unsigned char u = static_cast<unsigned char>(c);
        if ( (u < 0x20) || (u >= 0x7F) ) {
            char buf[5];
            std::snprintf(buf, sizeof(buf), "\\x%02X", u);
            ret += buf;
        } else {
            ret += c;
        }
    }

    return ret;
}

void
checkEq(const char* what,
        const std::string& expected,
        const std::string& actual)
{
    if (expected != actual) {
        ++g_failures;
        std::cerr << "FAIL " << what << ": expected \"" << escaped(expected) << "\", got \"" << escaped(actual)
                  << "\"" << std::endl;
    }
}

void
checkEq(const char* what,
        int expected,
        int actual)
{
    if (expected != actual) {
        ++g_failures;
        std::cerr << "FAIL " << what << ": expected " << expected << ", got " << actual << std::endl;
    }
}

// the frames of the files of `files` that match `pattern` (without path)
SequenceFromPattern
parse(const std::string& pattern,
      const StringList& files)
{
    SequenceFromPattern seq;
    filesListFromPattern_fast(pattern, files, &seq);

    return seq;
}

const std::string kNihon = "\xE6\x97\xA5\xE6\x9C\xAC"; // "日本" (U+65E5 U+672C)

void
testPatterns()
{
    StringList files;
    files.push_back(kNihon + "0001.exr"); // a frame number right after a non-ASCII byte
    files.push_back(kNihon + "0002.exr");
    files.push_back(kNihon + "_" + kNihon + ".exr"); // a non-ASCII byte where the pattern has a frame number

    checkEq("frames of 日本####.exr", 2, (int)parse(kNihon + "####.exr", files).size() );
    checkEq("frames of 日本%04d.exr", 2, (int)parse(kNihon + "%04d.exr", files).size() );
    checkEq("frames of 日本_####.exr", 0, (int)parse(kNihon + "_####.exr", files).size() );

    FileNameContent c("/d/" + kNihon + "_0012.exr");
    checkEq("numbers in 日本_0012.exr", 1, c.getPotentialFrameNumbersCount() );
    std::string number;
    checkEq("number of 日本_0012.exr found", 1, c.getNumberByIndex(0, &number) ? 1 : 0);
    checkEq("number of 日本_0012.exr", "0012", number);
}

void
testViews()
{
    std::vector<std::string> views;
    views.push_back(kNihon); // %v is "日", the whole first character
    views.push_back(""); // %v is nothing (not a NUL byte)
    checkEq("%v of 日本", "img_\xE6\x97\xA5.exr", generateFileNameFromPattern("img_%v.exr", views, 1, 0) );
    checkEq("%v of an empty view name", "img_.exr", generateFileNameFromPattern("img_%v.exr", views, 1, 1) );
    checkEq("%V of 日本", "img_" + kNihon + ".exr", generateFileNameFromPattern("img_%V.exr", views, 1, 0) );
}

} // namespace

int
main()
{
    testPatterns();
    testViews();
    if (g_failures != 0) {
        std::cerr << g_failures << " check(s) failed" << std::endl;

        return 1;
    }
    std::cout << "sequence_parsing_utf8_test: all checks passed" << std::endl;

    return 0;
}
