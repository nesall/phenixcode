#include "settings.h"
#include <iostream>
#include <stdexcept>

using nlohmann::json;
using Mode = Settings::ReconcileMode;

namespace {
  int checks = 0;
  void check(bool condition, const char *label) {
    ++checks;
    if (!condition) throw std::runtime_error(label);
  }
  json catalog() {
    return json::parse(R"({
      "embedding_providers":[{"id":"disabled-embedding","enabled":false},{"id":"e1"},{"id":"e2"}],
      "generation_providers":[{"id":"disabled-generation","enabled":false},{"id":"auto"},{"id":"g1"},{"id":"g2"}]
    })");
  }
  json project() {
    return json::parse(R"({
      "source":{"project_id":"original","project_title":"Original"},
      "database":{"sqlite_path":"untouched.db"},"chunking":{},"custom":{"keep":true},
      "embedding":{"current_api":"e1","batch_size":4},
      "generation":{"current_api":"g1","enabled_providers":["g1","g2"],"temperature":0.4,
        "auto_router":{"enabled":true,"classifier":{"type":"api","api_id":"g1","prompt":"Classify"},
          "fallback":{"default_model_id":"g1"},
          "routing_rules":{"tier_1_simple":{"strategy":"direct","direct_model_id":"g1"}}}}
    })");
  }
}

