// Mediathek - A small JSON reader for the answers of web services.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#include "core/json.hpp"

#include <cstdlib>

namespace hui::json
{

const Value *Value::get(std::string_view key) const
{
    if (kind != Kind::object)
        return nullptr;
    for (std::size_t i = 0; i < keys.size(); ++i)
        if (keys[i] == key)
            return &items[i];
    return nullptr;
}

std::string Value::text(std::string_view key) const
{
    const Value *member = get(key);
    return member != nullptr && member->kind == Kind::string ? member->string : std::string();
}

namespace
{

constexpr int kMaxDepth = 32;

class Reader
{
  public:
    explicit Reader(std::string_view text) : text_(text)
    {
    }

    bool document(Value *out)
    {
        skip_space();
        if (!value(out, 0))
            return false;
        skip_space();
        return at_ == text_.size();
    }

  private:
    void skip_space()
    {
        while (at_ < text_.size() && (text_[at_] == ' ' || text_[at_] == '\t' ||
                                      text_[at_] == '\n' || text_[at_] == '\r'))
            ++at_;
    }

    bool literal(std::string_view word)
    {
        if (text_.substr(at_, word.size()) != word)
            return false;
        at_ += word.size();
        return true;
    }

    bool value(Value *out, int depth)
    {
        if (depth > kMaxDepth || at_ >= text_.size())
            return false;
        const char c = text_[at_];
        if (c == '{')
            return object(out, depth);
        if (c == '[')
            return array(out, depth);
        if (c == '"')
        {
            out->kind = Value::Kind::string;
            return string(&out->string);
        }
        if (c == 't' || c == 'f')
        {
            out->kind = Value::Kind::boolean;
            out->boolean = c == 't';
            return literal(c == 't' ? "true" : "false");
        }
        if (c == 'n')
            return literal("null");
        return number(out);
    }

    bool number(Value *out)
    {
        const std::size_t begin = at_;
        while (at_ < text_.size() &&
               (text_[at_] == '-' || text_[at_] == '+' || text_[at_] == '.' || text_[at_] == 'e' ||
                text_[at_] == 'E' || (text_[at_] >= '0' && text_[at_] <= '9')))
            ++at_;
        if (at_ == begin)
            return false;
        const std::string digits(text_.substr(begin, at_ - begin));
        char *end = nullptr;
        out->number = std::strtod(digits.c_str(), &end);
        out->kind = Value::Kind::number;
        return end != nullptr && *end == '\0';
    }

    static void append_utf8(std::string &out, unsigned code)
    {
        if (code < 0x80)
        {
            out.push_back(static_cast<char>(code));
        }
        else if (code < 0x800)
        {
            out.push_back(static_cast<char>(0xC0 | (code >> 6)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        }
        else if (code < 0x10000)
        {
            out.push_back(static_cast<char>(0xE0 | (code >> 12)));
            out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        }
        else
        {
            out.push_back(static_cast<char>(0xF0 | (code >> 18)));
            out.push_back(static_cast<char>(0x80 | ((code >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        }
    }

    bool hex4(unsigned *code)
    {
        if (at_ + 4 > text_.size())
            return false;
        unsigned value = 0;
        for (int i = 0; i < 4; ++i)
        {
            const char c = text_[at_ + static_cast<std::size_t>(i)];
            value <<= 4;
            if (c >= '0' && c <= '9')
                value |= static_cast<unsigned>(c - '0');
            else if (c >= 'a' && c <= 'f')
                value |= static_cast<unsigned>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F')
                value |= static_cast<unsigned>(c - 'A' + 10);
            else
                return false;
        }
        at_ += 4;
        *code = value;
        return true;
    }

    bool string(std::string *out)
    {
        ++at_; // the opening quote
        out->clear();
        while (at_ < text_.size())
        {
            const char c = text_[at_++];
            if (c == '"')
                return true;
            if (static_cast<unsigned char>(c) < 0x20)
                return false;
            if (c != '\\')
            {
                out->push_back(c);
                continue;
            }
            if (at_ >= text_.size())
                return false;
            const char escape = text_[at_++];
            switch (escape)
            {
            case '"':
            case '\\':
            case '/':
                out->push_back(escape);
                break;
            case 'b':
                out->push_back('\b');
                break;
            case 'f':
                out->push_back('\f');
                break;
            case 'n':
                out->push_back('\n');
                break;
            case 'r':
                out->push_back('\r');
                break;
            case 't':
                out->push_back('\t');
                break;
            case 'u':
            {
                unsigned code = 0;
                if (!hex4(&code))
                    return false;
                // A high surrogate pairs with the low one that follows it.
                if (code >= 0xD800 && code < 0xDC00)
                {
                    unsigned low = 0;
                    if (text_.substr(at_, 2) != "\\u")
                        return false;
                    at_ += 2;
                    if (!hex4(&low) || low < 0xDC00 || low > 0xDFFF)
                        return false;
                    code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
                }
                append_utf8(*out, code);
                break;
            }
            default:
                return false;
            }
        }
        return false;
    }

    bool array(Value *out, int depth)
    {
        ++at_;
        out->kind = Value::Kind::array;
        skip_space();
        if (at_ < text_.size() && text_[at_] == ']')
        {
            ++at_;
            return true;
        }
        for (;;)
        {
            skip_space();
            Value item;
            if (!value(&item, depth + 1))
                return false;
            out->items.push_back(std::move(item));
            skip_space();
            if (at_ >= text_.size())
                return false;
            const char c = text_[at_++];
            if (c == ']')
                return true;
            if (c != ',')
                return false;
        }
    }

    bool object(Value *out, int depth)
    {
        ++at_;
        out->kind = Value::Kind::object;
        skip_space();
        if (at_ < text_.size() && text_[at_] == '}')
        {
            ++at_;
            return true;
        }
        for (;;)
        {
            skip_space();
            if (at_ >= text_.size() || text_[at_] != '"')
                return false;
            std::string key;
            if (!string(&key))
                return false;
            skip_space();
            if (at_ >= text_.size() || text_[at_++] != ':')
                return false;
            skip_space();
            Value item;
            if (!value(&item, depth + 1))
                return false;
            out->keys.push_back(std::move(key));
            out->items.push_back(std::move(item));
            skip_space();
            if (at_ >= text_.size())
                return false;
            const char c = text_[at_++];
            if (c == '}')
                return true;
            if (c != ',')
                return false;
        }
    }

    std::string_view text_;
    std::size_t at_ = 0;
};

} // namespace

bool parse(std::string_view text, Value *out)
{
    *out = Value();
    return Reader(text).document(out);
}

} // namespace hui::json
