#pragma once

#include <map>
#include <string>
#include <vector>

namespace shizuku
{

// Minimal JSON value (objects, arrays, strings, numbers, true/false/null), used for .svp files and note edits.
struct Json
{
    enum class Type { null, boolean, number, string, array, object } type = Type::null;
    double number = 0.0;
    std::string text; // UTF-8
    std::vector<Json> items;
    std::map<std::string, Json> members;

    const Json* get (const std::string& key) const
    {
        const auto it = members.find (key);
        return it == members.end() ? nullptr : &it->second;
    }
    double numberOr (const std::string& key, double fallback) const
    {
        const auto* value = get (key);
        return value != nullptr && value->type == Type::number ? value->number : fallback;
    }
    std::string textOr (const std::string& key, const std::string& fallback) const
    {
        const auto* value = get (key);
        return value != nullptr && value->type == Type::string ? value->text : fallback;
    }
};

// Parses a whole JSON document (\u escapes decoded to UTF-8). Throws std::runtime_error on malformed input.
Json parseJson (const std::string& source);

// The JSON string literal for a UTF-8 string (quotes included).
std::string jsonQuoted (const std::string& text);

} // namespace shizuku
