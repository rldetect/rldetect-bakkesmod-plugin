#include "MiniJson.h"

#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <sstream>
#include <stdexcept>

namespace minijson
{
    Value::Value() = default;
    Value::Value(std::nullptr_t) {}
    Value::Value(bool v) : type_(Type::Bool), boolValue_(v) {}
    Value::Value(double v) : type_(Type::Number), numberValue_(v) {}
    Value::Value(const std::string& v) : type_(Type::String), stringValue_(v) {}
    Value::Value(std::string&& v) : type_(Type::String), stringValue_(std::move(v)) {}
    Value::Value(const char* v) : type_(Type::String), stringValue_(v ? v : "") {}
    Value::Value(const Array& v) : type_(Type::Array), arrayValue_(v) {}
    Value::Value(Array&& v) : type_(Type::Array), arrayValue_(std::move(v)) {}
    Value::Value(const Object& v) : type_(Type::Object), objectValue_(v) {}
    Value::Value(Object&& v) : type_(Type::Object), objectValue_(std::move(v)) {}

    Value::Type Value::type() const { return type_; }
    bool Value::isNull() const { return type_ == Type::Null; }
    bool Value::isBool() const { return type_ == Type::Bool; }
    bool Value::isNumber() const { return type_ == Type::Number; }
    bool Value::isString() const { return type_ == Type::String; }
    bool Value::isArray() const { return type_ == Type::Array; }
    bool Value::isObject() const { return type_ == Type::Object; }

    bool Value::asBool(bool fallback) const { return isBool() ? boolValue_ : fallback; }
    double Value::asNumber(double fallback) const { return isNumber() ? numberValue_ : fallback; }
    const std::string& Value::asString() const
    {
        static const std::string empty;
        return isString() ? stringValue_ : empty;
    }
    const Value::Array& Value::asArray() const
    {
        static const Array empty;
        return isArray() ? arrayValue_ : empty;
    }
    const Value::Object& Value::asObject() const
    {
        static const Object empty;
        return isObject() ? objectValue_ : empty;
    }

    std::string Value::stringOr(const std::string& fallback) const
    {
        return isString() ? stringValue_ : fallback;
    }

    int Value::intOr(int fallback) const
    {
        if (!isNumber())
        {
            return fallback;
        }
        return static_cast<int>(numberValue_);
    }

    bool Value::boolOr(bool fallback) const
    {
        return isBool() ? boolValue_ : fallback;
    }

    const Value& Value::operator[](const std::string& key) const
    {
        static const Value nullValue;
        if (!isObject())
        {
            return nullValue;
        }

        const auto it = objectValue_.find(key);
        return it != objectValue_.end() ? it->second : nullValue;
    }

    const Value& Value::operator[](size_t index) const
    {
        static const Value nullValue;
        if (!isArray() || index >= arrayValue_.size())
        {
            return nullValue;
        }
        return arrayValue_[index];
    }

    bool Value::contains(const std::string& key) const
    {
        return isObject() && objectValue_.find(key) != objectValue_.end();
    }

    namespace
    {
        class Parser
        {
        public:
            explicit Parser(const std::string& input)
                : input_(input), ptr_(input_.c_str()), end_(input_.c_str() + input_.size())
            {
            }

            bool ParseRoot(Value& out, std::string& error)
            {
                SkipWhitespace();
                if (!ParseValue(out, error))
                {
                    return false;
                }

                SkipWhitespace();
                if (ptr_ != end_)
                {
                    error = MakeError("Unexpected trailing characters");
                    return false;
                }

                return true;
            }

        private:
            const std::string& input_;
            const char* ptr_;
            const char* end_;

            void SkipWhitespace()
            {
                while (ptr_ < end_ && std::isspace(static_cast<unsigned char>(*ptr_)))
                {
                    ++ptr_;
                }
            }

            std::string MakeError(const std::string& message) const
            {
                std::ostringstream oss;
                oss << message << " at byte " << (ptr_ - input_.c_str());
                return oss.str();
            }

