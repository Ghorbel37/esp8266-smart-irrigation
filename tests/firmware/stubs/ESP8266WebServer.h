#pragma once
#include "Arduino.h"
#define HTTP_GET 0
#define HTTP_POST 1
// Records routes and fakes one request at a time: set `args`, call `request(path)`, read `lastBody`
class ESP8266WebServer {
 public:
  std::map<std::string, std::function<void()>> routes;
  std::map<std::string, int> methods;
  std::vector<std::pair<std::string, std::string>> args;
  std::string lastBody;
  bool keepAlive_ = true;  // what the sketch asked for its last answer
  ESP8266WebServer(int) {}
  void on(const char *path, int m, std::function<void()> f) { routes[path] = f; methods[path] = m; }
  void begin() {}
  void keepAlive(bool k) { keepAlive_ = k; }
  void handleClient() {}
  void send(int, const char *, const char *body) { lastBody = body; }
  void send_P(int, const char *, const char *body) { lastBody = body; }
  bool hasArg(const char *n) { for (auto &a : args) if (a.first == n) return true; return false; }
  String arg(const char *n) { for (auto &a : args) if (a.first == n) return String(a.second); return String(); }
  String arg(int i) { return String(args[i].second); }
  String argName(int i) { return String(args[i].first); }
  int args_count() { return (int)args.size(); }
  uint8_t args_() { return (uint8_t)args.size(); }
  void request(const std::string &path) { routes.at(path)(); }
};
// The sketch calls server.args() to count arguments
#define args() args_()
