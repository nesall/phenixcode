#include "chunker.h"
#include <3rdparty/utf8.h>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <algorithm>

namespace {
int checks = 0;
void check(bool value, const char *message) { ++checks; if (!value) throw std::runtime_error(message); }
void verify(const SimpleTokenizer &tokenizer, const std::string &text, const std::string &uri,
  size_t maximum, float overlap, bool semantic) {
  Chunker chunker(tokenizer, 1, maximum, overlap);
  const auto chunks = chunker.chunkText(text, uri, semantic);
  std::vector<bool> covered(text.size(), false);
  std::vector<size_t> lines{0};
  for (size_t i=0;i<text.size();++i) if (text[i]=='\n') lines.push_back(i+1);
  check(text.empty() || !chunks.empty(), "nonempty input produces chunks");
  check(chunks.size() <= text.size(), "chunking makes bounded forward progress");
  for (const auto &chunk : chunks) {
    size_t start=chunk.metadata.start, end=chunk.metadata.end;
    if (chunk.metadata.unit=="line") {
      check(start < lines.size() && end <= lines.size(), "line ranges use original lines");
      start=lines.at(start); end=end==lines.size()?text.size():lines.at(end);
    }
    check(start < end && end <= text.size(), "source ranges are valid");
    check(chunk.text==text.substr(start,end-start), "chunk text is an exact original source slice");
    check(utf8::is_valid(chunk.text.begin(),chunk.text.end()), "chunk boundaries preserve UTF-8");
    check(chunk.metadata.tokenCount==tokenizer.countTokensWithVocab(chunk.text), "stored counts match emitted text");
    check(chunk.metadata.tokenCount<=maximum, "every chunk fits the budget");
    std::fill(covered.begin()+start,covered.begin()+end,true);
  }
  check(std::all_of(covered.begin(),covered.end(),[](bool present){return present;}), "all source bytes are covered");
}
}
int main(int argc,char**argv) {
  try {
    if(argc!=2) throw std::runtime_error("Provide the tokenizer fixture path");
    SimpleTokenizer vocab(argv[1]);
    SimpleTokenizer fallback("missing-chunker-test-tokenizer.json");
    check(vocab.countTokensWithVocab(std::string(101,'a'))==1,"unknown words count as one token");
    check(vocab.countTokensWithVocab("HELLO")==vocab.countTokensWithVocab("hello"),"configured ASCII lowercasing is honored");
    check(vocab.countTokensWithVocab("hello",true)==vocab.countTokensWithVocab("hello")+2,"special tokens counted once per input");
    check(vocab.countTokensWithVocab(std::string("\xe2\x82",2))==0,"BERT cleaning safely removes invalid UTF-8 replacements");
    const std::string code="int a = 1; int b = 2; int c = 3;\r\nreturn a;";
    const std::string markdown="    Heading\n\n    content with     spaces\n```cpp\nint x;\n```\n";
    std::string unicode;
    for(int i=0;i<80;++i) unicode += "\xce\xb1\xce\xb2\xce\xb3 ";
    for(const auto *tok : {&vocab,&fallback}) {
      for(float overlap : {0.f,0.25f,0.9f}) {
        verify(*tok,code,"example.cpp",4,overlap,true);
        verify(*tok,markdown,"example.md",8,overlap,true);
        verify(*tok,code,"example.cpp",8,overlap,false);
        verify(*tok,unicode,"example.txt",8,overlap,false);
        verify(*tok,std::string(1000,'a'),"long.cpp",20,overlap,true);
        verify(*tok," \n\t\r\n", "whitespace.cpp",4,overlap,true);
      }
      verify(*tok,std::string(100000,' '),"padding.cpp",128,0.25f,true);
      std::string oversizedLine;
      for(int i=0;i<4096;++i) oversizedLine += "value += 1; ";
      verify(*tok,oversizedLine,"unknown-extension",128,0.25f,true);
    }
    const std::string joinedWords="hello\vhello";
    check(vocab.countTokensWithVocab(joinedWords)>2*vocab.countTokensWithVocab("hello"),
      "cleaning can make joined unit counts exceed their sum");
    verify(vocab,joinedWords,"controls.txt",2,0.f,false);
    verify(vocab,joinedWords+joinedWords+joinedWords,"controls.txt",3,0.5f,false);
    Chunker overlapChunks(vocab,1,3,0.5f);
    const std::string spaced="hello  world  hello  world  hello  world";
    const auto trimmedOverlap=overlapChunks.chunkText(spaced,"spaces.txt",false);
    check(trimmedOverlap.size()>1,"overlap fixture spans multiple chunks");
    for(size_t i=1;i<trimmedOverlap.size();++i)
      check(trimmedOverlap[i].text.front()!=' ',"overlap starts after already covered zero-token whitespace");
    verify(vocab,spaced,"spaces.txt",3,0.5f,false);

    std::string payload;
    for (int i = 0; i < 8192; ++i) payload += "QUJD";
    const std::string prefix = "// image\nconst char *icon = \"data:image/png;base64,";
    const std::string suffix = "\";\nint meaningful_identifier = 42;\n";
    const auto original = prefix + payload + suffix;
    const auto masked = Chunker::maskOpaqueBlobs(original);
    check(masked == prefix + "[32768 encoded characters omitted]" + suffix, "explicit image payload is masked and surrounding code preserved");
    check(Chunker::maskOpaqueBlobs(payload) == payload, "standalone base64-compatible strings are preserved");
    check(Chunker::maskOpaqueBlobs("data:image/png;base64,AAAA") == "data:image/png;base64,AAAA", "small payloads are preserved");
    check(Chunker::maskOpaqueBlobs("metadata:image/png;base64," + payload) == "metadata:image/png;base64," + payload, "data scheme must start at a boundary");
    check(Chunker::maskOpaqueBlobs("data:image/png;base64," + payload + "===") == "data:image/png;base64," + payload + "===", "malformed padding is preserved");
    check(Chunker::maskOpaqueBlobs("data:image/png;base64," + payload + "$invalid") == "data:image/png;base64," + payload + "$invalid", "invalid payload suffix is preserved");
    std::string identifier;
    for (int i=0;i<100;++i) identifier += "aB12";
    check(Chunker::maskOpaqueBlobs(identifier)==identifier,"long mixed-case identifiers are preserved");
    check(Chunker::maskOpaqueBlobs(original + original)==masked + masked,"multiple data URIs are independently masked");
    check(Chunker::maskOpaqueBlobs("DATA:image/png;BASE64,"+payload).find("omitted]")!=std::string::npos,"data URI scheme and encoding are case insensitive");
    std::string manyUris,expectedUris,longHeaders;
    for(int i=0;i<512;++i) {
      manyUris += "data:;base64,"+std::string(128,'A')+"\n";
      expectedUris += "data:;base64,[128 encoded characters omitted]\n";
      longHeaders += "data:"+std::string(300,'x')+":";
    }
    check(Chunker::maskOpaqueBlobs(manyUris)==expectedUris,"many unpadded payloads are masked independently");
    longHeaders += ";base64,"+payload;
    check(Chunker::maskOpaqueBlobs(longHeaders)==longHeaders,"overlong data URI headers are preserved");
    Chunker unknown(vocab,1,24,0.f);
    const auto unknownChunks=unknown.chunkText("const char *icon = \"data:image/png;base64,"+payload+"\";", "unknown-extension");
    check(!unknownChunks.empty() && unknownChunks.size()<10,"unknown extensions classify masked data without regexing the payload");
    for (const auto *tok : {&vocab,&fallback}) {
      for (const bool semantic : {false,true}) {
        for (const float overlap : {0.f,0.25f}) {
          Chunker blobs(*tok,1,24,overlap);
          const auto chunks=blobs.chunkText(original,"image.cpp",semantic);
          check(chunks.size()<10,"large images produce bounded chunk counts");
          std::string combined;
          std::vector<bool> covered(original.size(),false);
          std::vector<size_t> lines{0};
          for(size_t i=0;i<original.size();++i) if(original[i]=='\n') lines.push_back(i+1);
          bool markerSeen=false;
          for(const auto &chunk:chunks) {
            auto begin=chunk.metadata.start,end=chunk.metadata.end;
            if(chunk.metadata.unit=="line") { begin=lines.at(begin);end=end==lines.size()?original.size():lines.at(end); }
            check(begin<end && end<=original.size(),"masked chunks retain valid original ranges");
            std::fill(covered.begin()+begin,covered.begin()+end,true);
            check(chunk.metadata.tokenCount==tok->countTokensWithVocab(chunk.text) && chunk.metadata.tokenCount<=24,"marker tokens participate in budgeting");
            if(chunk.text.find("omitted]")!=std::string::npos) {
              markerSeen=true;
              check(chunk.text.find("[32768 encoded characters omitted]")!=std::string::npos,"marker stays indivisible");
              check(begin<=prefix.size() && end>=prefix.size()+payload.size(),"marker range covers original payload");
            }
            combined+=chunk.text;
          }
          check(markerSeen,"indexed text retains a payload description");
          check(std::all_of(covered.begin(),covered.end(),[](bool value){return value;}),"masked chunk ranges cover all original bytes");
          if(overlap==0.f) check(combined==masked,"masking preserves all surrounding text through chunking");
        }
      }
    }
    bool markerRejected=false;
    try { Chunker tiny(fallback,1,1,0.f); tiny.chunkText(original,"image.cpp"); }
    catch(const std::runtime_error &) { markerRejected=true; }
    check(markerRejected,"too-small marker budgets fail explicitly");

    Chunker normal(vocab,1,100,0.f);
    const auto lines=normal.chunkText("int x;\nreturn x;", "example.cpp");
    check(lines[0].metadata.unit=="line" && lines[0].metadata.end==2,"whole lines retain original line coordinates");
    check(normal.chunkText(code,"example.cpp",false)[0].metadata.type=="text","explicit text strategy is honored");
    check(normal.chunkText("", "empty.cpp").empty(),"empty input stays empty");
    for(int scenario=0;scenario<5;++scenario) {
      bool rejected=false;
      try {
        Chunker invalid(vocab,scenario==1?10:1,scenario==0?0:5,
          scenario==2?-0.1f:scenario==3?1.f:scenario==4?std::numeric_limits<float>::quiet_NaN():0.f);
      } catch(const std::invalid_argument &) { rejected=true; }
      check(rejected,"invalid chunking configuration is rejected");
    }
    std::cout<<checks<<" chunking checks passed.\n";
  } catch(const std::exception &error) { std::cerr<<error.what()<<'\n'; return 1; }
}
