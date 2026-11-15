#ifndef LUA_RUNTIME_STUB

#include <atomic>
#include <cctype>
#include <cstring>
#include <fstream>
#include <functional>
#include <limits>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "esphome/components/network/util.h"
#include "esphome/core/application.h"
#include "esphome/core/log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_heap_caps.h"
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
// HTTPS/TLS + Lua + JSON needs a lot of stack; this worker is separate from the Lua task.
static const uint32_t HTTP_WORKER_STACK = 20480;
static const size_t HTTP_MAX_INFLIGHT = 4;

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

struct HttpRequest {
  std::string method;
  std::string url;
  std::vector<LuaHttpHeader> headers;
  std::string body;
  std::string dst;
  std::string adapter;
  std::string ca_pem;
  std::string client_ca_pem;
  std::string client_key_pem;
  std::string client_password;
  uint32_t timeout_ms{HTTP_DEFAULT_TIMEOUT_MS};
  bool timeout_forever{false};
  bool debug{false};
  bool ipv6{false};
};

struct HttpResult {
  int code{-ESP_FAIL};
  std::vector<LuaHttpHeader> headers;
  std::string body;
  size_t body_len{0};
  bool download{false};
  bool ok{false};
};

// One in-flight async request. The worker owns a shared_ptr, the job table owns another
// one; cancellation just removes the table entry and lets the worker finish/free.
struct HttpJob {
  uint32_t id{0};
  std::string owner;
  HttpRequest req;
  HttpResult result;
  int co_ref{LUA_NOREF};
  std::atomic<bool> cancelled{false};
  bool done{false};        // guarded by g_jobs_lock
  bool dispatched{false};  // guarded by g_jobs_lock
};

static SemaphoreHandle_t g_jobs_lock = nullptr;
static std::unordered_map<uint32_t, std::shared_ptr<HttpJob>> g_jobs;
static uint32_t g_next_job_id = 1;

static void ensure_jobs_lock() {
  if (g_jobs_lock == nullptr) g_jobs_lock = xSemaphoreCreateMutex();
}

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

