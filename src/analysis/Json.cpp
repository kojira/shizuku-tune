#include "Json.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>

namespace shizuku
{

namespace
{
void appendUtf8 (std::string& out, unsigned long codePoint)
{
    if (codePoint < 0x80)
        out += (char) codePoint;
    else if (codePoint < 0x800)
    {
        out += (char) (0xc0 | (codePoint >> 6));
        out += (char) (0x80 | (codePoint & 0x3f));
    }
    else if (codePoint < 0x10000)
    {
        out += (char) (0xe0 | (codePoint >> 12));
        out += (char) (0x80 | ((codePoint >> 6) & 0x3f));
        out += (char) (0x80 | (codePoint & 0x3f));
    }
    else
    {
        out += (char) (0xf0 | (codePoint >> 18));
        out += (char) (0x80 | ((codePoint >> 12) & 0x3f));
        out += (char) (0x80 | ((codePoint >> 6) & 0x3f));
        out += (char) (0x80 | (codePoint & 0x3f));
    }
}

class JsonParser
{
public:
    explicit JsonParser (const std::string& sourceIn) : source (sourceIn) {}

    Json parseDocument()
    {
        auto value = parseValue();
        skipSpace();
        if (position != source.size())
            fail ("trailing characters");
        return value;
    }

private:
    [[noreturn]] void fail (const char* what) const
    {
        throw std::runtime_error (std::string ("JSON parse error (") + what + ") at offset " + std::to_string (position));
    }
    void skipSpace()
    {
        while (position < source.size() && std::isspace ((unsigned char) source[position]))
            ++position;
    }
    char peek()
    {
        skipSpace();
        if (position >= source.size())
            fail ("unexpected end");
        return source[position];
    }
    void expect (char c)
    {
        if (peek() != c)
            fail ("unexpected character");
        ++position;
    }

    Json parseValue()
    {
        const char c = peek();
        Json value;
        if (c == '{')
        {
            value.type = Json::Type::object;
            ++position;
            if (peek() == '}')
            {
                ++position;
                return value;
            }
            for (;;)
            {
                const auto key = parseString();
                expect (':');
                value.members[key] = parseValue();
                if (peek() == ',')
                {
                    ++position;
                    continue;
                }
                expect ('}');
                return value;
            }
        }
        if (c == '[')
        {
            value.type = Json::Type::array;
            ++position;
            if (peek() == ']')
            {
                ++position;
                return value;
            }
            for (;;)
            {
                value.items.push_back (parseValue());
                if (peek() == ',')
                {
                    ++position;
                    continue;
                }
                expect (']');
                return value;
            }
        }
        if (c == '"')
        {
            value.type = Json::Type::string;
            value.text = parseString();
            return value;
        }
        if (source.compare (position, 4, "true") == 0 || source.compare (position, 5, "false") == 0)
        {
            value.type = Json::Type::boolean;
            value.number = source[position] == 't' ? 1.0 : 0.0;
            position += source[position] == 't' ? 4 : 5;
            return value;
        }
        if (source.compare (position, 4, "null") == 0)
        {
            position += 4;
            return value;
        }
        const char* begin = source.c_str() + position;
        char* end = nullptr;
        value.type = Json::Type::number;
        value.number = std::strtod (begin, &end);
        if (end == begin)
            fail ("bad value");
        position += (size_t) (end - begin);
        return value;
    }

    unsigned long parseHex4()
    {
        if (position + 4 > source.size())
            fail ("bad \\u escape");
        const auto digits = source.substr (position, 4);
        position += 4;
        char* end = nullptr;
        const auto value = std::strtoul (digits.c_str(), &end, 16);
        if (end != digits.c_str() + 4)
            fail ("bad \\u escape");
        return value;
    }

    std::string parseString()
    {
        expect ('"');
        std::string out;
        while (position < source.size() && source[position] != '"')
        {
            char c = source[position++];
            if (c == '\\' && position < source.size())
            {
                const char escaped = source[position++];
                switch (escaped)
                {
                    case 'n': out += '\n'; break;
                    case 't': out += '\t'; break;
                    case 'r': out += '\r'; break;
                    case 'b': out += '\b'; break;
                    case 'f': out += '\f'; break;
                    case 'u':
                    {
                        auto codePoint = parseHex4();
                        if (codePoint >= 0xd800 && codePoint < 0xdc00 && source.compare (position, 2, "\\u") == 0)
                        {
                            position += 2;
                            const auto low = parseHex4();
                            codePoint = 0x10000 + ((codePoint - 0xd800) << 10) + (low - 0xdc00);
                        }
                        appendUtf8 (out, codePoint);
                        break;
                    }
                    default: out += escaped; break;
                }
                continue;
            }
            out += c;
        }
        expect ('"');
        return out;
    }

    const std::string& source;
    size_t position = 0;
};
} // namespace

Json parseJson (const std::string& source)
{
    return JsonParser (source).parseDocument();
}

std::string jsonQuoted (const std::string& text)
{
    std::string out = "\"";
    for (const char c : text)
    {
        switch (c)
        {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\t': out += "\\t"; break;
            case '\r': out += "\\r"; break;
            default:
                if ((unsigned char) c < 0x20)
                {
                    char buffer[8];
                    std::snprintf (buffer, sizeof (buffer), "\\u%04x", (unsigned) (unsigned char) c);
                    out += buffer;
                }
                else
                    out += c;
        }
    }
    return out + "\"";
}

} // namespace shizuku