            bool ParseValue(Value& out, std::string& error)
            {
                SkipWhitespace();
                if (ptr_ >= end_)
                {
                    error = MakeError("Unexpected end of input");
                    return false;
                }

                switch (*ptr_)
                {
                case 'n': return ParseNull(out, error);
                case 't':
                case 'f': return ParseBool(out, error);
                case '"': return ParseStringValue(out, error);
                case '[': return ParseArray(out, error);
                case '{': return ParseObject(out, error);
                default:
                    if (*ptr_ == '-' || std::isdigit(static_cast<unsigned char>(*ptr_)))
                    {
                        return ParseNumber(out, error);
                    }
                    error = MakeError("Unexpected character");
                    return false;
                }
            }

            bool MatchLiteral(const char* literal)
            {
                const char* lit = literal;
                const char* walker = ptr_;
                while (*lit != '\0')
                {
                    if (walker >= end_ || *walker != *lit)
                    {
                        return false;
                    }
                    ++walker;
                    ++lit;
                }
                ptr_ = walker;
                return true;
            }

            bool ParseNull(Value& out, std::string& error)
            {
                if (!MatchLiteral("null"))
                {
                    error = MakeError("Invalid literal");
                    return false;
                }
                out = Value();
                return true;
            }

            bool ParseBool(Value& out, std::string& error)
            {
                if (MatchLiteral("true"))
                {
                    out = Value(true);
                    return true;
                }
                if (MatchLiteral("false"))
                {
                    out = Value(false);
                    return true;
                }
                error = MakeError("Invalid boolean");
                return false;
            }

