#ifndef LUA_RUNTIME_STUB

#include <cctype>
#include <cstring>
#include <fstream>
#include <limits>
#include <string>
#include <vector>

#include "esphome/components/network/util.h"
#include "esphome/core/application.h"
#include "esphome/core/log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_http_client.h"

#if CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
#include "esp_crt_bundle.h"
#endif

#include "lua.hpp"
#include "lua_runtime.h"

namespace {

static const char *const TAG = "lua_http";
static const uint32_t HTTP_DEFAULT_TIMEOUT_MS = 10 * 60 * 1000UL;
static const size_t HTTP_BUFFER_SIZE = 512;
static const int HTTP_MAX_REDIRECTS = 5;

struct LuaHttpHeader {
  std::string name;
  std::string value;
};

struct LuaHttpEventData {
  std::vector<LuaHttpHeader> *response_headers;
  bool debug;
};

struct LuaHttpOptions {
  uint32_t timeout_ms{HTTP_DEFAULT_TIMEOUT_MS};
  bool timeout_forever{false};
  std::string dst;
  std::string adapter;
  bool debug{false};
  bool ipv6{false};
  int callback_ref{LUA_NOREF};
  int userdata_ref{LUA_NOREF};
};

static std::string to_upper(std::string s) {
  for (auto &ch : s) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
  return s;
}

static esp_err_t lua_http_event_handler(esp_http_client_event_t *evt) {
  auto *user_data = static_cast<LuaHttpEventData *>(evt->user_data);
  if (user_data == nullptr || user_data->response_headers == nullptr) {
    return ESP_OK;
  }

  if (evt->event_id == HTTP_EVENT_ON_HEADER && evt->header_key != nullptr && evt->header_value != nullptr) {
    user_data->response_headers->push_back({evt->header_key, evt->header_value});
    if (user_data->debug) {
      ESP_LOGD(TAG, "response header: %s=%s", evt->header_key, evt->header_value);
    }
  }
  return ESP_OK;
}

static void lua_http_cleanup_refs(lua_State *L, LuaHttpOptions &opts) {
  if (opts.callback_ref != LUA_NOREF) {
    luaL_unref(L, LUA_REGISTRYINDEX, opts.callback_ref);
    opts.callback_ref = LUA_NOREF;
  }
  if (opts.userdata_ref != LUA_NOREF) {
    luaL_unref(L, LUA_REGISTRYINDEX, opts.userdata_ref);
    opts.userdata_ref = LUA_NOREF;
  }
}

static bool lua_http_parse_headers(lua_State *L, int idx, std::vector<LuaHttpHeader> &headers, std::string &error) {
  idx = lua_absindex(L, idx);
  if (lua_isnoneornil(L, idx)) {
    return true;
  }
  if (!lua_istable(L, idx)) {
    error = "headers must be a table or nil";
    return false;
  }

  lua_pushnil(L);
  while (lua_next(L, idx) != 0) {
    size_t key_len = 0;
    size_t value_len = 0;

    const char *key = luaL_tolstring(L, -2, &key_len);
    std::string key_copy = key != nullptr ? std::string(key, key_len) : std::string();
    lua_pop(L, 1);

    const char *value = luaL_tolstring(L, -1, &value_len);
    std::string value_copy = value != nullptr ? std::string(value, value_len) : std::string();
    lua_pop(L, 1);

    headers.push_back({key_copy, value_copy});
    lua_pop(L, 1);
  }

  return true;
}

static bool lua_http_parse_opts(lua_State *L, int idx, LuaHttpOptions &opts, std::string &error) {
  idx = lua_absindex(L, idx);
  if (lua_isnoneornil(L, idx)) {
    return true;
  }
  if (!lua_istable(L, idx)) {
    error = "opts must be a table or nil";
    return false;
  }

  lua_getfield(L, idx, "timeout");
  if (!lua_isnil(L, -1)) {
    lua_Integer timeout_ms = luaL_checkinteger(L, -1);
    if (timeout_ms < 0) {
      lua_pop(L, 1);
      error = "opts.timeout must be >= 0";
      return false;
    }
    if (timeout_ms == 0) {
      opts.timeout_forever = true;
      opts.timeout_ms = std::numeric_limits<uint32_t>::max();
    } else {
      opts.timeout_ms = static_cast<uint32_t>(timeout_ms);
    }
  }
  lua_pop(L, 1);

  lua_getfield(L, idx, "dst");
  if (!lua_isnil(L, -1)) {
    size_t len = 0;
    const char *dst = luaL_checklstring(L, -1, &len);
    opts.dst.assign(dst, len);
  }
  lua_pop(L, 1);

  lua_getfield(L, idx, "adapter");
  if (!lua_isnil(L, -1)) {
    size_t len = 0;
    const char *adapter = luaL_checklstring(L, -1, &len);
    opts.adapter.assign(adapter, len);
  }
  lua_pop(L, 1);

  lua_getfield(L, idx, "debug");
  if (!lua_isnil(L, -1)) {
    opts.debug = lua_toboolean(L, -1);
  }
  lua_pop(L, 1);

  lua_getfield(L, idx, "ipv6");
  if (!lua_isnil(L, -1)) {
    opts.ipv6 = lua_toboolean(L, -1);
  }
  lua_pop(L, 1);

  lua_getfield(L, idx, "callback");
  if (!lua_isnil(L, -1)) {
    if (!lua_isfunction(L, -1)) {
      lua_pop(L, 1);
      error = "opts.callback must be a function";
      return false;
    }
    lua_pushvalue(L, -1);
    opts.callback_ref = luaL_ref(L, LUA_REGISTRYINDEX);
  }
  lua_pop(L, 1);

  lua_getfield(L, idx, "userdata");
  if (!lua_isnil(L, -1)) {
    lua_pushvalue(L, -1);
    opts.userdata_ref = luaL_ref(L, LUA_REGISTRYINDEX);
  }
  lua_pop(L, 1);

  return true;
}

static bool lua_http_parse_method(const std::string &method, esp_http_client_method_t &method_idf) {
  const std::string upper = to_upper(method);
  if (upper == "GET") method_idf = HTTP_METHOD_GET;
  else if (upper == "POST") method_idf = HTTP_METHOD_POST;
  else if (upper == "PUT") method_idf = HTTP_METHOD_PUT;
  else if (upper == "PATCH") method_idf = HTTP_METHOD_PATCH;
  else if (upper == "DELETE") method_idf = HTTP_METHOD_DELETE;
  else if (upper == "HEAD") method_idf = HTTP_METHOD_HEAD;
  else if (upper == "OPTIONS") method_idf = HTTP_METHOD_OPTIONS;
  else if (upper == "COPY") method_idf = HTTP_METHOD_COPY;
  else if (upper == "MOVE") method_idf = HTTP_METHOD_MOVE;
  else if (upper == "LOCK") method_idf = HTTP_METHOD_LOCK;
  else if (upper == "UNLOCK") method_idf = HTTP_METHOD_UNLOCK;
  else if (upper == "PROPFIND") method_idf = HTTP_METHOD_PROPFIND;
  else if (upper == "PROPPATCH") method_idf = HTTP_METHOD_PROPPATCH;
  else if (upper == "MKCOL") method_idf = HTTP_METHOD_MKCOL;
  else if (upper == "REPORT") method_idf = HTTP_METHOD_REPORT;
  else if (upper == "NOTIFY") method_idf = HTTP_METHOD_NOTIFY;
  else if (upper == "SUBSCRIBE") method_idf = HTTP_METHOD_SUBSCRIBE;
  else if (upper == "UNSUBSCRIBE") method_idf = HTTP_METHOD_UNSUBSCRIBE;
  else return false;
  return true;
}

static bool lua_http_is_complete(int status_code, bool chunked, size_t content_length, size_t bytes_read,
                                 esp_http_client_handle_t client) {
  if ((status_code >= 100 && status_code < 200) || status_code == 204 || status_code == 205 || status_code == 304) {
    return true;
  }
  if (!chunked) {
    return bytes_read >= content_length;
  }
  return esp_http_client_is_complete_data_received(client);
}

static bool lua_http_call_progress(lua_State *L, const LuaHttpOptions &opts, size_t content_length, size_t body_length,
                                   std::string &error) {
  if (opts.callback_ref == LUA_NOREF) {
    return true;
  }

  lua_rawgeti(L, LUA_REGISTRYINDEX, opts.callback_ref);
  lua_pushinteger(L, static_cast<lua_Integer>(content_length));
  lua_pushinteger(L, static_cast<lua_Integer>(body_length));
  if (opts.userdata_ref != LUA_NOREF) {
    lua_rawgeti(L, LUA_REGISTRYINDEX, opts.userdata_ref);
  } else {
    lua_pushnil(L);
  }

  if (lua_pcall(L, 3, 0, 0) != LUA_OK) {
    const char *err = lua_tostring(L, -1);
    error = err != nullptr ? err : "progress callback failed";
    lua_pop(L, 1);
    return false;
  }

  return true;
}

static void lua_http_push_headers(lua_State *L, const std::vector<LuaHttpHeader> &headers) {
  lua_newtable(L);
  for (const auto &header : headers) {
    lua_pushlstring(L, header.name.data(), header.name.size());
    lua_pushlstring(L, header.value.data(), header.value.size());
    lua_settable(L, -3);
  }
}

static int lua_http_push_error(lua_State *L, int code) {
  lua_pushinteger(L, code);
  lua_pushnil(L);
  lua_pushnil(L);
  return 3;
}

static int lua_http_request(lua_State *L) {
  size_t method_len = 0;
  size_t url_len = 0;
  const char *method_arg = luaL_checklstring(L, 1, &method_len);
  const char *url_arg = luaL_checklstring(L, 2, &url_len);
  std::string method(method_arg, method_len);
  std::string url(url_arg, url_len);
  std::vector<LuaHttpHeader> request_headers;
  std::vector<LuaHttpHeader> response_headers;
  std::string parse_error;
  LuaHttpOptions opts;

  if (!lua_http_parse_headers(L, 3, request_headers, parse_error)) {
    lua_http_cleanup_refs(L, opts);
    return luaL_error(L, "%s", parse_error.c_str());
  }
  if (!lua_http_parse_opts(L, 5, opts, parse_error)) {
    lua_http_cleanup_refs(L, opts);
    return luaL_error(L, "%s", parse_error.c_str());
  }

  size_t body_len = 0;
  const char *body_ptr = "";
  if (!lua_isnoneornil(L, 4)) {
    body_ptr = luaL_checklstring(L, 4, &body_len);
  }
  std::string body(body_ptr, body_len);

  size_t ca_len = 0;
  size_t client_ca_len = 0;
  size_t client_key_len = 0;
  size_t client_password_len = 0;
  const char *ca_pem = nullptr;
  const char *client_ca_pem = nullptr;
  const char *client_key_pem = nullptr;
  const char *client_password = nullptr;

  if (!lua_isnoneornil(L, 6)) ca_pem = luaL_checklstring(L, 6, &ca_len);
  if (!lua_isnoneornil(L, 7)) client_ca_pem = luaL_checklstring(L, 7, &client_ca_len);
  if (!lua_isnoneornil(L, 8)) client_key_pem = luaL_checklstring(L, 8, &client_key_len);
  if (!lua_isnoneornil(L, 9)) client_password = luaL_checklstring(L, 9, &client_password_len);

  esp_http_client_method_t method_idf = HTTP_METHOD_GET;
  if (!lua_http_parse_method(method, method_idf)) {
    lua_http_cleanup_refs(L, opts);
    return lua_http_push_error(L, -ESP_ERR_INVALID_ARG);
  }

  if (!esphome::network::is_connected()) {
    if (opts.debug) {
      ESP_LOGW(TAG, "request skipped because network is disconnected");
    }
    lua_http_cleanup_refs(L, opts);
    return lua_http_push_error(L, -ESP_ERR_INVALID_STATE);
  }

  if (!opts.adapter.empty()) {
    ESP_LOGW(TAG, "adapter option is not implemented yet, ignored: %s", opts.adapter.c_str());
  }

  const bool secure = url.rfind("https://", 0) == 0;
  if (opts.debug) {
    ESP_LOGI(TAG, "request %s %s (body=%u bytes, download=%s, ipv6=%s)", method.c_str(), url.c_str(),
             static_cast<unsigned>(body.size()), opts.dst.empty() ? "no" : "yes", YESNO(opts.ipv6));
  }

  LuaHttpEventData event_data{&response_headers, opts.debug};
  esp_http_client_config_t config = {};
  config.url = url.c_str();
  config.method = method_idf;
  config.timeout_ms = opts.timeout_forever ? std::numeric_limits<int>::max() : static_cast<int>(opts.timeout_ms);
  config.disable_auto_redirect = true;
  config.max_redirection_count = HTTP_MAX_REDIRECTS;
  config.user_agent = "Lemonade Lua HTTP";
  config.auth_type = HTTP_AUTH_TYPE_NONE;
  config.buffer_size = HTTP_BUFFER_SIZE;
  config.buffer_size_tx = HTTP_BUFFER_SIZE;
  config.event_handler = lua_http_event_handler;
  config.user_data = &event_data;
  config.addr_type = opts.ipv6 ? HTTP_ADDR_TYPE_INET6 : HTTP_ADDR_TYPE_UNSPEC;

  if (secure) {
    if (ca_pem != nullptr && ca_len > 0) {
      config.cert_pem = ca_pem;
      config.cert_len = ca_len + 1;
#if CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
    } else {
      config.crt_bundle_attach = esp_crt_bundle_attach;
#endif
    }

    if (client_ca_pem != nullptr && client_ca_len > 0) {
      config.client_cert_pem = client_ca_pem;
      config.client_cert_len = client_ca_len + 1;
    }
    if (client_key_pem != nullptr && client_key_len > 0) {
      config.client_key_pem = client_key_pem;
      config.client_key_len = client_key_len + 1;
    }
    if (client_password != nullptr && client_password_len > 0) {
      config.client_key_password = client_password;
      config.client_key_password_len = client_password_len;
    }
  }

  esp_http_client_handle_t client = esp_http_client_init(&config);
  if (client == nullptr) {
    lua_http_cleanup_refs(L, opts);
    return lua_http_push_error(L, -ESP_ERR_NO_MEM);
  }

  int result_code = -ESP_FAIL;
  size_t content_length = 0;
  size_t total_body_len = 0;
  bool chunked = false;
  bool ok = false;
  const bool download_mode = !opts.dst.empty();
  std::string response_body;
  std::ofstream output;
  std::string callback_error;

  for (const auto &header : request_headers) {
    esp_http_client_set_header(client, header.name.c_str(), header.value.c_str());
  }

  do {
    esphome::lua_runtime::lua_abort_if_ota(L);

    esp_err_t err = esp_http_client_open(client, static_cast<int>(body.size()));
    if (err != ESP_OK) {
      result_code = -err;
      break;
    }

    if (!body.empty()) {
      int remaining = static_cast<int>(body.size());
      int offset = 0;
      while (remaining > 0) {
        esphome::lua_runtime::lua_abort_if_ota(L);
        int written = esp_http_client_write(client, body.data() + offset, remaining);
        if (written < 0) {
          result_code = written;
          break;
        }
        remaining -= written;
        offset += written;
      }
      if (remaining > 0) {
        break;
      }
    }

    content_length = static_cast<size_t>(esp_http_client_fetch_headers(client));
    chunked = esp_http_client_is_chunked_response(client);
    result_code = esp_http_client_get_status_code(client);

    int redirects_left = HTTP_MAX_REDIRECTS;
    while ((result_code == 301 || result_code == 302 || result_code == 303 || result_code == 307 || result_code == 308) &&
           redirects_left > 0) {
      if (opts.debug) {
        ESP_LOGI(TAG, "redirect status=%d", result_code);
      }
      response_headers.clear();
      err = esp_http_client_set_redirection(client);
      if (err != ESP_OK) {
        result_code = -err;
        break;
      }
      err = esp_http_client_open(client, 0);
      if (err != ESP_OK) {
        result_code = -err;
        break;
      }
      content_length = static_cast<size_t>(esp_http_client_fetch_headers(client));
      chunked = esp_http_client_is_chunked_response(client);
      result_code = esp_http_client_get_status_code(client);
      redirects_left--;
    }

    if (result_code < 100) {
      break;
    }

    if (download_mode) {
      output.open(opts.dst, std::ios::binary | std::ios::trunc);
      if (!output.is_open()) {
        result_code = -ESP_FAIL;
        break;
      }
    } else if (content_length > 0 && content_length < (256 * 1024)) {
      response_body.reserve(content_length);
    }

    if (!lua_http_call_progress(L, opts, content_length, total_body_len, callback_error)) {
      result_code = -ESP_FAIL;
      break;
    }

    uint8_t buffer[HTTP_BUFFER_SIZE];
    uint32_t last_data_time = esphome::millis();
    while (true) {
      esphome::lua_runtime::lua_abort_if_ota(L);
      int read_len = esp_http_client_read(client, reinterpret_cast<char *>(buffer), sizeof(buffer));
      esphome::App.feed_wdt();

      if (read_len > 0) {
        total_body_len += static_cast<size_t>(read_len);
        last_data_time = esphome::millis();
        if (download_mode) {
          output.write(reinterpret_cast<const char *>(buffer), read_len);
          if (!output.good()) {
            result_code = -ESP_FAIL;
            break;
          }
        } else {
          response_body.append(reinterpret_cast<const char *>(buffer), read_len);
        }

        if (!lua_http_call_progress(L, opts, content_length, total_body_len, callback_error)) {
          result_code = -ESP_FAIL;
          break;
        }
        continue;
      }

      if (read_len < 0) {
        result_code = read_len;
        break;
      }

      if (lua_http_is_complete(result_code, chunked, content_length, total_body_len, client)) {
        ok = true;
        break;
      }

      if (!opts.timeout_forever && esphome::millis() - last_data_time >= opts.timeout_ms) {
        result_code = -ESP_ERR_TIMEOUT;
        break;
      }

      vTaskDelay(pdMS_TO_TICKS(1));
    }
  } while (false);

  if (output.is_open()) {
    output.close();
  }
  esp_http_client_close(client);
  esp_http_client_cleanup(client);

  if (!callback_error.empty()) {
    ESP_LOGE(TAG, "callback error: %s", callback_error.c_str());
  }

  if (!ok) {
    if (opts.debug) {
      ESP_LOGW(TAG, "request failed: code=%d", result_code);
    }
    lua_http_cleanup_refs(L, opts);
    return lua_http_push_error(L, result_code);
  }

  if (opts.debug) {
    ESP_LOGI(TAG, "request finished: code=%d, body=%u bytes, chunked=%s", result_code,
             static_cast<unsigned>(total_body_len), YESNO(chunked));
  }

  lua_pushinteger(L, result_code);
  lua_http_push_headers(L, response_headers);
  if (download_mode) {
    lua_pushinteger(L, static_cast<lua_Integer>(total_body_len));
  } else {
    lua_pushlstring(L, response_body.data(), response_body.size());
  }
  lua_http_cleanup_refs(L, opts);
  return 3;
}

static const luaL_Reg HTTP_FUNCS[] = {
    {"request", lua_http_request},
    {"request_async", lua_http_request},
    {nullptr, nullptr},
};

}  // namespace

extern "C" int luaopen_http(lua_State *L) {
  luaL_newlib(L, HTTP_FUNCS);
  return 1;
}

#endif  // LUA_RUNTIME_STUB
