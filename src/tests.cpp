#include "cutils.h"

#include <iostream>
#include <string>
#include <vector>

namespace {

  struct TestCase {
    const char *name;
    std::string input;
    std::string expected;
  };

  bool test_stripMarkdownFromCodeBlock(const TestCase &t) {
    auto out = utils::stripMarkdownFromCodeBlock(t.input);
    bool ok = (out == t.expected);
    if (ok) {
      std::cout << "[PASS] " << t.name << "\n";
    } else {
      utils::stripMarkdownFromCodeBlock(t.input);

      std::cout << "[FAIL] " << t.name << "\n";
      std::cout << "  input   : " << t.input << "\n";
      std::cout << "  expected: " << t.expected << "\n";
      std::cout << "  got     : " << out << "\n";
    }
    return ok;
  }

} // anonymous namespace


void runUnitTests() {
  std::vector<TestCase> tests = {
    { "short_string_less_than_6", "abc", "abc" },
    { "not_starting_with_fence", "`` code ```", "`` code ```" },
    { "single_line_fenced", "```code```", "code" },
    { "fenced_with_language_and_trailing_newline",
      "```cpp\nint x = 1;\n```",
      "int x = 1;" },
    { "fenced_without_language",
      "```\nline\n```",
      "line" },
    { "closing_fence_with_trailing_whitespace",
      "```py\nprint(1)\n```   \n",
      "print(1)" },
    { "closing_fence_with_extra_content_after => unchanged",
      "```js\nvar a = 2;\n```\nEXTRA",
      "```js\nvar a = 2;\n```\nEXTRA" },
    { "no_closing_fence => unchanged",
      "```cpp\nint a = 0;\n",
      "```cpp\nint a = 0;\n" },
    { "empty_inner_block_behaviour (implementation returns original)",
      "```\n```",
      "```\n```" },
    { "language_with_surrounding_whitespace",
      "```  cpp  \nvoid f();\n\n```",
      "void f();\n" }
  };

  int passed = 0;
  for (const auto &t : tests) {
    if (test_stripMarkdownFromCodeBlock(t)) ++passed;
  }

  std::cout << "\nSummary: " << passed << " / " << tests.size() << " passed.\n";
}