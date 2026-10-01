// Stand-in for ESPAsyncWebServer. Records routes and their methods; a test calls a route with
// server.request(path, args) and reads the answer in server.lastBody.
#pragma once
#include "Arduino.h"
enum WebRequestMethod { HTTP_GET = 1u << 1, HTTP_POST = 1u << 3 };

class AsyncWebServerRequest {
 public:
  std::vector<std::pair<std::string, std::string>> params;  // query string or form fields
  std::string body;
  bool hasArg(const char *n) const { for (auto &p : params) if (p.first == n) return true; return false; }
  String arg(const char *n) const { for (auto &p : params) if (p.first == n) return String(p.second); return String(); }
  String arg(int i) const { return String(params[i].second); }
  String argName(int i) const { return String(params[i].first); }
  size_t args() const { return params.size(); }
  void send(int, const char *, const char *content) { body = content; }
  void send(int, const char *, const uint8_t *content, size_t len) { body.assign((const char *)content, len); }
};

using ArRequestHandlerFunction = std::function<void(AsyncWebServerRequest *)>;

class AsyncWebServer {
 public:
  std::map<std::string, ArRequestHandlerFunction> routes;
  std::map<std::string, int> methods;
  std::string lastBody;
  AsyncWebServer(int) {}
  void on(const char *path, int method, ArRequestHandlerFunction f) { routes[path] = f; methods[path] = method; }
  void begin() {}
  void request(const std::string &path, std::vector<std::pair<std::string, std::string>> args = {}) {
    AsyncWebServerRequest r;
    r.params = args;
    routes.at(path)(&r);
    lastBody = r.body;
  }
};
