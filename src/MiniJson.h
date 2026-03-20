#pragma once

#include <map>
#include <string>
#include <vector>

namespace minijson
{
    class Value
    {
    public:
        enum class Type
        {
            Null,
            Bool,
            Number,
            String,
            Array,
            Object
        };

        using Array = std::vector<Value>;
        using Object = std::map<std::string, Value>;

        Value();
        Value(std::nullptr_t);
        Value(bool v);
        Value(double v);
        Value(const std::string& v);
        Value(std::string&& v);
        Value(const char* v);
        Value(const Array& v);
        Value(Array&& v);
        Value(const Object& v);
        Value(Object&& v);

        Type type() const;

        bool isNull() const;
        bool isBool() const;
        bool isNumber() const;
        bool isString() const;
        bool isArray() const;
        bool isObject() const;

        bool asBool(bool fallback = false) const;
        double asNumber(double fallback = 0.0) const;
        const std::string& asString() const;
        const Array& asArray() const;
        const Object& asObject() const;

        std::string stringOr(const std::string& fallback) const;
        int intOr(int fallback) const;
        bool boolOr(bool fallback) const;

        const Value& operator[](const std::string& key) const;
        const Value& operator[](size_t index) const;
        bool contains(const std::string& key) const;

    private:
        Type type_ = Type::Null;
        bool boolValue_ = false;
        double numberValue_ = 0.0;
        std::string stringValue_;
        Array arrayValue_;
        Object objectValue_;
    };

    bool Parse(const std::string& input, Value& outValue, std::string& outError);
    std::string DumpCompact(const Value& value);
}