            static void AppendUtf8(std::string& out, unsigned int codepoint)
            {
                if (codepoint <= 0x7F)
                {
                    out.push_back(static_cast<char>(codepoint));
                }
                else if (codepoint <= 0x7FF)
                {
                    out.push_back(static_cast<char>(0xC0 | (codepoint >> 6)));
                    out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
                }
                else if (codepoint <= 0xFFFF)
                {
                    out.push_back(static_cast<char>(0xE0 | (codepoint >> 12)));
                    out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
                    out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
                }
                else
                {
                    out.push_back(static_cast<char>(0xF0 | (codepoint >> 18)));
                    out.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F)));
                    out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
                    out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
                }
            }

            bool ParseHex4(unsigned int& value)
            {
                value = 0;
                for (int i = 0; i < 4; ++i)
                {
                    if (ptr_ >= end_)
                    {
                        return false;
                    }

                    value <<= 4;
                    const char c = *ptr_++;
                    if (c >= '0' && c <= '9')
                    {
                        value |= static_cast<unsigned int>(c - '0');
                    }
                    else if (c >= 'a' && c <= 'f')
                    {
                        value |= static_cast<unsigned int>(10 + c - 'a');
                    }
                    else if (c >= 'A' && c <= 'F')
                    {
                        value |= static_cast<unsigned int>(10 + c - 'A');
                    }
                    else
                    {
                        return false;
                    }
                }
                return true;
            }

            bool ParseString(std::string& out, std::string& error)
            {
                if (ptr_ >= end_ || *ptr_ != '"')
                {
                    error = MakeError("Expected string");
                    return false;
                }

                ++ptr_;
                out.clear();

                while (ptr_ < end_)
                {
                    const char c = *ptr_++;
                    if (c == '"')
                    {
                        return true;
                    }

                    if (c == '\\')
                    {
                        if (ptr_ >= end_)
                        {
                            error = MakeError("Incomplete escape sequence");
                            return false;
                        }

                        const char esc = *ptr_++;
                        switch (esc)
                        {
                        case '"': out.push_back('"'); break;
                        case '\\': out.push_back('\\'); break;
                        case '/': out.push_back('/'); break;
                        case 'b': out.push_back('\b'); break;
                        case 'f': out.push_back('\f'); break;
                        case 'n': out.push_back('\n'); break;
                        case 'r': out.push_back('\r'); break;
                        case 't': out.push_back('\t'); break;
                        case 'u':
                        {
                            unsigned int codepoint = 0;
                            if (!ParseHex4(codepoint))
                            {
                                error = MakeError("Invalid unicode escape");
                                return false;
                            }

                            if (codepoint >= 0xD800 && codepoint <= 0xDBFF)
                            {
                                const char* save = ptr_;
                                if (ptr_ + 1 < end_ && ptr_[0] == '\\' && ptr_[1] == 'u')
                                {
                                    ptr_ += 2;
                                    unsigned int low = 0;
                                    if (ParseHex4(low) && low >= 0xDC00 && low <= 0xDFFF)
                                    {
                                        codepoint = 0x10000 + (((codepoint - 0xD800) << 10) | (low - 0xDC00));
                                    }
                                    else
                                    {
                                        ptr_ = save;
                                    }
                                }
                            }

                            AppendUtf8(out, codepoint);
                            break;
                        }
                        default:
                            error = MakeError("Unsupported escape sequence");
                            return false;
                        }
                    }
                    else
                    {
                        if (static_cast<unsigned char>(c) < 0x20)
                        {
                            error = MakeError("Control character in string");
                            return false;
                        }
                        out.push_back(c);
                    }
                }

                error = MakeError("Unterminated string");
                return false;
            }

            bool ParseStringValue(Value& out, std::string& error)
            {
                std::string s;
                if (!ParseString(s, error))
                {
                    return false;
                }
                out = Value(std::move(s));
                return true;
            }

            bool ParseNumber(Value& out, std::string& error)
            {
                const char* start = ptr_;

                if (*ptr_ == '-')
                {
                    ++ptr_;
                }

                if (ptr_ >= end_)
                {
                    error = MakeError("Incomplete number");
                    return false;
                }

                if (*ptr_ == '0')
                {
                    ++ptr_;
                }
                else
                {
                    if (!std::isdigit(static_cast<unsigned char>(*ptr_)))
                    {
                        error = MakeError("Invalid number");
                        return false;
                    }
                    while (ptr_ < end_ && std::isdigit(static_cast<unsigned char>(*ptr_)))
                    {
                        ++ptr_;
                    }
                }

                if (ptr_ < end_ && *ptr_ == '.')
                {
                    ++ptr_;
                    if (ptr_ >= end_ || !std::isdigit(static_cast<unsigned char>(*ptr_)))
                    {
                        error = MakeError("Invalid fractional number");
                        return false;
                    }
                    while (ptr_ < end_ && std::isdigit(static_cast<unsigned char>(*ptr_)))
                    {
                        ++ptr_;
                    }
                }

                if (ptr_ < end_ && (*ptr_ == 'e' || *ptr_ == 'E'))
                {
                    ++ptr_;
                    if (ptr_ < end_ && (*ptr_ == '+' || *ptr_ == '-'))
                    {
                        ++ptr_;
                    }
                    if (ptr_ >= end_ || !std::isdigit(static_cast<unsigned char>(*ptr_)))
                    {
                        error = MakeError("Invalid exponent");
                        return false;
                    }
                    while (ptr_ < end_ && std::isdigit(static_cast<unsigned char>(*ptr_)))
                    {
                        ++ptr_;
                    }
                }

                std::string token(start, ptr_);
                char* parseEnd = nullptr;
                const double value = std::strtod(token.c_str(), &parseEnd);
                if (parseEnd == token.c_str() || !std::isfinite(value))
                {
                    error = MakeError("Invalid numeric value");
                    return false;
                }

                out = Value(value);
                return true;
            }

            bool ParseArray(Value& out, std::string& error)
            {
                if (*ptr_ != '[')
                {
                    error = MakeError("Expected array");
                    return false;
                }

                ++ptr_;
                SkipWhitespace();

                Value::Array arr;
                if (ptr_ < end_ && *ptr_ == ']')
                {
                    ++ptr_;
                    out = Value(std::move(arr));
                    return true;
                }

                while (true)
                {
                    Value item;
                    if (!ParseValue(item, error))
                    {
                        return false;
                    }
                    arr.push_back(std::move(item));

                    SkipWhitespace();
                    if (ptr_ >= end_)
                    {
                        error = MakeError("Unterminated array");
                        return false;
                    }

                    if (*ptr_ == ']')
                    {
                        ++ptr_;
                        out = Value(std::move(arr));
                        return true;
                    }

                    if (*ptr_ != ',')
                    {
                        error = MakeError("Expected ',' or ']'");
                        return false;
                    }

                    ++ptr_;
                    SkipWhitespace();
                }
            }

            bool ParseObject(Value& out, std::string& error)
            {
                if (*ptr_ != '{')
                {
                    error = MakeError("Expected object");
                    return false;
                }

                ++ptr_;
                SkipWhitespace();

                Value::Object obj;
                if (ptr_ < end_ && *ptr_ == '}')
                {
                    ++ptr_;
                    out = Value(std::move(obj));
                    return true;
                }

                while (true)
                {
                    std::string key;
                    if (!ParseString(key, error))
                    {
                        return false;
                    }

                    SkipWhitespace();
                    if (ptr_ >= end_ || *ptr_ != ':')
                    {
                        error = MakeError("Expected ':' after object key");
                        return false;
                    }
                    ++ptr_;
                    SkipWhitespace();

                    Value value;
                    if (!ParseValue(value, error))
                    {
                        return false;
                    }

                    obj.emplace(std::move(key), std::move(value));

                    SkipWhitespace();
                    if (ptr_ >= end_)
                    {
                        error = MakeError("Unterminated object");
                        return false;
                    }

                    if (*ptr_ == '}')
                    {
                        ++ptr_;
                        out = Value(std::move(obj));
                        return true;
                    }

                    if (*ptr_ != ',')
                    {
                        error = MakeError("Expected ',' or '}'");
                        return false;
                    }

                    ++ptr_;
                    SkipWhitespace();
                }
            }
        };

        std::string EscapeString(const std::string& input)
        {
            std::ostringstream oss;
            for (const unsigned char c : input)
            {
                switch (c)
                {
                case '"': oss << "\\\""; break;
                case '\\': oss << "\\\\"; break;
                case '\b': oss << "\\b"; break;
                case '\f': oss << "\\f"; break;
                case '\n': oss << "\\n"; break;
                case '\r': oss << "\\r"; break;
                case '\t': oss << "\\t"; break;
                default:
                    if (c < 0x20)
                    {
                        const char* hex = "0123456789abcdef";
                        oss << "\\u00" << hex[(c >> 4) & 0xF] << hex[c & 0xF];
                    }
                    else
                    {
                        oss << static_cast<char>(c);
                    }
                    break;
                }
            }
            return oss.str();
        }

        void DumpValue(std::ostringstream& oss, const Value& value)
        {
            switch (value.type())
            {
            case Value::Type::Null:
                oss << "null";
                break;
            case Value::Type::Bool:
                oss << (value.asBool() ? "true" : "false");
                break;
            case Value::Type::Number:
                oss << value.asNumber();
                break;
            case Value::Type::String:
                oss << '"' << EscapeString(value.asString()) << '"';
                break;
            case Value::Type::Array:
            {
                oss << '[';
                bool first = true;
                for (const auto& item : value.asArray())
                {
                    if (!first)
                    {
                        oss << ',';
                    }
                    first = false;
                    DumpValue(oss, item);
                }
                oss << ']';
                break;
            }
            case Value::Type::Object:
            {
                oss << '{';
                bool first = true;
                for (const auto& [key, item] : value.asObject())
                {
                    if (!first)
                    {
                        oss << ',';
                    }
                    first = false;
                    oss << '"' << EscapeString(key) << "\":";
                    DumpValue(oss, item);
                }
                oss << '}';
                break;
            }
            }
        }
    }

    bool Parse(const std::string& input, Value& outValue, std::string& outError)
    {
        Parser parser(input);
        return parser.ParseRoot(outValue, outError);
    }

    std::string DumpCompact(const Value& value)
    {
        std::ostringstream oss;
        DumpValue(oss, value);
        return oss.str();
    }
}
