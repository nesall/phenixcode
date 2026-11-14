#include "app.h"
#include "evaluator.h"
#include "settings.h"
#include "auth.h"
#include "3rdparty/fmt/format.h"
#include "3rdparty/CLI11.hpp"
#include <utils_log/logger.hpp>

#include <iostream>
#include <string>
#include <vector>
#include <csignal>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <unistd.h>
#include <termios.h>
#include <sys/stat.h>
#endif


namespace {

  std::string promptPassword(const std::string &prompt) {
    std::cout << prompt;
    std::string password;

#ifdef _WIN32
    // Windows: Hide input
    HANDLE hStdin = GetStdHandle(STD_INPUT_HANDLE);
    DWORD mode;
    GetConsoleMode(hStdin, &mode);
    SetConsoleMode(hStdin, mode & (~ENABLE_ECHO_INPUT));
    std::getline(std::cin, password);
    SetConsoleMode(hStdin, mode);
#else
    // Linux/Mac: Use termios
    struct termios oldt, newt;
    tcgetattr(STDIN_FILENO, &oldt);
    newt = oldt;
    newt.c_lflag &= ~ECHO;
    tcsetattr(STDIN_FILENO, TCSANOW, &newt);
    std::getline(std::cin, password);
    tcsetattr(STDIN_FILENO, TCSANOW, &oldt);
#endif

    std::cout << "\n";
    return password;
  }
} // anonymous namespace

void SignalHandler::handleSignal(int)
{
  shutdownRequested = 1;
}

bool SignalHandler::shouldShutdown()
{
  return shutdownRequested != 0;
}

void SignalHandler::requestShutdown()
{
  shutdownRequested = 1;
}

void SignalHandler::setup()
{
  // Only use standard C++ signal function
  std::signal(SIGINT, handleSignal);
  std::signal(SIGTERM, handleSignal);

  // Note: SIGKILL cannot be caught on any platform
  // Note: Some signals are platform-specific
}

volatile std::sig_atomic_t SignalHandler::shutdownRequested = 0;


int App::handleInteractivePasswordReset()
{
  std::cout << "===================================\n";
  LOG_MSG << "   Reset Admin Password             ";
  std::cout << "===================================\n\n";

  int nofTries = 3;
  std::string newPassword;
  do {
    newPassword = promptPassword("Enter new password (min 8 chars): ");
    std::string confirm = promptPassword("Confirm password: ");
    if (newPassword != confirm) {
      LOG_MSG << "Error: Passwords do not match\n";
      continue;
    }
    if (newPassword.length() < 8) {
      LOG_MSG << "Error: Password must be at least 8 characters\n";
      continue;
    }
    if (newPassword == "admin") {
      LOG_MSG << "Error: Cannot use 'admin' as password\n";
      continue;
    }
    break;
  } while (--nofTries);

  if (0 == nofTries) {
    LOG_MSG << "Unable to reset admin password. Exiting.";
    return 1;
  }

  AdminAuth auth;
  auth.setPassword(newPassword);
  LOG_MSG << "\nPassword updated successfully!";
  return 0;
}

int App::handlePasswordStatus()
{
  AdminAuth auth;
  std::cout << "Admin Password Status:\n";
  std::cout << "-------------------------\n";
  if (auth.isDefaultPassword()) {
    LOG_MSG << "Status: Using default password 'admin'\n";
    LOG_MSG << "  WARNING: Please change the default password!\n";
    LOG_MSG << fmt::format("Run: {} reset-password --pass <your_password>\n", App::binaryName());
  } else {
    LOG_MSG << "Status: Custom password set \n";
    LOG_MSG << "Last modified: " << auth.fileLastModifiedTime() << "\n";
  }
  return 0;
}

std::string App::findConfigFile(const std::string &filename)
{
  std::vector<std::string> searchPaths = {
      filename,                          // Current dir
      "../" + filename,                  // Parent
      "../../" + filename,               // Grandparent
      std::string(std::getenv("HOME") ? std::getenv("HOME") : ".") + "/.config/phenixcode/" + filename,  // User config
      "/etc/phenixcode/" + filename        // System-wide (Linux)
  };

  for (const auto &path : searchPaths) {
    if (std::filesystem::exists(path)) {
      //LOG_MSG << "Found config at:" << path;
      return path;
    }
  }

  // Not found, run wizard
  LOG_MSG << "Config file not found, running setup wizard...";
  //AdminAuth auth;
  //return runSetupWizard(auth);
  return {};
}

