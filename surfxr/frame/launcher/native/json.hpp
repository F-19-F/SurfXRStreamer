#pragma once
#include <json-c/json.h>
#include <glib.h>
#include <algorithm>
#include <cstdint>
#include <initializer_list>
#include <stdexcept>
#include <string>
#include <vector>

namespace surfxr::launcher {
// Private, value-semantic JSON adapter. Copies never share mutable config state.
class Json {
 public:
  Json() = default;
  explicit Json(json_object* value) : value_(value) {}
  Json(const char* value) : value_(json_object_new_string(value)) {}
  Json(const std::string& value) : value_(json_object_new_string_len(value.data(), static_cast<int>(value.size()))) {}
  Json(bool value) : value_(json_object_new_boolean(value)) {}
  Json(int value) : value_(json_object_new_int(value)) {}
  Json(int64_t value) : value_(json_object_new_int64(value)) {}
  Json(double value) : value_(json_object_new_double(value)) {}
  Json(const Json& other) { if (other.value_ && json_object_deep_copy(other.value_, &value_, nullptr)) throw std::bad_alloc(); }
  Json(Json&& other) noexcept : value_(other.value_) { other.value_ = nullptr; }
  Json& operator=(Json other) noexcept { std::swap(value_, other.value_); return *this; }
  ~Json() { if (value_) json_object_put(value_); }
  static Json object() { return Json(json_object_new_object()); }
  static Json array() { return Json(json_object_new_array()); }
  static Json parse(const std::string& text) {
    if (text.size() > 262144 || !g_utf8_validate(text.data(), static_cast<gssize>(text.size()), nullptr)) throw std::runtime_error("json_too_large_or_invalid");
    auto* tok = json_tokener_new_ex(16);
    if (!tok) throw std::bad_alloc();
    json_tokener_set_flags(tok, JSON_TOKENER_STRICT);
    auto input = text + " ";
    Json result(json_tokener_parse_ex(tok, input.data(), static_cast<int>(input.size())));
    const bool valid = json_tokener_get_error(tok) == json_tokener_success;
    const auto end = static_cast<size_t>(tok->char_offset);
    json_tokener_free(tok);
    if (!valid || input.find_first_not_of(" \r\n\t", end) != std::string::npos) throw std::runtime_error("invalid_json");
    return result;
  }
  bool is(json_type type) const { return json_object_is_type(value_, type); }
  bool has(const std::string& key) const { json_object* v = nullptr; return is(json_type_object) && json_object_object_get_ex(value_, key.c_str(), &v); }
  Json operator[](const std::string& key) const {
    json_object* v = nullptr;
    if (is(json_type_object)) json_object_object_get_ex(value_, key.c_str(), &v);
    Json result;
    if (v && json_object_deep_copy(v, &result.value_, nullptr)) throw std::bad_alloc();
    return result;
  }
  Json at(size_t index) const {
    auto* v = is(json_type_array) ? json_object_array_get_idx(value_, index) : nullptr;
    Json result;
    if (v && json_object_deep_copy(v, &result.value_, nullptr)) throw std::bad_alloc();
    return result;
  }
  size_t size() const { return is(json_type_array) ? json_object_array_length(value_) : keys().size(); }
  std::vector<std::string> keys() const {
    std::vector<std::string> result;
    if (is(json_type_object)) { json_object_object_foreach(value_, key, val) { (void)val; result.emplace_back(key); } }
    return result;
  }
  bool only(std::initializer_list<const char*> allowed) const {
    if (!is(json_type_object)) return false;
    for (const auto& key : keys()) if (std::none_of(allowed.begin(), allowed.end(), [&](const char* name) { return key == name; })) return false;
    return true;
  }
  void set(const std::string& key, Json value) {
    if (!is(json_type_object)) throw std::runtime_error("object_required");
    json_object_object_add(value_, key.c_str(), value.value_); value.value_ = nullptr;
  }
  void push(Json value) { json_object_array_add(value_, value.value_); value.value_ = nullptr; }
  std::string text(const std::string& fallback = "") const {
    return is(json_type_string) ? std::string(json_object_get_string(value_), json_object_get_string_len(value_)) : fallback;
  }
  int64_t integer(int64_t fallback = 0) const { return is(json_type_int) ? json_object_get_int64(value_) : fallback; }
  double number(double fallback = 0) const { return is(json_type_int) || is(json_type_double) ? json_object_get_double(value_) : fallback; }
  bool boolean(bool fallback = false) const { return is(json_type_boolean) ? json_object_get_boolean(value_) : fallback; }
  std::string dump() const { return value_ ? json_object_to_json_string_ext(value_, JSON_C_TO_STRING_PLAIN) : "null"; }
 private:
  json_object* value_ = nullptr;
};
inline Json object(std::initializer_list<std::pair<std::string, Json>> fields) {
  auto result = Json::object(); for (const auto& item : fields) result.set(item.first, item.second); return result;
}
}
