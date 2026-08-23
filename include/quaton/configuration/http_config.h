#ifndef QUATON_CONFIGURATION_HTTP_CONFIG_H
#define QUATON_CONFIGURATION_HTTP_CONFIG_H
#pragma once

#include <string>
#include <vector>

namespace Quaton {

/**
 * @struct HttpConfig
 * @brief HTTP client configuration structure
 */
struct HttpConfig {
  long timeout_ms = 30000;                   ///< Timeout in milliseconds
  std::string user_agent = "Quaton/1.0.0";   ///< User agent string
  int max_connections_per_server = 128;      ///< Max connections per server
  std::vector<std::string> default_headers;  ///< Default request headers
  bool follow_redirects = true;              ///< Follow HTTP redirects
  bool ssl_verify_peer = true;               ///< Verify SSL peer certificate
  bool ssl_verify_host = true;               ///< Verify SSL host

  /**
   * @brief Create default HTTP configuration
   * @return Default HTTP configuration
   */
  static HttpConfig create_default() { return HttpConfig{}; }

  /**
   * @brief Create HTTP configuration with custom timeout
   * @param timeout_ms Timeout in milliseconds
   * @return HTTP configuration with custom timeout
   */
  static HttpConfig with_timeout(long timeout_ms) {
    HttpConfig config;
    config.timeout_ms = timeout_ms;
    return config;
  }

  /**
   * @brief Create HTTP configuration with custom user agent
   * @param user_agent User agent string
   * @return HTTP configuration with custom user agent
   */
  static HttpConfig with_user_agent(const std::string& user_agent) {
    HttpConfig config;
    config.user_agent = user_agent;
    return config;
  }
};

}  // namespace Quaton

#endif  // QUATON_CONFIGURATION_HTTP_CONFIG_H