int main(int argc, char **argv) {
  try {
    if (argc > 1 && std::string(argv[1]) == "--bridge") {
      json args;
      std::cin >> args;
      ProvidersSettings providers;
      providers.updateFromConfig(args.at("providers"));
      Settings::ProviderRenames renames;
      const auto mapping = args.value("renames", json::object());
      renames.embedding = mapping.value("embedding", std::map<std::string, std::string>{});
      renames.generation = mapping.value("generation", std::map<std::string, std::string>{});
      const auto result = Settings::reconcileProject(args.at("project").at("jsonData"), providers, renames);
      std::cout << json{{"status", "success"}, {"jsonData", result.jsonData}, {"changed", result.changed},
        {"needsReindex", result.needsReindex}, {"dropped", result.dropped}, {"needsSelection", result.needsSelection}}.dump();
      return 0;
    }
    const auto rawCatalog = catalog();
    ProvidersSettings providers;
    providers.updateFromConfig(rawCatalog);
    auto blank = Settings::createBlank(providers);
    const auto blankJson = blank.configJson();
    check(blank.configPath().empty(), "blank factory does not assign a file");
    check(blankJson["embedding"]["current_api"] == "e1", "blank selects first eligible embedding");
    check(blankJson["generation"]["enabled_providers"] == json::array({"g1"}), "blank selects first eligible generation");
    check(blankJson["generation"]["auto_router"]["routing_rules"].size() == 3, "blank keeps routing tiers");
    check(blankJson["generation"]["default_temperature"] == 0.1 && blankJson["chunking"]["semantic"] == true,
      "blank preserves product defaults");
    check(blankJson["source"]["project_id"] == "" && blankJson["source"]["global_exclude"].size() > 10,
      "blank preserves source defaults and leaves ID unassigned");
    blank.validate();
    auto independent = Settings::createBlank(providers);
    blank.updateFromConfig({{"source", {{"project_title", "Changed"}}}});
    check(independent.getProjectTitle() == "Default Project Title", "blank instances are independent");
    const auto original = project();
    const auto stable = Settings::reconcileProject(original, providers);
    check(!stable.changed && stable.jsonData == original, "valid project stays identical");
    check(!stable.needsReindex && stable.needsSelection.empty() && stable.dropped.empty(), "valid project has empty report");
    Settings valid(stable.jsonData, rawCatalog);

    auto stale = original;
    stale["embedding"]["current_api"] = "old-embedding";
    stale["generation"]["current_api"] = "old-generation";
    stale["generation"]["enabled_providers"] = {"old-generation"};
    stale["generation"]["auto_router"]["classifier"]["api_id"] = "old-generation";
    stale["generation"]["auto_router"]["fallback"]["default_model_id"] = "old-generation";
    stale["generation"]["auto_router"]["routing_rules"]["tier_1_simple"]["direct_model_id"] = "old-generation";
    const auto created = Settings::reconcileProject(stale, providers, {}, Mode::NewProject);
    check(created.jsonData["embedding"]["current_api"] == "e1", "new project selects first eligible embedding");
    check(created.jsonData["generation"]["enabled_providers"] == json::array({"g1"}), "new project replaces fully stale selection");
    check(created.jsonData["generation"]["current_api"] == "g1", "new project repairs current API");
    check(!created.needsReindex && created.needsSelection.empty(), "new project has usable provider selections");
    Settings createdSettings(created.jsonData, rawCatalog);
    check(created.jsonData["source"] == original["source"] && created.jsonData["database"] == original["database"] &&
      created.jsonData["custom"] == original["custom"], "unrelated project content preserved");
    check(stale["embedding"]["current_api"] == "old-embedding", "input JSON is never mutated");
    check(!Settings::reconcileProject(created.jsonData, providers, {}, Mode::NewProject).changed, "new reconciliation is idempotent");

    const auto existing = Settings::reconcileProject(stale, providers);
    check(existing.jsonData["embedding"]["current_api"] == "", "existing project requires explicit replacement embedding");
    check(existing.jsonData["generation"]["enabled_providers"].empty(), "existing project enables no new models");
    check(existing.jsonData["generation"]["current_api"] == "", "existing empty selection clears current model");
    check(existing.needsReindex && existing.needsSelection.size() == 2, "existing invalid selections reported");
    check(existing.jsonData["generation"]["auto_router"]["enabled"] == false, "empty selection disables router");
    check(existing.dropped == std::vector<std::string>{"old-generation"}, "removed provider reported");

    Settings::ProviderRenames renames;
    renames.embedding["old-embedding"] = "e1";
    renames.generation["old-generation"] = "g1";
    const auto renamed = Settings::reconcileProject(stale, providers, renames);
    check(renamed.jsonData["embedding"]["current_api"] == "e1" && !renamed.needsReindex, "embedding ID rename avoids unnecessary reindex");
    check(renamed.jsonData["generation"]["auto_router"]["classifier"]["api_id"] == "g1", "API classifier renamed");
    check(renamed.needsSelection.empty(), "renames preserve selections");
    Settings renamedSettings(renamed.jsonData, rawCatalog);

    auto internal = original;
    internal["generation"]["auto_router"]["classifier"] = {{"type", "internal"}, {"api_id", "unused"}, {"prompt", "Classify"}};
    renames.generation["unused"] = "g2";
    auto internalResult = Settings::reconcileProject(internal, providers, renames);
    check(!internalResult.changed, "internal classifier unused API ID preserved");
    Settings internalSettings(internalResult.jsonData, rawCatalog);

    auto duplicates = original;
    duplicates["generation"]["enabled_providers"] = {"g2", "g2", "missing", "disabled-generation", "auto"};
    const auto deduplicated = Settings::reconcileProject(duplicates, providers);
    check(deduplicated.jsonData["generation"]["enabled_providers"] == json::array({"g2"}), "deduplicate, filter disabled, remove legacy auto");
    check(deduplicated.jsonData["generation"]["current_api"] == "g2", "fallback uses existing enabled selection");
    check(deduplicated.jsonData["generation"]["auto_router"]["classifier"]["api_id"] == "g2", "API classifier must be project-enabled");
    check(deduplicated.dropped.size() == 3, "all unavailable IDs reported");
    check(!Settings::reconcileProject(deduplicated.jsonData, providers).changed, "existing reconciliation is idempotent");
    Settings deduplicatedSettings(deduplicated.jsonData, rawCatalog);

    auto ensemble = original;
    auto &rule = ensemble["generation"]["auto_router"]["routing_rules"]["tier_1_simple"];
    rule = {{"strategy", "ensemble"}, {"draft_model_ids", {"old-generation", "old-generation", "missing"}},
      {"synthesizer_model_id", "old-generation"}};
    const auto draftResult = Settings::reconcileProject(ensemble, providers, renames);
    const auto &draftRule = draftResult.jsonData["generation"]["auto_router"]["routing_rules"]["tier_1_simple"];
    check(draftRule["draft_model_ids"] == json::array({"g1"}) && draftRule["synthesizer_model_id"] == "g1", "draft and synthesizer IDs renamed and filtered");
    rule["draft_model_ids"] = {"missing"};
    const auto direct = Settings::reconcileProject(ensemble, providers);
    check(direct.jsonData["generation"]["auto_router"]["routing_rules"]["tier_1_simple"]["strategy"] == "direct", "empty ensemble falls back to direct");
    Settings directSettings(direct.jsonData, rawCatalog);

    ProvidersSettings empty;
    empty.updateFromConfig({{"embedding_providers", json::array()}, {"generation_providers", json::array()}});
    bool blankRejected = false;
    try { Settings::createBlank(empty); } catch (const std::exception &) { blankRejected = true; }
    check(blankRejected, "blank rejects catalogs without eligible providers");
    const auto noProviders = Settings::reconcileProject(original, empty, {}, Mode::NewProject);
    check(noProviders.needsSelection.size() == 2, "empty catalogs require selections for creation");
    bool rejected = false;
    try { Settings invalid(noProviders.jsonData, empty.configJson()); } catch (const std::exception &) { rejected = true; }
    check(rejected, "reconciliation and validation remain separate");
    const auto missingSections = Settings::reconcileProject(json::object(), providers, {}, Mode::NewProject);
    check(missingSections.jsonData["generation"]["current_api"] == "g1", "missing provider sections initialized");
    rejected = false;
    try { Settings::reconcileProject(json::array(), providers); } catch (const std::exception &) { rejected = true; }
    check(rejected, "malformed project rejected");
    std::cout << checks << " reconciliation checks passed.\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