int App::run(int argc, char *argv[])
{
  App::setBinaryName(argv[0]);

  SignalHandler::setup();

  CLI::App app{ "PhenixCode RAG System" };
  app.set_version_flag("--version,-v",
    fmt::format("PhenixCode RAG System\nAuthor: Arman Sahakyan\nVersion: {}\nBuild date: {} {}", EMBEDDER_VERSION, __DATE__, __TIME__));
  app.require_subcommand(0, 1); // Allow 0 or 1 subcommand (0 shows help)

  std::string configPath = "settings.json";
  app.add_option("-c,--config", configPath, "Config file path")->envname("PHENIXCODE_PROJECT_CONFIG")->check(CLI::ExistingFile);

  std::string providersPath;
  app.add_option("-w,--providers-config", providersPath, "Providers file path")->envname("PHENIXCODE_PROVIDERS_CONFIG")->check(CLI::ExistingFile);

  bool noStartupTests = false;
  app.add_flag("--no-startup-tests", noStartupTests, "Skip startup model test calls");

  // Password management commands
  auto cmdValidatePass = app.add_subcommand("validate-password", "Validate admin password");
  std::string validPassword;
  cmdValidatePass->add_option("--pass", validPassword, "Valid password")->required();

  auto cmdResetPass = app.add_subcommand("reset-password", "Reset admin password");
  std::string newPassword;
  cmdResetPass->add_option("--pass", newPassword, "New password")
    ->required()
    ->check(CLI::IsMember({ "admin" }, CLI::ignore_case).description("Cannot be 'admin'"));
  auto cmdResetInteractive = app.add_subcommand("reset-password-interactive", "Reset password interactively");
  auto cmdPasswordStatus = app.add_subcommand("password-status", "Check password status");

  auto passwordGroup = app.add_option_group("password", "Password Management");
  passwordGroup->add_subcommand(cmdResetPass);
  passwordGroup->add_subcommand(cmdResetInteractive);
  passwordGroup->add_subcommand(cmdPasswordStatus);
  passwordGroup->require_option(0, 1); // Max one password command

  // Main commands
  auto cmdEmbed = app.add_subcommand("embed", "Process and embed all configured sources");
  bool embedForce = false;
  cmdEmbed->add_flag("--force", embedForce, "Force re-embed all files");

  auto cmdUpdate = app.add_subcommand("update", "Incrementally update changed files only");

  auto cmdWatch = app.add_subcommand("watch", "Continuously monitor and update");
  int watchInterval = 60;
  cmdWatch->add_option("--interval", watchInterval, "Update interval in seconds")->default_val(60);

  auto cmdSearch = app.add_subcommand("search", "Search for similar chunks");
  std::string searchQuery;
  size_t searchTopk = 5;
  cmdSearch->add_option("query", searchQuery, "Search query")->required();
  cmdSearch->add_option("--top", searchTopk, "Number of results")->default_val(5);

  auto cmdStats = app.add_subcommand("stats", "Show database statistics");

  auto cmdClear = app.add_subcommand("clear", "Clear all data");
  bool clearNoConfirm = false;
  cmdClear->add_flag("--yes,-y", clearNoConfirm, "Skip confirmation prompt");

  auto cmdCompact = app.add_subcommand("compact", "Reclaim deleted space");

  auto cmdChat = app.add_subcommand("chat", "Chat mode");

  auto cmdServe = app.add_subcommand("serve", "Start HTTP API server");
  bool serveNoConfirm = false;
  cmdServe->add_flag("--yes,-y", serveNoConfirm, "Skip confirmation prompt");

  int servePort = 8590;
  bool serveWatch = false;
  int serveWatchInterval = 60;
  std::string privateAppKey;
  std::string infoFile;
  cmdServe->add_option("-p,--port", servePort, "Server port")
    ->default_val(8590)
    ->envname("PHENIXCODE_SERVER_PORT")
    ->check(CLI::Range(0, 65535));
  cmdServe->add_flag("--watch", serveWatch, "Enable auto-update");
  cmdServe->add_option("--interval", serveWatchInterval, "Watch interval in seconds")->default_val(60);
  cmdServe->add_option("--appkey", privateAppKey, "Caller-provided key for privileged operations (e.g., exit requests through the instance registry)");
  cmdServe->add_option("--info-file", infoFile, "Path to store instance info such as port server started");

  auto cmdProviders = app.add_subcommand("providers", "List embedding and completion providers");
  std::string testProvider;
  cmdProviders->add_option("--test", testProvider, "Test call to a given provider");

  auto cmdEval = app.add_subcommand("eval", "Evaluate retrieval quality");
  std::string evalDatasetPath;
  bool evalInitFlag = false;
  std::string evalOutputPath = "eval.json";
  size_t evalSamples = 10;
  bool evalListFlag = false;
  std::string evalDetailId;
  size_t evalTopK = 5;
  bool evalAddVectorsFlag = false;
  bool evalSweepFlag = false;
  std::vector<float> evalBm25Weights;
  std::vector<float> evalGrepWeights;
  std::vector<size_t> evalTopKs;

  cmdEval->add_option("--dataset", evalDatasetPath, "Path to eval.json dataset");
  cmdEval->add_flag("--init", evalInitFlag, "Generate template eval.json");
  cmdEval->add_option("--output", evalOutputPath, "Output path for --init")->default_val("eval.json");
  cmdEval->add_option("--samples", evalSamples, "Number of sample queries for --init")->default_val(10);
  cmdEval->add_flag("--list", evalListFlag, "List available document IDs");
  cmdEval->add_option("--detail", evalDetailId, "Show details for a document ID");
  cmdEval->add_option("--top", evalTopKs, "Top-k values (repeat or space-separated)")
    ->expected(-1)->default_val(std::vector<size_t>{ 5 });
  cmdEval->add_flag("--add-vectors", evalAddVectorsFlag, "Embed queries in dataset, write result to --output");
  cmdEval->add_option("--bm25-weight", evalBm25Weights, "BM25 weight (repeat or space-separated)")
    ->expected(-1)->default_val(std::vector<float>{ kHybridBm25Weight });
  cmdEval->add_option("--grep-weight", evalGrepWeights, "Grep RRF weight (repeat or space-separated)")
    ->expected(-1)->default_val(std::vector<float>{ kGrepRrfWeight });
  cmdEval->add_flag("--sweep", evalSweepFlag, "Sweep bm25 x grep weights");

  try {
    app.parse(argc, argv);

    configPath = findConfigFile(configPath);
    if (configPath.empty()) {
      LOG_MSG << "No config file found. Exiting.";
      return 1;
    }

    const bool isReadOnlyEval = cmdEval->parsed() && !evalDatasetPath.empty() == false; // init, list, detail — no dataset run
    if (!isReadOnlyEval && providersPath.empty()) {
      providersPath = findConfigFile("providers.json");
      if (providersPath.empty()) {
        LOG_MSG << "No providers.json found. Use -w <path> to specify one.";
        return 1;
      }
    }

    std::unique_ptr<Settings> settings;
    try {
      if (isReadOnlyEval && providersPath.empty())
        settings = std::make_unique<Settings>(configPath);
      else
        settings = std::make_unique<Settings>(configPath, providersPath);
      settings->initProjectIdIfMissing(true);
      settings->initProjectTitleIfMissing(true);
    } catch (const std::exception &ex) {
      LOG_MSG_ERROR << ex.what();
      throw;
    }
    
    SET_LOG_OUTPUT_FILE_PATH(settings->loggingLoggingFile());
    SET_LOG_DIAGNOSTICS_FILE_PATH(settings->loggingDiagnosticsFile());
    SET_LOG_TO_FILE(settings->loggingLogToFile());
    SET_LOG_TO_CONSOLE(settings->loggingLogToConsole());
    LOG_MSG << "Build Date:" << __DATE__ << __TIME__;
    LOG_MSG << "Read settings from" << std::filesystem::absolute(configPath);

    // Handle password commands first (no app initialization needed)
    if (cmdValidatePass->parsed()) {
      AdminAuth auth;
      std::string jwtToken;
      if (auth.authenticate({ validPassword, "Basic" }, jwtToken)) {
        LOG_MSG << "Password is valid";
      } else {
        LOG_MSG << "Invalid password";
      }
      return 0;
    }

    if (cmdResetPass->parsed()) {
      AdminAuth auth;
      if (newPassword.length() < 8) {
        LOG_MSG << "Error: Password must be at least 8 characters";
        return 1;
      }
      if (newPassword == "admin") {
        LOG_MSG << "Error: Cannot use 'admin' as password";
        return 1;
      }
      auth.setPassword(newPassword);
      LOG_MSG << "Admin password has been reset";
      return 0;
    }

    if (cmdResetInteractive->parsed()) {
      return handleInteractivePasswordReset();
    }

    if (cmdPasswordStatus->parsed()) {
      return handlePasswordStatus();
    }

    App appInstance;
    appInstance.setSettings(std::move(settings));
    appInstance.setPrivateAppKey(privateAppKey);
    appInstance.initialize();

    if (!noStartupTests) {
      if (!isReadOnlyEval && !appInstance.testSettings()) {
        LOG_MSG << "Wrong/incomplete settings. Exiting.";
        return 1;
      }
    }

    // Handle main commands
    if (cmdEmbed->parsed()) {
      appInstance.embed(embedForce);
    } else if (cmdUpdate->parsed()) {
      appInstance.update();
    } else if (cmdWatch->parsed()) {
      appInstance.watch(watchInterval);
    } else if (cmdSearch->parsed()) {
      appInstance.search(searchQuery, searchTopk);
    } else if (cmdStats->parsed()) {
      appInstance.stats();
    } else if (cmdClear->parsed()) {
      appInstance.clear(clearNoConfirm);
    } else if (cmdCompact->parsed()) {
      appInstance.compact();
    } else if (cmdChat->parsed()) {
      appInstance.chat();
    } else if (cmdProviders->parsed()) {
      appInstance.providers(testProvider);
    } else if (cmdEval->parsed()) {
      eval::Evaluator eval(appInstance);
      if (evalInitFlag) {
        eval.init(evalOutputPath, evalSamples);
      } else if (evalListFlag) {
        eval.list();
      } else if (!evalDetailId.empty()) {
        eval.detail(evalDetailId);
      } else if (evalAddVectorsFlag) {
        if (evalDatasetPath.empty()) {
          LOG_MSG << "--add-vectors requires --dataset <path>";
          return 1;
        }
        eval.addVectors(evalDatasetPath, evalOutputPath);
      } else if (evalSweepFlag) {
        eval.sweep(evalDatasetPath, evalTopKs, evalBm25Weights, evalGrepWeights);
      } else if (!evalDatasetPath.empty()) {
        eval.run(evalDatasetPath,
          evalTopKs.empty() ? 5 : evalTopKs.front(),
          evalBm25Weights.empty() ? kHybridBm25Weight : evalBm25Weights.front(),
          evalGrepWeights.empty() ? kGrepRrfWeight : evalGrepWeights.front());
      } else {
        LOG_MSG << "Specify --dataset <path>, --init, --list, or --detail <id>";
        LOG_MSG << "  eval --init --output eval.json    Generate template";
        LOG_MSG << "  eval --list                       Show available doc IDs";
        LOG_MSG << "  eval --detail <doc_id>            Show doc details";
        LOG_MSG << "  eval --dataset eval.json          Run evaluation";
      }
    } else if (cmdServe->parsed()) {
      if (!serveNoConfirm && appInstance.auth().isDefaultPassword()) {
        std::cout << "\n  WARNING: You are using the default admin password!\n";
        std::cout << "This is a security risk. Please change it:\n";
        std::cout << "  " << binaryName() << " reset-password --pass <new_password>\n\n";
        std::cout << "Continue anyway? [y/N]: ";

        std::string response;
        std::getline(std::cin, response);

        if (response != "y" && response != "Y") {
          LOG_MSG << "Server start cancelled. Please reset password first.";
          return 1;
        }
      }
      appInstance.serve(servePort, serveWatch, serveWatchInterval, infoFile);
    } else {
      // No command specified, show help
      std::cout << app.help() << std::endl;
      return 1;
    }

  } catch (const CLI::ParseError &e) {
    // CLI11 has built-in formatting
    return app.exit(e);
  } catch (const std::filesystem::filesystem_error &e) {
    LOG_MSG_ERROR << "Filesystem error: " << e.what();
    LOG_MSG_ERROR << "Path: " << e.path1();
    return 1;
  } catch (const std::exception &e) {
    LOG_MSG_ERROR << "Error: " << e.what();
    LOG_MSG_ERROR << "Run with --help for usage information";
    return 1;
  }
  return 0;
}
