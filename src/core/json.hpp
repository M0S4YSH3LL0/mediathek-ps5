// Mediathek - A small JSON reader for the answers of web services.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace hui::json
{

// One JSON value. Arrays keep their elements in `items`; objects keep values
// in `items` and the matching names in `keys`.
struct Value
{
    enum class Kind : unsigned char
    {
        null,
        boolean,
        number,
        string,
        array,
        object,
    };

    Kind kind = Kind::null;
    bool boolean = false;
    double number = 0.0;
    std::string string;
    std::vector<Value> items;
    std::vector<std::string> keys;

    // The member of an object, or null when this is no object or lacks it.
    const Value *get(std::string_view key) const;
    // The member as text; "" when it is missing or not a string.
    std::string text(std::string_view key) const;
};

// Parses a whole document. False on a syntax error, trailing text, or nesting
// deeper than 32 levels (an answer from the network is not trusted).
bool parse(std::string_view text, Value *out);

} // namespace hui::json
