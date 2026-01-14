#pragma once

#include <vector>
#include <string>
#include <string_view>

#include <3rdparty/nlohmann/json.hpp>
using Json = nlohmann::json;

inline std::vector<std::string> json_keys(const Json &obj) {
  std::vector<std::string> keys;
  keys.reserve(obj.size());
  for (auto it = obj.begin(); it != obj.end(); ++it) {
    keys.push_back(it.key());
  }
  return keys;
}

inline double json_to_double(const Json &v) {
  if (v.is_number())
    return v.get<double>();
  if (v.is_string())
    return std::strtod(v.get<std::string>().c_str(), nullptr);
  return 0.0;
}

template <class T>
T json_val(const Json &obj, std::string_view key, const T &def = {}) {
  return obj.value<T>(key, def);
}

inline std::string json_str_val(const Json &obj, std::string_view key, const std::string &def = {}) {
  return obj.value<std::string>(key, def);
}
