#include "net/io/HttpSession.hpp"

#include <boost/asio/write.hpp>
#include <boost/beast/core/tcp_stream.hpp>
#include <boost/beast/version.hpp>
#include <boost/beast/websocket/rfc6455.hpp>
#include <print>

#include "net/io/WebSocketSession.hpp"

namespace Exchange::Net {
namespace {
std::string_view mimeType(std::string_view path) {
  const auto dot{path.rfind('.')};
  const std::string_view extension{
      dot == std::string_view::npos ? std::string_view{} : path.substr(dot)};
  if (extension == ".html") return "text/html; charset=utf-8";
  if (extension == ".js") return "application/javascript; charset=utf-8";
  if (extension == ".css") return "text/css; charset=utf-8";
  if (extension == ".json") return "application/json";
  if (extension == ".svg") return "image/svg+xml";
  if (extension == ".ico") return "image/x-icon";
  if (extension == ".png") return "image/png";
  return "application/octet-stream";
}
}  // namespace

std::string pathCat(std::string_view root, std::string_view target,
                    bool& rejected) {
  rejected = false;

  // Strip a query string before anything looks at the path.
  const auto query{target.find('?')};
  if (query != std::string_view::npos) target = target.substr(0, query);

  if (target.empty() || target.front() != '/') {
    rejected = true;
    return {};
  }
  // No canonicalization, on purpose: an implementation that normalizes has to
  // be right about symlinks, percent-encoding and separators, and one that
  // refuses only has to be right about "..". This is the only place an
  // untrusted string reaches the filesystem, so it gets the boring answer.
  if (target.find("..") != std::string_view::npos) {
    rejected = true;
    return {};
  }
  if (target.find('\0') != std::string_view::npos) {
    rejected = true;
    return {};
  }

  std::string path{root};
  if (!path.empty() && path.back() == '/') path.pop_back();
  path.append(target);
  if (target == "/") path.append("index.html");
  return path;
}

HttpSession::HttpSession(tcp::socket socket, SessionContext context,
                         std::string web_root)
    : m_socket(std::move(socket)),
      m_context(context),
      m_web_root(std::move(web_root)) {}

void HttpSession::start() { doRead(); }

void HttpSession::doRead() {
  m_request = {};
  auto self{shared_from_this()};
  http::async_read(
      m_socket, m_buffer, m_request,
      [this, self](const boost::system::error_code& ec, std::size_t) {
        // end_of_stream is the ordinary keep-alive timeout; anything else
        // is a real error. Both end the connection, so neither is special.
        if (ec) {
          doClose();
          return;
        }
        handleRequest();
      });
}

void HttpSession::handleRequest() {
  /*
  One listener, two protocols. The upgrade check has to happen here rather
  than at accept time, because whether a connection is a WebSocket is a
  property of its first request, not of its port.
  */
  if (boost::beast::websocket::is_upgrade(m_request)) {
    const uint32_t session_id{m_context.io.nextSessionId()};
    auto session{std::make_shared<WebSocketSession>(std::move(m_socket),
                                                    m_context, session_id)};
    session->run(std::move(m_request));
    return;
  }

  auto respond{[&](http::status status, std::string_view body,
                   std::string_view content_type) {
    auto response{std::make_shared<http::response<http::string_body>>(
        status, m_request.version())};
    response->set(http::field::server, "exchange");
    response->set(http::field::content_type, content_type);
    response->keep_alive(m_request.keep_alive());
    response->body() = std::string{body};
    response->prepare_payload();
    m_response = response;

    auto self{shared_from_this()};
    http::async_write(m_socket, *response,
                      [this, self, response](
                          const boost::system::error_code& ec, std::size_t) {
                        if (ec || response->need_eof()) {
                          doClose();
                          return;
                        }
                        doRead();
                      });
  }};

  if (m_request.method() != http::verb::get &&
      m_request.method() != http::verb::head) {
    respond(http::status::method_not_allowed, "method not allowed",
            "text/plain");
    return;
  }

  bool rejected{false};
  const std::string path{pathCat(m_web_root, m_request.target(), rejected)};
  if (rejected) {
    // Deliberately the same answer as a missing file: a traversal attempt
    // learns nothing about what exists.
    respond(http::status::not_found, "not found", "text/plain");
    return;
  }

  boost::system::error_code ec{};
  http::file_body::value_type file{};
  file.open(path.c_str(), boost::beast::file_mode::scan, ec);
  if (ec) {
    respond(http::status::not_found, "not found", "text/plain");
    return;
  }

  const auto size{file.size()};
  auto response{std::make_shared<http::response<http::file_body>>(
      std::piecewise_construct, std::make_tuple(std::move(file)),
      std::make_tuple(http::status::ok, m_request.version()))};
  response->set(http::field::server, "exchange");
  response->set(http::field::content_type, mimeType(path));
  response->content_length(size);
  response->keep_alive(m_request.keep_alive());
  m_response = response;

  auto self{shared_from_this()};
  http::async_write(
      m_socket, *response,
      [this, self, response](const boost::system::error_code& e, std::size_t) {
        if (e || response->need_eof()) {
          doClose();
          return;
        }
        doRead();
      });
}

void HttpSession::doClose() {
  boost::system::error_code ec{};
  m_socket.shutdown(tcp::socket::shutdown_send, ec);
  m_socket.close(ec);
}
}  // namespace Exchange::Net
