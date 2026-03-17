#pragma once

#include <string>
#include <future>
#include "nlohmann/json.hpp"

// Command sent from TCP client to the bridge
struct McpCommand
{
    int                             Id = 0;
    std::string                     Cmd;
    nlohmann::json                  Params;     // Full JSON message (includes "ref", "depth", etc.)
    std::promise<nlohmann::json>    Promise;
};
