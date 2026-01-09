#ifndef _INSTREGISTRY_H_
#define _INSTREGISTRY_H_

#include <memory>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

class Settings;

class InstanceRegistry {
public:
  explicit InstanceRegistry(const std::string &registryPath = {});
  explicit InstanceRegistry(int port, int watchInterval, const Settings &settings, const std::string &registryPath = {});
  ~InstanceRegistry();

  // non-copyable, but movable
  InstanceRegistry(const InstanceRegistry &) = delete;
  InstanceRegistry &operator=(const InstanceRegistry &) = delete;
  InstanceRegistry(InstanceRegistry &&) = default;
  InstanceRegistry &operator=(InstanceRegistry &&) = default;

  std::vector<json> getActiveInstances() const;
  std::string getInstanceId() const;

private:
  struct Impl;
  std::unique_ptr<Impl> imp;
};

#endif // _INSTREGISTRY_H_