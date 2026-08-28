#ifndef SERVER_TOOLS_HPP
#define SERVER_TOOLS_HPP

#include <cstdint>

#include <fstream>
#include <iostream>
#include <string>
#include <string_view>

#include <openssl/ssl.h>
#include <sys/socket.h>
#include <unistd.h>

#define SERVER_PATH "../src/" // absolute path or relative path to server.cpp

/*
 * Close connection and free relevant objects.
 */
inline void server_close_conn(SSL *conn_ssl, std::int32_t conn_sfd) {
  SSL_shutdown(conn_ssl);
  SSL_free(conn_ssl);
  shutdown(conn_sfd, SHUT_RDWR);
  close(conn_sfd);
}

/*
 * Read characters from file into body
 */
inline bool server_read_file(const char *file, std::string &body) {
  std::ifstream html_stream(file, std::ios::binary);
  if (!html_stream) {
    std::cerr << "Couldn't open " << file << "\n";
    return 1;
  }
  html_stream.seekg(0, std::ios::end);
  std::streamoff html_size = html_stream.tellg(); // Get size of html file
  body.resize(static_cast<std::size_t>(html_size), '\0');
  html_stream.seekg(0, std::ios::beg);      // Rewind before reading
  html_stream.read(body.data(), html_size); // body
  return 0;
}

/*
 * Construct and send response buffer
 */
inline bool server_send_response(const char *status, const char *content_type,
                                 std::string &body, SSL *conn_ssl) {
  // Construct response
  std::string response = "HTTP/1.1 " + std::string(status) +
                         "\r\n"
                         "Content-Type: " +
                         std::string(content_type) +
                         "; charset=utf-8\r\n"
                         "Content-Length: " +
                         std::to_string(body.size()) +
                         "\r\n"
                         // Revalidate every load. Without this the browser
                         // heuristically caches script.js and quietly runs
                         // stale UI against a current server.
                         "Cache-Control: no-cache\r\n"
                         "Connection: close\r\n"
                         "\r\n"; // header
  response.reserve(response.size() + body.size());
  response += body; // Append body to header

  // Send response buffer
  if (SSL_write(conn_ssl, response.c_str(),
                static_cast<std::int32_t>(response.size())) <= 0) {
    std::cout << "Send failed\n";
    return 1;
  }

  return 0;
}

/*
 * Content type from the path's extension, for the handful the site serves
 */
inline const char *server_mime(std::string_view path) {
  if (path.ends_with(".html"))
    return "text/html";
  if (path.ends_with(".css"))
    return "text/css";
  if (path.ends_with(".js"))
    return "text/javascript";
  if (path.ends_with(".json"))
    return "application/json";
  if (path.ends_with(".svg"))
    return "image/svg+xml";
  return "application/octet-stream";
}

/* Read one value out of a query string, "a=1&db=github,devpost" style.
 *
 * -> Returns an empty view when the parameter is absent. Points into query, so
 *    it lives exactly as long as the request buffer does.
 */
inline std::string_view server_query_value(std::string_view query,
                                           std::string_view name) {
  for (std::size_t at = 0; at < query.size();) {
    std::size_t end = query.find('&', at);
    std::string_view pair =
        query.substr(at, end == std::string_view::npos ? end : end - at);

    std::size_t eq = pair.find('=');
    if (eq != std::string_view::npos && pair.substr(0, eq) == name) {
      return pair.substr(eq + 1);
    }

    if (end == std::string_view::npos) {
      break;
    }
    at = end + 1;
  }
  return {};
}

/* Read one number out of a query string, "a=1&limit=200" style.
 *
 *  - query is the part after the '?', without it
 *
 * -> Returns fallback when the parameter is absent or not a number.
 */
inline long server_query_long(std::string_view query, std::string_view name,
                              long fallback) {
  for (std::size_t at = 0; at < query.size();) {
    std::size_t end = query.find('&', at);
    std::string_view pair =
        query.substr(at, end == std::string_view::npos ? end : end - at);

    std::size_t eq = pair.find('=');
    if (eq != std::string_view::npos && pair.substr(0, eq) == name) {
      // strtol needs a terminator, and a view of a request buffer has none
      std::string value(pair.substr(eq + 1));
      char *stop = nullptr;
      long parsed = std::strtol(value.c_str(), &stop, 10);
      return stop != value.c_str() ? parsed : fallback;
    }

    if (end == std::string_view::npos) {
      break;
    }
    at = end + 1;
  }
  return fallback;
}

/* Serve one file out of src/site.
 *
 *  - "/" resolves to index.html, a query string is not part of the path
 *  - a traversal, an embedded NUL or a path that is not rooted answers 400
 *  - anything that resolves but does not exist answers 404 with site/404.html
 *
 * NOTE: The rejection is a substring test on the endpoint as it arrived, so no
 * traversal ever reaches the filesystem. HTTP1Header does no percent decoding,
 * which is the only reason "%2e%2e" cannot slip past this. Adding decoding
 * later means revisiting it here.
 */
inline bool server_send_file(std::string_view endpoint, SSL *conn_ssl) {
  std::string empty;

  if (endpoint.empty() || endpoint.front() != '/' ||
      endpoint.find("..") != std::string_view::npos ||
      endpoint.find('\0') != std::string_view::npos) {
    std::cerr << "Rejected endpoint: " << endpoint << "\n";
    return server_send_response("400 Bad Request", "text/plain", empty,
                                conn_ssl);
  }

  std::string_view path = endpoint.substr(0, endpoint.find('?'));

  // Resolved before the type is read off it, otherwise "/" has no extension to
  // go on and index.html goes out as a binary download.
  std::string rel(path == "/" ? std::string_view("/index.html") : path);
  std::string file(REL_PATH_FROM_BUILD "site");
  file.append(rel);

  std::string body;
  if (server_read_file(file.c_str(), body)) {
    if (server_read_file(REL_PATH_FROM_BUILD "site/404.html", body)) {
      return 1;
    }
    return server_send_response("404 Not Found", "text/html", body, conn_ssl);
  }

  return server_send_response("200 OK", server_mime(rel), body, conn_ssl);
}

/* Collect a request body of length bytes.
 *
 *  - raw/read_len are the first read off the socket, which already holds the
 *    head of the body whenever it fit in the same segment
 *
 * NOTE: The accept loop reads once into a fixed buffer, so without this a body
 * longer than what shared that read is silently truncated.
 *
 * -> Returns non zero when the head of the body cannot be found or the socket
 *    closes before length bytes arrive.
 */
inline bool server_read_body(SSL *conn_ssl, const char *raw,
                             std::size_t read_len, long length,
                             std::string &body) {
  if (length <= 0) {
    body.clear();
    return 0;
  }

  std::string_view head(raw, read_len);
  std::size_t start = head.find("\r\n\r\n");
  if (start == std::string_view::npos) {
    std::cerr << "Request has no header terminator\n";
    return 1;
  }
  start += 4;

  body.assign(head.substr(start));
  body.reserve(static_cast<std::size_t>(length));

  char chunk[4096];
  while (body.size() < static_cast<std::size_t>(length)) {
    std::int32_t n = SSL_read(conn_ssl, chunk, sizeof(chunk));
    if (n <= 0) {
      std::cerr << "Body read stopped at " << body.size() << " of " << length
                << " bytes\n";
      return 1;
    }
    body.append(chunk, static_cast<std::size_t>(n));
  }

  return 0;
}

#endif
