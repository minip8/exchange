#pragma once

#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/http.hpp>
#include <memory>
#include <string>

#include "net/io/IoThread.hpp"
#include "net/io/TcpSession.hpp"

namespace Exchange::Net {
namespace beast = boost::beast;
namespace http = beast::http;

/*
One HTTP connection on the public port.

There is a single listener on 8080, not two: the first request decides. If it
is a WebSocket upgrade the connection becomes a WebSocketSession and this
object hands over its socket; otherwise it is a static file request for the
GUI and the connection stays HTTP.

This is the only place in the server where an untrusted string reaches the
filesystem, which is why pathCat is where it is and says what it says.
*/
class HttpSession : public std::enable_shared_from_this<HttpSession> {
 public:
  HttpSession(tcp::socket socket, SessionContext context, std::string web_root);

  void start();

 private:
  void doRead();
  void handleRequest();
  template <typename Body>
  void send(http::response<Body>&& response);
  void doClose();

  tcp::socket m_socket;
  SessionContext m_context;
  std::string m_web_root;
  beast::flat_buffer m_buffer{};
  http::request<http::string_body> m_request{};
  // The response outlives the async_write that sends it, so it cannot be a
  // local. A shared_ptr because the two body types are different responses.
  std::shared_ptr<void> m_response{};
};

/*
Joins a URL path onto the web root, refusing anything that could escape it.

Refuses absolute paths and any ".." segment outright rather than trying to
canonicalize: a normalizing implementation has to be right about symlinks,
encodings and platform separators, and a rejecting one only has to be right
about two things.
*/
std::string pathCat(std::string_view root, std::string_view target,
                    bool& rejected);
}  // namespace Exchange::Net
