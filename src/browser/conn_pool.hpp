#pragma once
#include "../common/conn.hpp"
#include <memory>
#include <string>

namespace connpool {

// key is "scheme://host:port". A pooled connection can already be dead, so the
// caller must retry once on a fresh one.
std::unique_ptr<Conn> acquire(const std::string& key);

// Only for a response read to its exact declared length, without Connection: close.
void release(const std::string& key, std::unique_ptr<Conn> conn);

void clear();

} // namespace connpool
