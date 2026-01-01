#ifndef _APP_H_
#define _APP_H_

#include <memory>
#include <string>
#include <nlohmann/json.hpp>

class Chunker;
class Settings;
class AdminAuth;
class VectorDatabase;
class SourceProcessor;
class EmbeddingClient;
class CompletionClient;
class SimpleTokenizer;
class InstanceRegistry;

class App {
  struct Impl;
  std::unique_ptr<Impl> imp;
public:
  explicit App();
  ~App();

  // CLI commands
  void embed(bool noPrompt);
  void watch(int interval_seconds = 60);
  size_t update();
  void compact();
  void search(const std::string &query, size_t topK = 5);
  void stats();
  void clear(bool noPrompt);
  void chat();
  void serve(int port, bool watch = false, int interval = 60, const std::string &infoFile = {});
  void providers(const std::string &testProvider);

  const Settings &settings() const;
  Settings &refSettings();
  const SimpleTokenizer &tokenizer() const;
  const SourceProcessor &sourceProcessor() const;
  const Chunker &chunker() const;
  const VectorDatabase &db() const;
  VectorDatabase &db();
  const AdminAuth &auth() const;
  AdminAuth &auth();
  const InstanceRegistry &registry() const;

  bool isValidPrivateAppKey(const std::string &appKey);
  void requestShutdownAsync();

public:
  static void printUsage();
  static int run(int argc, char *argv[]);

public:
  float dbSizeMB() const;
  float indSizeMB() const;
  size_t uptimeSeconds() const;
  size_t startTimestamp() const;
  size_t lastUpdateTimestamp() const;
  nlohmann::json sourceStats() const;

  //std::string describeProjectTitle() const;

private:
  void initialize(/*const std::string &configPath*/);
  bool testSettings() const;
  static std::string runSetupWizard(AdminAuth &auth);
  static std::string findConfigFile(const std::string &filename);
  static int handleInteractivePasswordReset();
  static int handlePasswordStatus();
};

#endif // _APP_H_