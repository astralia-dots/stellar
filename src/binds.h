#pragma once

#include <cstdint>
#include <expected>
#include <string>
#include <vector>

struct Keybind {
    uint16_t modmask = 0;
    std::string key;
    std::string dispatcher;
    std::string arg;
    std::string submap;
};

using BindResult = std::expected<std::vector<Keybind>, std::string>;

/// Parses `hyprctl binds -j` output. Missing or mistyped fields default to empty/zero.
BindResult parse_binds(const std::string& json);

/// Runs `hyprctl binds -j`. Shelling out keeps us in step with Hyprland's own IPC.
BindResult fetch();

/// One line per bind, `keys : action`, with the `:` aligned. Needs a monospace font.
std::vector<std::string> format_binds(const std::vector<Keybind>& binds);
