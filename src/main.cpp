#include "app.h"
#include <utils_log/logger.hpp>

#ifdef _DEBUG
extern void runUnitTests();
#endif

//#define TEST_CHUNKING

#ifdef TEST_CHUNKING
#include <iostream>
#include "chunker.h"
#include "sourceproc.h"
#endif

int main(int argc, char *argv[]) {

#ifdef TEST_CHUNKING

  if (argc > 1 && std::string(argv[1]) == "test_chunking") {
    LOG_START;
    LOG_MSG << "test_chunking";
    App app("settings.json");

    const auto &chunker = app.chunker();
    std::string testCode = R"(#include <iostream>
    class MyClass {
    public:
        MyClass() {}
        void myFunction(int x) {
            if (x > 0) {
                std::cout << "Positive" << std::endl;
            } else {
                std::cout << "Non-positive" << std::endl;
            }
        }
    };
)";
  
    std::string testText = R"(# Project Title
)";

    const auto &srcProc = app.sourceProcessor();
    auto src = const_cast<SourceProcessor &>(srcProc).collectSources();
    if (4 < src.size()) {
      testCode = src[2].content;
    }

    auto codeChunks = chunker.chunkText(testCode);
    auto textChunks = chunker.chunkText(testText);
  
    LOG_MSG << "Code Chunks:";
    for (const auto &chunk : codeChunks) {
      LOG_MSG << "\n\n----- chunk=" << chunk.chunkId << ", tokens = "<< chunk.metadata.tokenCount << "\n" << chunk.text << "\n";
    }
  
    LOG_MSG << "\n\nText Chunks:";
    for (const auto &chunk : textChunks) {
      LOG_MSG << "\n\n----- size="<< chunk.metadata.tokenCount << "\n" << chunk.text << "\n";
    }
  
    return 0;
  }

#endif

#ifdef _DEBUG
  runUnitTests();
#endif

  return App::run(argc, argv);
}