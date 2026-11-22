// Rekadeck desktop simulator: runs the real device core (firmware/lib/rekacore) against DesktopHal, and serves a
// page at http://127.0.0.1:8090 with the screen, the four keys and test controls.
//
//   ./rekadeck_sim [--port 8090] [--data ./sim_data] [--fast]
//
// --fast: partial refreshes and Wi-Fi joins take a tenth of their real time, for the automated tests in tests/sim.
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <thread>

#include "device.h"
#include "hal_desktop.h"
#include "portal_html.h"
#include "vendor/httplib.h"

#ifndef SIM_UI_PATH
#define SIM_UI_PATH "ui.html"
#endif

static void runDevice(DesktopHal& hal) {
  bool power_on = true;
  for (;;) {
    hal.beginBoot(power_on);
    {
      auto dev = std::make_unique<reka::Device>(hal);   // fresh RAM every boot, like the ESP32 after deep sleep
      dev->setup();
      while (!hal.resetRequested() && dev->loop()) hal.delayMs(10);
    }
    if (!hal.resetRequested() && hal.waitForWake()) {
      power_on = false;
      continue;
    }
    hal.performReset();
    power_on = true;
  }
}

int main(int argc, char** argv) {
  int port = 8090;
  std::string data = "sim_data";
  bool fast = false;
  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    if (a == "--fast") fast = true;
    else if (a == "--port" && i + 1 < argc) port = std::atoi(argv[++i]);
    else if (a == "--data" && i + 1 < argc) data = argv[++i];
  }
  DesktopHal hal(data, fast);
  std::thread device([&] { runDevice(hal); });
  device.detach();

  httplib::Server srv;
  srv.Get("/", [](const httplib::Request&, httplib::Response& res) {
    std::ifstream f(SIM_UI_PATH);
    std::stringstream ss;
    ss << f.rdbuf();
    res.set_content(f ? ss.str() : std::string("ui.html not found at " SIM_UI_PATH), "text/html");
  });
  srv.Get("/api/state", [&](const httplib::Request&, httplib::Response& res) {
    res.set_content(hal.stateJson(), "application/json");
  });
  srv.Get("/api/screen", [&](const httplib::Request& req, httplib::Response& res) {
    auto s = hal.screen(req.has_param("prev"));
    res.set_content(std::string(s.begin(), s.end()), "application/octet-stream");
  });
  srv.Get("/api/refreshes", [&](const httplib::Request& req, httplib::Response& res) {
    uint64_t after = req.has_param("after") ? std::strtoull(req.get_param_value("after").c_str(), nullptr, 10) : 0;
    res.set_content(hal.refreshesJson(after), "application/json");
  });
  srv.Get("/api/frame", [&](const httplib::Request& req, httplib::Response& res) {
    auto f = hal.frame(std::strtoull(req.get_param_value("seq").c_str(), nullptr, 10));
    res.set_content(std::string(f.begin(), f.end()), "application/octet-stream");
  });
  srv.Get("/api/log", [&](const httplib::Request& req, httplib::Response& res) {
    uint64_t after = req.has_param("after") ? std::strtoull(req.get_param_value("after").c_str(), nullptr, 10) : 0;
    res.set_content(hal.logJson(after), "application/json");
  });
  srv.Post("/api/key", [&](const httplib::Request& req, httplib::Response& res) {
    hal.keyEvent(std::atoi(req.get_param_value("k").c_str()), req.get_param_value("down") == "1");
    res.set_content("{}", "application/json");
  });
  srv.Post("/api/control", [&](const httplib::Request& req, httplib::Response& res) {
    hal.control(req.body);
    res.set_content(hal.stateJson(), "application/json");
  });
  // the setup portal, as your phone would see it after joining the Rekadeck's own network
  srv.Get("/portal", [&](const httplib::Request&, httplib::Response& res) {
    if (!hal.portalActive()) {
      res.set_content("<p style='font:16px system-ui;padding:24px'>The Rekadeck isn't in Wi-Fi setup right now. "
                      "Use Menu (hold keys 1 and 4) then Wi-Fi setup.</p>", "text/html");
      return;
    }
    res.set_content(hal.portalPage(), "text/html");
  });
  srv.Get("/scan", [&](const httplib::Request&, httplib::Response& res) {
    res.set_content(hal.scanJson(), "application/json");
  });
  srv.Post("/save", [&](const httplib::Request& req, httplib::Response& res) {
    reka::PortalSubmit s;
    s.ssid = req.get_param_value("ssid");
    s.pass = req.get_param_value("pass");
    s.server = req.get_param_value("server");
    hal.portalSubmit(s);
    res.set_content(reka::PORTAL_SAVED_HTML, "text/html");
  });

  std::cout << "\n  Rekadeck simulator: http://127.0.0.1:" << port << "\n"
            << "  Start the mock server too (see README) so it has something to sync with.\n" << std::endl;
  if (!srv.listen("127.0.0.1", port)) {
    std::cerr << "couldn't listen on port " << port << "\n";
    return 1;
  }
}
