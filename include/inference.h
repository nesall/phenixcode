#ifndef _INFERENCE_H_
#define _INFERENCE_H_

#include <vector>
#include <string>
#include <memory>
#include <functional>
#include <nlohmann/json.hpp>


class App;
struct SearchResult;
struct ApiConfig;


class InferenceClient {
public:
  InferenceClient(const ApiConfig &cfg, size_t timeout);
  virtual ~InferenceClient();

protected:
  struct Impl;
  std::unique_ptr<Impl> imp;

  const ApiConfig &cfg() const;
  size_t timeoutMs() const;
};

class EmbeddingClient : public InferenceClient {
public:
  enum class EncodeType { Document, Query };
  EmbeddingClient(const ApiConfig &cfg, size_t timeout);
  void generateEmbeddings(const std::vector<std::string> &texts, std::vector<std::vector<float>> &embeddingsList, EmbeddingClient::EncodeType et) const;
  void generateEmbeddings(const std::string &text, std::vector<float> &embeddings, EmbeddingClient::EncodeType et) const;

  static float calculateL2Norm(const std::vector<float> &vec);
private:
  std::vector<std::string> prepareContent(const std::vector<std::string> &texts, EmbeddingClient::EncodeType et) const;
};

class CompletionClient : public InferenceClient {
  const App &app_;
public:
  CompletionClient(const ApiConfig &cfg, size_t timeout, const App &a);
  std::string generateCompletion(
    const nlohmann::json &messages, 
    const std::vector<SearchResult> &searchRes, 
    float temperature,
    size_t maxTokens,
    std::function<void(const std::string &)> onStream) const;

  std::string generateFim(
    const std::string &prefix, 
    const std::string &suffix, 
    const std::vector<std::string> &stops,
    float temperature, 
    size_t maxTokens,
    const std::vector<SearchResult> &searchRes
  ) const;

private:
  std::string buildContext(const std::vector<SearchResult> &searchRes, bool commentOut = false, const std::string &fileDivider = {}) const;
};

#endif // _INFERENCE_H_