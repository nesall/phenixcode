#include "tokenizer.h"
#include "json_shim.h"
#include <filesystem>
#include <fstream>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <atomic>

using nlohmann::json;
namespace fs = std::filesystem;
namespace {
int checks=0;
void check(bool value,const char *description) { ++checks; if(!value) throw std::runtime_error(description); }
struct Fixture {
  fs::path directory=fs::temp_directory_path()/("phenixcode-tokenizer-test-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  fs::path path=directory/"tokenizer.json";
  Fixture() { if(!fs::create_directory(directory)) throw std::runtime_error("Unable to create tokenizer fixture"); }
  ~Fixture() {
    if(directory.parent_path()==fs::temp_directory_path() && directory.filename().string().starts_with("phenixcode-tokenizer-test-")) {
      std::error_code error; fs::remove_all(directory,error);
    }
  }
  void write(const json &normalizer, size_t maximum=100) {
    json vocab={{"ca",0},{"##fe",1},{"a",2},{"##a",3},{"b",4},{"##b",5},{"i",6},{"##i",7},
      {"\xce\xb1",8},{"##\xce\xb1",9}};
    std::ofstream file(path,std::ios::binary);
    file<<json{{"model",{{"type","WordPiece"},{"vocab",vocab},{"max_input_chars_per_word",maximum}}},{"normalizer",normalizer}};
  }
};
}
int main() {
  try {
    Fixture fixture;
    const std::string cafe="caf\xc3\xa9";
    const std::string decomposed="cafe\xcc\x81";
    const json bert={{"type","BertNormalizer"},{"lowercase",true},{"strip_accents",nullptr}};
    fixture.write(bert);
    SimpleTokenizer tokenizer(fixture.path.string());
    check(tokenizer.countTokensWithVocab(cafe)==2,"default lowercase BERT strips Latin accents");
    check(tokenizer.countTokensWithVocab("CAF\xc3\x89")==2,"uppercase Latin accents normalize");
    check(tokenizer.countTokensWithVocab(decomposed)==2,"decomposed accents are removed");
    check(tokenizer.countTokensWithVocab("\xc4\x8d" "af\xc3\xa9")==2,"Latin Extended canonical accents fold");
    check(tokenizer.countTokensWithVocab("\xce\x91\xce\x91")==2,"simple Unicode lowercasing is applied");
    check(tokenizer.countTokensWithVocab("a\xe2\x80\x9c" "b\xe2\x80\x9d\xe2\x80\x94" "a")==6,"Unicode punctuation splits into separate tokens");
    check(tokenizer.countTokensWithVocab("a\xc2\xa0" "b\xe2\x80\x83" "a")==3,"non-ASCII whitespace splits words");
    check(tokenizer.countTokensWithVocab(std::string("aa\0aa",5))==4,"BERT cleaning removes controls before WordPiece");
    check(tokenizer.countTokensWithVocab("aa\xe2\x80\x8b" "aa")==4,"Unicode format controls are cleaned");
    check(tokenizer.countTokensWithVocab("\xf0\xa0\x80\x80\xf0\xa0\x80\x81")==2,"supplementary CJK characters are separated");
    check(tokenizer.countTokensWithVocab("\xf0\xa0",false)==0,"truncated UTF-8 is cleaned safely");
    check(tokenizer.countTokensWithVocab("",true)==2,"empty input retains requested special tokens");
    check(tokenizer.countTokensWithVocab(cafe,true)==4,"special tokens are added once per sequence");

    auto keep=bert;keep["strip_accents"]=false;fixture.write(keep);
    SimpleTokenizer preserve(fixture.path.string());
    check(preserve.countTokensWithVocab(cafe)==1,"explicit false preserves accents");
    keep["lowercase"]=nullptr;fixture.write(keep);
    SimpleTokenizer nullable(fixture.path.string());
    check(nullable.countTokensWithVocab("AA")==1,"null lowercase is safely treated as false");
    keep["lowercase"]="true";fixture.write(keep);
    SimpleTokenizer wrongType(fixture.path.string());
    check(wrongType.countTokensWithVocab("AA")==1,"nonboolean lowercase is safely handled");
    fixture.write({{"type","Sequence"},{"normalizers",json::array({{{"type","Lowercase"}},{{"type","StripAccents"}}})}});
    SimpleTokenizer sequence(fixture.path.string());
    check(sequence.countTokensWithVocab("CAF\xc3\x89")==2,"normalizer sequences are traversed");
    fixture.write({{"type","Sequence"},{"normalizers",json::array({{{"type","BertNormalizer"},{"lowercase",false}},{{"type","Lowercase"}}})}});
    SimpleTokenizer ordered(fixture.path.string());
    check(ordered.countTokensWithVocab("CAF\xc3\x89")==1,"sequence steps retain independent settings and order");
    fixture.write({{"type","Sequence"},{"normalizers",json::array({{{"type","Sequence"},{"normalizers",json::array({bert})}}})}});
    SimpleTokenizer nested(fixture.path.string());
    check(nested.countTokensWithVocab(cafe)==2,"nested sequences are supported");
    auto noChinese=bert;noChinese["handle_chinese_chars"]=false;fixture.write(noChinese);
    SimpleTokenizer joinedChinese(fixture.path.string());
    check(joinedChinese.countTokensWithVocab("\xf0\xa0\x80\x80\xf0\xa0\x80\x81")==1,"explicit false disables CJK separation");
    auto noClean=bert;noClean["clean_text"]=false;fixture.write(noClean);
    SimpleTokenizer controls(fixture.path.string());
    check(controls.countTokensWithVocab(std::string("aa\0aa",5))==1,"explicit false disables control cleaning");
    fixture.write(bert,1);SimpleTokenizer shortWords(fixture.path.string());
    check(shortWords.countTokensWithVocab("aa")==1,"configured maximum word length is honored");
    fixture.write(nullptr);SimpleTokenizer nullNormalizer(fixture.path.string());
    check(nullNormalizer.countTokensWithVocab("aa")==2,"null normalizer is safe");
    {std::ofstream file(fixture.path);file<<"{invalid JSON";}
    SimpleTokenizer malformed(fixture.path.string());
    SimpleTokenizer missing((fixture.directory/"missing.json").string());
    check(malformed.countTokensWithVocab("abcdef hello")==missing.countTokensWithVocab("abcdef hello"),"malformed JSON falls back to estimation");
    {std::ofstream file(fixture.path);file<<"null";}
    SimpleTokenizer invalidRoot(fixture.path.string());
    check(invalidRoot.countTokensWithVocab("abcdef")==2,"invalid root falls back safely");
    std::atomic<bool> consistent=true;
    std::vector<std::thread> threads;
    for(int i=0;i<8;++i) threads.emplace_back([&] {for(int j=0;j<1000;++j) if(tokenizer.countTokensWithVocab(cafe)!=2) consistent=false;});
    for(auto &thread:threads) thread.join();
    check(consistent,"shared tokenizer cache is thread safe");
    std::cout<<checks<<" tokenizer checks passed.\n";
  } catch(const std::exception &error) {std::cerr<<error.what()<<'\n';return 1;}
}
