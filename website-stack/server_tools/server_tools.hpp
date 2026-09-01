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
  std::size_t html_size = html_stream.tellg(); // Get size of html file
  body.resize(html_size, '\0');
  html_stream.seekg(0, std::ios::beg);      // Rewind before reading
  html_stream.read(body.data(), html_size); // body
  return 0;
}

/*
 * Construct and send response buffer
 */
inline bool server_send_response(std::string_view status,
                                 std::string_view content_type,
                                 std::string_view body, SSL *conn_ssl) {
  // Construct response
  std::string response = "HTTP/1.1 " + std::string(status) +
                         "\r\n"
                         "Content-Type: " +
                         std::string(content_type) +
                         "; charset=utf-8\r\n"
                         "Content-Length: " +
                         std::to_string(body.size()) +
                         "\r\n"
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
 * Serves the file in the endpoint relative to the path in rel
 */
inline bool server_send_file(std::string_view rel, std::string_view endpoint,
                             SSL *conn_ssl) {

  std::string path(rel); // Construct absolute file path
  if (endpoint.empty()) {
    path.append("index.html");
  } else if (endpoint.find("..") != std::string_view::npos ||
             endpoint.find('\0') != std::string_view::npos ||
             endpoint.find('?') != std::string_view::npos) {
    // Check if the endpoint is valid
    std::cerr << "Rejected endpoint: " << endpoint << "\n";
    return server_send_response("400 Bad Request", "text/plain", "", conn_ssl);
  } else {
    path.append(endpoint);
  }

  // Read and send path
  std::string body;
  if (server_read_file(path.c_str(), body)) { // If reading file fails
    std::string path404(rel);                 // read and send 404.html
    path404.append("404.html");
    server_read_file(path404.c_str(), body);
    return server_send_response("404 Not Found", "text/html", body, conn_ssl);
  }

  return server_send_response("200 OK", server_mime(path), body, conn_ssl);
}

#endif