static bool lua_http_parse_request(lua_State *L, HttpRequest &req, LuaHttpOptions &opts, std::string &error) {
  size_t method_len = 0;
  size_t url_len = 0;
  const char *method_arg = luaL_checklstring(L, 1, &method_len);
  const char *url_arg = luaL_checklstring(L, 2, &url_len);
  req.method.assign(method_arg, method_len);
  req.url.assign(url_arg, url_len);

  if (!lua_http_parse_headers(L, 3, req.headers, error)) {
    return false;
  }
  if (!lua_http_parse_opts(L, 5, opts, error)) {
    return false;
  }

  req.timeout_ms = opts.timeout_ms;
  req.timeout_forever = opts.timeout_forever;
  // Move (not copy) so that yielding out of http.request cannot leak these strings: the
  // coroutine's C frame is longjmp'd and its C++ destructors do not run.
  req.dst = std::move(opts.dst);
  req.adapter = std::move(opts.adapter);
  req.debug = opts.debug;
  req.ipv6 = opts.ipv6;

  if (!lua_isnoneornil(L, 4)) {
    size_t body_len = 0;
    const char *body = luaL_checklstring(L, 4, &body_len);
    req.body.assign(body, body_len);
  }

  auto copy_arg = [L, &error](int idx, std::string &out) {
    if (lua_isnoneornil(L, idx)) return;
    size_t len = 0;
    const char *value = luaL_checklstring(L, idx, &len);
    if (value == nullptr) {
      error = "PEM argument must be a string";
      return;
    }
    out.assign(value, len);
  };
  copy_arg(6, req.ca_pem);
  copy_arg(7, req.client_ca_pem);
  copy_arg(8, req.client_key_pem);
  copy_arg(9, req.client_password);

  return error.empty();
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

// Pure C++ request core, used by both the blocking path (Lua task) and the async worker.
static void lua_http_perform(const HttpRequest &req, HttpResult &out, const std::function<bool()> &should_abort,
                             const std::function<bool(size_t, size_t)> &progress) {
  out.download = !req.dst.empty();
  out.code = -ESP_FAIL;
  out.ok = false;

  esp_http_client_method_t method_idf = HTTP_METHOD_GET;
  if (!lua_http_parse_method(req.method, method_idf)) {
    out.code = -ESP_ERR_INVALID_ARG;
    return;
  }

  if (!esphome::network::is_connected()) {
    if (req.debug) {
      ESP_LOGW(TAG, "request skipped because network is disconnected");
    }
    out.code = -ESP_ERR_INVALID_STATE;
    return;
  }

  if (!req.adapter.empty()) {
    ESP_LOGW(TAG, "adapter option is not implemented yet, ignored: %s", req.adapter.c_str());
  }

  const bool secure = req.url.rfind("https://", 0) == 0;
  if (req.debug) {
    ESP_LOGI(TAG, "request %s %s (body=%u bytes, download=%s, ipv6=%s)", req.method.c_str(), req.url.c_str(),
             static_cast<unsigned>(req.body.size()), req.dst.empty() ? "no" : "yes", YESNO(req.ipv6));
  }

  LuaHttpEventData event_data{&out.headers, req.debug};
  esp_http_client_config_t config = {};
  config.url = req.url.c_str();
  config.method = method_idf;
  config.timeout_ms = req.timeout_forever ? std::numeric_limits<int>::max() : static_cast<int>(req.timeout_ms);
  config.disable_auto_redirect = true;
  config.max_redirection_count = HTTP_MAX_REDIRECTS;
  config.user_agent = "Lemonade Lua HTTP";
  config.auth_type = HTTP_AUTH_TYPE_NONE;
  config.buffer_size = HTTP_BUFFER_SIZE;
  config.buffer_size_tx = HTTP_BUFFER_SIZE;
  config.event_handler = lua_http_event_handler;
  config.user_data = &event_data;
  config.addr_type = req.ipv6 ? HTTP_ADDR_TYPE_INET6 : HTTP_ADDR_TYPE_UNSPEC;

  if (secure) {
    if (!req.ca_pem.empty()) {
      config.cert_pem = req.ca_pem.c_str();
      config.cert_len = req.ca_pem.size() + 1;
#if CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
    } else {
      config.crt_bundle_attach = esp_crt_bundle_attach;
#endif
    }

    if (!req.client_ca_pem.empty()) {
      config.client_cert_pem = req.client_ca_pem.c_str();
      config.client_cert_len = req.client_ca_pem.size() + 1;
    }
    if (!req.client_key_pem.empty()) {
      config.client_key_pem = req.client_key_pem.c_str();
      config.client_key_len = req.client_key_pem.size() + 1;
    }
    if (!req.client_password.empty()) {
      config.client_key_password = req.client_password.c_str();
      config.client_key_password_len = req.client_password.size();
    }
  }

  esp_http_client_handle_t client = esp_http_client_init(&config);
  if (client == nullptr) {
    out.code = -ESP_ERR_NO_MEM;
    return;
  }

  int result_code = -ESP_FAIL;
  size_t content_length = 0;
  size_t total_body_len = 0;
  bool chunked = false;
  bool ok = false;
  std::string response_body;
  std::ofstream output;

  for (const auto &header : req.headers) {
    esp_http_client_set_header(client, header.name.c_str(), header.value.c_str());
  }

  do {
    if (should_abort()) {
      result_code = -ESP_ERR_INVALID_STATE;
      break;
    }

    esp_err_t err = esp_http_client_open(client, static_cast<int>(req.body.size()));
    if (err != ESP_OK) {
      result_code = -err;
      break;
    }

    if (!req.body.empty()) {
      int remaining = static_cast<int>(req.body.size());
      int offset = 0;
      while (remaining > 0) {
        if (should_abort()) {
          result_code = -ESP_ERR_INVALID_STATE;
          break;
        }
        int written = esp_http_client_write(client, req.body.data() + offset, remaining);
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
      if (req.debug) {
        ESP_LOGI(TAG, "redirect status=%d", result_code);
      }
      out.headers.clear();
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

    if (out.download) {
      output.open(req.dst, std::ios::binary | std::ios::trunc);
      if (!output.is_open()) {
        result_code = -ESP_FAIL;
        break;
      }
    } else if (content_length > 0 && content_length < (256 * 1024)) {
      response_body.reserve(content_length);
    }

    if (progress && !progress(content_length, total_body_len)) {
      result_code = -ESP_FAIL;
      break;
    }

    uint8_t buffer[HTTP_BUFFER_SIZE];
    uint32_t last_data_time = esphome::millis();
    while (true) {
      if (should_abort()) {
        result_code = -ESP_ERR_INVALID_STATE;
        break;
      }
      int read_len = esp_http_client_read(client, reinterpret_cast<char *>(buffer), sizeof(buffer));
      esphome::App.feed_wdt();

      if (read_len > 0) {
        total_body_len += static_cast<size_t>(read_len);
        last_data_time = esphome::millis();
        if (out.download) {
          output.write(reinterpret_cast<const char *>(buffer), read_len);
          if (!output.good()) {
            result_code = -ESP_FAIL;
            break;
          }
        } else {
          response_body.append(reinterpret_cast<const char *>(buffer), read_len);
        }

        if (progress && !progress(content_length, total_body_len)) {
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

      if (!req.timeout_forever && esphome::millis() - last_data_time >= req.timeout_ms) {
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

  out.code = result_code;
  out.ok = ok;
  out.body = std::move(response_body);
  out.body_len = total_body_len;

  if (req.debug) {
    if (ok) {
      ESP_LOGI(TAG, "request finished: code=%d, body=%u bytes, chunked=%s", result_code,
               static_cast<unsigned>(total_body_len), YESNO(chunked));
    } else {
      ESP_LOGW(TAG, "request failed: code=%d", result_code);
    }
  }
}

static int lua_http_yield_cont(lua_State *L, int status, lua_KContext ctx) {
  (void) status;
  (void) ctx;
  // The resume arguments are on the stack; they become the return values of http.request.
  return lua_gettop(L);
}

static void lua_http_worker(void *arg);

// Starts a worker for a request made from a coroutine. Returns 0 when the worker is
// running (the caller must then yield with lua_http_yield_cont), or 3 with an error result
// already pushed when the request could not be started.
static int lua_http_start_async(lua_State *L, HttpRequest &req) {
  const std::string owner = esphome::lua_runtime::lua_runtime_script_path(L);
  if (owner.empty()) {
    return lua_http_push_error(L, -ESP_ERR_INVALID_STATE);
  }

  ensure_jobs_lock();
  if (g_jobs_lock == nullptr) {
    return lua_http_push_error(L, -ESP_ERR_NO_MEM);
  }
  auto job = std::make_shared<HttpJob>();
  job->owner = owner;
  job->req = std::move(req);
  lua_pushthread(L);
  job->co_ref = luaL_ref(L, LUA_REGISTRYINDEX);

  {
    xSemaphoreTake(g_jobs_lock, portMAX_DELAY);
    size_t inflight = 0;
    for (const auto &entry : g_jobs) {
      if (!entry.second->done) inflight++;
    }
    if (inflight >= HTTP_MAX_INFLIGHT) {
      xSemaphoreGive(g_jobs_lock);
      luaL_unref(L, LUA_REGISTRYINDEX, job->co_ref);
      return lua_http_push_error(L, -ESP_ERR_NO_MEM);
    }
    job->id = g_next_job_id++;
    g_jobs[job->id] = job;
    xSemaphoreGive(g_jobs_lock);
  }

  auto *holder = new std::shared_ptr<HttpJob>(job);
  BaseType_t ok = xTaskCreatePinnedToCoreWithCaps(lua_http_worker, "lua_http", HTTP_WORKER_STACK, holder, 1, nullptr, 0,
                                                  MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (ok != pdPASS) {
    ok = xTaskCreatePinnedToCore(lua_http_worker, "lua_http", HTTP_WORKER_STACK, holder, 1, nullptr, 0);
  }
  if (ok != pdPASS) {
    delete holder;
    xSemaphoreTake(g_jobs_lock, portMAX_DELAY);
    g_jobs.erase(job->id);
    xSemaphoreGive(g_jobs_lock);
    luaL_unref(L, LUA_REGISTRYINDEX, job->co_ref);
    return lua_http_push_error(L, -ESP_ERR_NO_MEM);
  }

  return 0;
}

static int lua_http_request(lua_State *L) {
  HttpRequest req;
  LuaHttpOptions opts;
  std::string parse_error;

  if (!lua_http_parse_request(L, req, opts, parse_error)) {
    lua_http_cleanup_refs(L, opts);
    return luaL_error(L, "%s", parse_error.c_str());
  }

  // Inside a coroutine (e.g. sys.taskInit) the request runs on a worker and this coroutine
  // is resumed with the result. Progress callbacks need the Lua state during the transfer,
  // so those requests keep the blocking path.
  if (lua_isyieldable(L) && opts.callback_ref == LUA_NOREF && opts.userdata_ref == LUA_NOREF) {
    int started = lua_http_start_async(L, req);
    if (started == 0) {
      lua_settop(L, 0);
      return lua_yieldk(L, 0, 0, lua_http_yield_cont);
    }
    return started;
  }

  HttpResult result;
  auto should_abort = [L]() {
    esphome::lua_runtime::lua_abort_if_ota(L);
    return false;
  };
  auto progress = [L, &opts](size_t content_length, size_t body_length) {
    std::string callback_error;
    bool ok = lua_http_call_progress(L, opts, content_length, body_length, callback_error);
    if (!ok) {
      ESP_LOGE(TAG, "callback error: %s", callback_error.c_str());
    }
    return ok;
  };

  lua_http_perform(req, result, should_abort, progress);
  lua_http_cleanup_refs(L, opts);

  if (!result.ok) {
    return lua_http_push_error(L, result.code);
  }

  lua_pushinteger(L, result.code);
  lua_http_push_headers(L, result.headers);
  if (result.download) {
    lua_pushinteger(L, static_cast<lua_Integer>(result.body_len));
  } else {
    lua_pushlstring(L, result.body.data(), result.body.size());
  }
  return 3;
}

static void lua_http_worker(void *arg) {
  auto *holder = static_cast<std::shared_ptr<HttpJob> *>(arg);
  std::shared_ptr<HttpJob> job = *holder;
  delete holder;

  HttpResult result;
  auto should_abort = [&job]() { return job->cancelled.load(std::memory_order_acquire); };
  lua_http_perform(job->req, result, should_abort, nullptr);
  job->result = std::move(result);

  {
    ensure_jobs_lock();
    if (g_jobs_lock != nullptr) {
      xSemaphoreTake(g_jobs_lock, portMAX_DELAY);
      job->done = true;
      xSemaphoreGive(g_jobs_lock);
    }
  }

  esphome::lua_runtime::lua_http_notify(job->owner);
  vTaskDelete(nullptr);
}

static const luaL_Reg HTTP_FUNCS[] = {
    {"request", lua_http_request},
    {"request_async", lua_http_request},
    {nullptr, nullptr},
};

}  // namespace

namespace esphome {
namespace lua_runtime {

// Called from the Lua task (rtos.receive). Takes one finished job for this script and
// resumes the coroutine that was parked in http.request with (code, headers, body).
void lua_http_dispatch(lua_State *L, const std::string &owner) {
  if (g_jobs_lock == nullptr) return;

  while (true) {
    std::shared_ptr<HttpJob> job;
    {
      xSemaphoreTake(g_jobs_lock, portMAX_DELAY);
      for (auto it = g_jobs.begin(); it != g_jobs.end(); ++it) {
        if (it->second->owner == owner && it->second->done && !it->second->dispatched) {
          job = it->second;
          job->dispatched = true;
          g_jobs.erase(it);
          break;
        }
      }
      xSemaphoreGive(g_jobs_lock);
    }
    if (!job) return;

    if (job->co_ref == LUA_NOREF) continue;

    lua_rawgeti(L, LUA_REGISTRYINDEX, job->co_ref);
    lua_State *co = lua_tothread(L, -1);
    lua_pop(L, 1);
    if (co == nullptr) {
      luaL_unref(L, LUA_REGISTRYINDEX, job->co_ref);
      continue;
    }

    if (!job->result.ok) {
      lua_pushinteger(co, job->result.code);
      lua_pushnil(co);
      lua_pushnil(co);
    } else {
      lua_pushinteger(co, job->result.code);
      lua_http_push_headers(co, job->result.headers);
      if (job->result.download) {
        lua_pushinteger(co, static_cast<lua_Integer>(job->result.body_len));
      } else {
        lua_pushlstring(co, job->result.body.data(), job->result.body.size());
      }
    }

    int nres = 0;
    int status = lua_resume(co, L, 3, &nres);
    if (status == LUA_OK) {
      lua_settop(co, 0);
    } else if (status != LUA_YIELD) {
      const char *err = lua_tostring(co, -1);
      ESP_LOGE(TAG, "async HTTP coroutine error: %s", err != nullptr ? err : "(unknown)");
      lua_settop(co, 0);
    }
    // On LUA_YIELD the coroutine is kept alive either by the new HTTP job it parked on or
    // by sys.wait/sys.waitUntil's own reference; on completion/error it is dead.
    luaL_unref(L, LUA_REGISTRYINDEX, job->co_ref);
  }
}

// Called before lua_close: drop every in-flight/completed request of a script and release
// its coroutine refs. Workers keep their own shared_ptr and abort at their next check.
void lua_http_cancel_owner(lua_State *L, const std::string &owner) {
  if (g_jobs_lock == nullptr) return;

  xSemaphoreTake(g_jobs_lock, portMAX_DELAY);
  for (auto it = g_jobs.begin(); it != g_jobs.end();) {
    if (it->second->owner == owner) {
      it->second->cancelled.store(true, std::memory_order_release);
      if (it->second->co_ref != LUA_NOREF) {
        luaL_unref(L, LUA_REGISTRYINDEX, it->second->co_ref);
        it->second->co_ref = LUA_NOREF;
      }
      it = g_jobs.erase(it);
    } else {
      ++it;
    }
  }
  xSemaphoreGive(g_jobs_lock);
}

}  // namespace lua_runtime
}  // namespace esphome

extern "C" int luaopen_http(lua_State *L) {
  luaL_newlib(L, HTTP_FUNCS);
  return 1;
}

#endif  // LUA_RUNTIME_STUB
