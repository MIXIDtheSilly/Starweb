#pragma once

#include "config.hpp"

#include <string>

namespace proxy {

std::string normalize_host(const std::string& host);

const Server* pick_server(const Config& cfg, uint16_t port, const std::string& host);

const Location* pick_location(const Server& srv, const std::string& path);

} // namespace proxy
