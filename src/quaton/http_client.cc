#include "quaton/http_client.h"

#include <zstd.h>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <stdexcept>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#endif

namespace Quaton {

static std::once_flag curl_init_flag;

struct CurlGlobalGuard {
  ~CurlGlobalGuard() {
    curl_global_cleanup();
#ifdef _WIN32
    WSACleanup();
#endif
  }
} static curl_guard;

namespace {
void InitCurl() {
  std::call_once(curl_init_flag, []() {
#ifdef _WIN32
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
      throw std::runtime_error("Failed to initialize Winsock");
    }
#endif

    CURLcode res = curl_global_init(CURL_GLOBAL_ALL);
    if (res != CURLE_OK) {
      throw std::runtime_error("Failed to initialize CURL globally: " +
                               std::string(curl_easy_strerror(res)));
    }
  });
}
}  // namespace

HttpClient::HttpClient(const HttpConfig& config)
    : curl_(nullptr),
      headers_(nullptr),
      timeout_ms_(config.timeout_ms),
      user_agent_(config.user_agent),
      max_connections_per_server_(config.max_connections_per_server),
      follow_redirects_(config.follow_redirects),
      ssl_verify_peer_(config.ssl_verify_peer),
      ssl_verify_host_(config.ssl_verify_host) {
  InitCurl();

  curl_ = curl_easy_init();
  if (!curl_) {
    throw std::runtime_error("Failed to initialize CURL easy handle");
  }

  curl_easy_setopt(curl_, CURLOPT_MAXCONNECTS, max_connections_per_server_);
  curl_easy_setopt(curl_, CURLOPT_FOLLOWLOCATION, follow_redirects_ ? 1L : 0L);
  curl_easy_setopt(curl_, CURLOPT_SSL_VERIFYPEER, ssl_verify_peer_ ? 1L : 0L);
  curl_easy_setopt(curl_, CURLOPT_SSL_VERIFYHOST, ssl_verify_host_ ? 2L : 0L);
  curl_easy_setopt(curl_, CURLOPT_USERAGENT, user_agent_.c_str());
  curl_easy_setopt(curl_, CURLOPT_TIMEOUT_MS, timeout_ms_);

  // Apply default headers from config
  if (!config.default_headers.empty()) {
    set_headers(config.default_headers);
  }
}

HttpClient::HttpClient(int max_connections_per_server)
    : curl_(nullptr),
      headers_(nullptr),
      timeout_ms_(30000),
      user_agent_("Quaton/1.0.0"),
      max_connections_per_server_(max_connections_per_server),
      follow_redirects_(true),
      ssl_verify_peer_(true),
      ssl_verify_host_(true) {
  InitCurl();

  curl_ = curl_easy_init();
  if (!curl_) {
    throw std::runtime_error("Failed to initialize CURL easy handle");
  }

  curl_easy_setopt(curl_, CURLOPT_MAXCONNECTS, max_connections_per_server_);
  curl_easy_setopt(curl_, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(curl_, CURLOPT_SSL_VERIFYPEER, 1L);
  curl_easy_setopt(curl_, CURLOPT_SSL_VERIFYHOST, 2L);
  curl_easy_setopt(curl_, CURLOPT_USERAGENT, user_agent_.c_str());
  curl_easy_setopt(curl_, CURLOPT_TIMEOUT_MS, timeout_ms_);
}

HttpClient::~HttpClient() {
  if (headers_) {
    curl_slist_free_all(headers_);
    headers_ = nullptr;
  }

  if (curl_) {
    curl_easy_cleanup(curl_);
    curl_ = nullptr;
  }
}

std::future<std::vector<uint8_t>> HttpClient::perform_async_request(
    const std::string& url, const std::function<void(CURL*)>& setup_options) {
  return std::async(std::launch::async, [this, url, setup_options]() {
    std::unique_lock<std::mutex> lock(config_mutex_);
    long timeout = timeout_ms_;
    std::string user_agent = user_agent_;
    int max_connections = max_connections_per_server_;
    struct CurlSlistDeleter {
      void operator()(curl_slist* list) const {
        if (list) curl_slist_free_all(list);
      }
    };
    std::unique_ptr<curl_slist, CurlSlistDeleter> headers_copy;
    try {
      headers_copy.reset(copy_headers());
    } catch (...) {
      throw;
    }
    lock.unlock();

    CURL* thread_curl = curl_easy_init();
    if (!thread_curl) {
      throw std::runtime_error(
          "Failed to initialize CURL handle for async request");
    }

    try {
      setup_curl_options(thread_curl,
                         timeout,
                         user_agent,
                         max_connections,
                         headers_copy.get());

      std::vector<uint8_t> responseData;
      curl_easy_setopt(thread_curl, CURLOPT_URL, url.c_str());
      curl_easy_setopt(thread_curl, CURLOPT_WRITEFUNCTION, write_callback);
      curl_easy_setopt(thread_curl, CURLOPT_WRITEDATA, &responseData);

      if (setup_options) {
        setup_options(thread_curl);
      }

      CURLcode res = curl_easy_perform(thread_curl);
      if (res != CURLE_OK) {
        throw std::runtime_error(std::string("CURL request failed: ") +
                                 curl_easy_strerror(res));
      }

      long responseCode;
      curl_easy_getinfo(thread_curl, CURLINFO_RESPONSE_CODE, &responseCode);
      if (responseCode < 200 || responseCode >= 300) {
        throw std::runtime_error("HTTP request failed with status code: " +
                                 std::to_string(responseCode));
      }

      curl_easy_cleanup(thread_curl);
      return responseData;
    } catch (...) {
      curl_easy_cleanup(thread_curl);
      throw;
    }
  });
}

std::future<std::shared_ptr<std::vector<uint8_t>>>
HttpClient::perform_async_stream_request(
    const std::string& url, const std::function<void(CURL*)>& setup_options) {
  return std::async(std::launch::async, [this, url, setup_options]() {
    std::unique_lock<std::mutex> lock(config_mutex_);
    long timeout = timeout_ms_;
    std::string user_agent = user_agent_;
    int max_connections = max_connections_per_server_;
    curl_slist* headers_copy = nullptr;
    try {
      headers_copy = copy_headers();
    } catch (...) {
      throw;
    }
    lock.unlock();

    CURL* thread_curl = curl_easy_init();
    if (!thread_curl) {
      if (headers_copy) curl_slist_free_all(headers_copy);
      throw std::runtime_error(
          "Failed to initialize CURL handle for async stream request");
    }

    try {
      setup_curl_options(
          thread_curl, timeout, user_agent, max_connections, headers_copy);

      auto result = std::make_shared<std::vector<uint8_t>>();
      curl_easy_setopt(thread_curl, CURLOPT_URL, url.c_str());
      curl_easy_setopt(thread_curl, CURLOPT_WRITEFUNCTION, write_callback);
      curl_easy_setopt(thread_curl, CURLOPT_WRITEDATA, result.get());

      if (setup_options) {
        setup_options(thread_curl);
      }

      CURLcode res = curl_easy_perform(thread_curl);
      if (res != CURLE_OK) {
        throw std::runtime_error(std::string("CURL stream request failed: ") +
                                 curl_easy_strerror(res));
      }

      long responseCode;
      curl_easy_getinfo(thread_curl, CURLINFO_RESPONSE_CODE, &responseCode);
      if (responseCode < 200 || responseCode >= 300) {
        throw std::runtime_error(
            "HTTP stream request failed with status code: " +
            std::to_string(responseCode));
      }

      if (headers_copy) curl_slist_free_all(headers_copy);
      curl_easy_cleanup(thread_curl);
      return result;
    } catch (...) {
      if (headers_copy) curl_slist_free_all(headers_copy);
      curl_easy_cleanup(thread_curl);
      throw;
    }
  });
}

std::future<std::vector<uint8_t>> HttpClient::get_async(
    const std::string& url, const std::string& completion_option) {
  if (url.empty()) {
    throw std::invalid_argument("URL cannot be empty");
  }
  return perform_async_request(url, nullptr);
}

std::future<std::vector<uint8_t>> HttpClient::post_async(
    const std::string& url,
    const std::string& post_data,
    const std::string& completion_option) {
  if (url.empty()) {
    throw std::invalid_argument("URL cannot be empty");
  }
  return perform_async_request(url, [post_data](CURL* handle) {
    curl_easy_setopt(handle, CURLOPT_POST, 1L);
    curl_easy_setopt(handle, CURLOPT_POSTFIELDS, post_data.c_str());
    curl_easy_setopt(handle, CURLOPT_POSTFIELDSIZE, post_data.length());
  });
}

std::future<std::shared_ptr<std::vector<uint8_t>>> HttpClient::get_stream_async(
    const std::string& url, const std::string& completion_option) {
  if (url.empty()) {
    throw std::invalid_argument("URL cannot be empty");
  }
  return perform_async_stream_request(url, nullptr);
}

void HttpClient::set_headers(const std::vector<std::string>& headers) {
  std::unique_lock<std::mutex> lock(config_mutex_);

  if (headers.empty()) {
    if (headers_) {
      curl_slist_free_all(headers_);
      headers_ = nullptr;
    }
    return;
  }

  curl_slist* tempHeaders = nullptr;
  try {
    for (const auto& header : headers) {
      curl_slist* new_temp = curl_slist_append(tempHeaders, header.c_str());
      if (!new_temp) {
        throw std::runtime_error("Failed to append header: " + header);
      }
      tempHeaders = new_temp;
    }
  } catch (...) {
    if (tempHeaders) curl_slist_free_all(tempHeaders);
    throw;
  }

  if (headers_) {
    curl_slist_free_all(headers_);
  }
  headers_ = tempHeaders;

  if (headers_) {
    curl_easy_setopt(curl_, CURLOPT_HTTPHEADER, headers_);
  }
}

void HttpClient::set_timeout(long timeout_ms) {
  if (timeout_ms <= 0) {
    throw std::invalid_argument("Timeout must be greater than 0");
  }
  std::unique_lock<std::mutex> lock(config_mutex_);
  timeout_ms_ = timeout_ms;
  curl_easy_setopt(curl_, CURLOPT_TIMEOUT_MS, timeout_ms_);
}

void HttpClient::set_user_agent(const std::string& user_agent) {
  if (user_agent.empty()) {
    throw std::invalid_argument("User agent cannot be empty");
  }
  std::unique_lock<std::mutex> lock(config_mutex_);
  user_agent_ = user_agent;
  curl_easy_setopt(curl_, CURLOPT_USERAGENT, user_agent_.c_str());
}

size_t HttpClient::write_callback(void* contents,
                                  size_t size,
                                  size_t nmemb,
                                  void* user_data) {
  size_t totalSize = size * nmemb;
  auto* buffer = static_cast<std::vector<uint8_t>*>(user_data);

  if (!buffer) {
    return 0;
  }

  size_t requiredCapacity = buffer->size() + totalSize;
  if (buffer->capacity() < requiredCapacity) {
    size_t newCapacity =
        std::max(buffer->capacity() * 2, requiredCapacity + 1024);
    buffer->reserve(newCapacity);
  }

  size_t old_size = buffer->size();
  buffer->resize(old_size + totalSize);
  std::memcpy(buffer->data() + old_size, contents, totalSize);

  return totalSize;
}

curl_slist* HttpClient::copy_headers() {
  curl_slist* copy = nullptr;
  curl_slist* temp = headers_;
  while (temp) {
    curl_slist* new_node = curl_slist_append(copy, temp->data);
    if (!new_node) {
      if (copy) curl_slist_free_all(copy);
      throw std::runtime_error("Failed to copy headers");
    }
    copy = new_node;
    temp = temp->next;
  }
  return copy;
}

void HttpClient::setup_curl_options(CURL* handle,
                                    long timeout,
                                    const std::string& user_agent,
                                    int max_connections,
                                    curl_slist* headers_copy) {
  curl_easy_setopt(handle, CURLOPT_MAXCONNECTS, max_connections);
  curl_easy_setopt(handle, CURLOPT_FOLLOWLOCATION, follow_redirects_ ? 1L : 0L);
  curl_easy_setopt(handle, CURLOPT_SSL_VERIFYPEER, ssl_verify_peer_ ? 1L : 0L);
  curl_easy_setopt(handle, CURLOPT_SSL_VERIFYHOST, ssl_verify_host_ ? 2L : 0L);
  curl_easy_setopt(handle, CURLOPT_USERAGENT, user_agent.c_str());
  curl_easy_setopt(handle, CURLOPT_TIMEOUT_MS, timeout);
  if (headers_copy) {
    curl_easy_setopt(handle, CURLOPT_HTTPHEADER, headers_copy);
  }
}

}  // namespace Quaton