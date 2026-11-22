#include "course_model.h"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <set>
#include <sstream>
#include <utility>

namespace course {
namespace {

const char* kFormat = "gong-course";
const int kMaxJsonDepth = 64;

bool fail(std::string& error, const std::string& message) {
    error = message;
    return false;
}

bool isDigit(char c) { return c >= '0' && c <= '9'; }

bool isLeapYearImpl(int year) {
    return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

int daysInMonthImpl(int year, int month) {
    static const int kDays[12] = {31, 28, 31, 30, 31, 30,
                                  31, 31, 30, 31, 30, 31};
    if (month < 1 || month > 12) return 0;
    if (month == 2 && isLeapYearImpl(year)) return 29;
    return kDays[month - 1];
}

long long daysFromCivil(int year, int month, int day) {
    // Howard Hinnant's civil calendar algorithm, independent of time_t.
    int y = year;
    const unsigned m = static_cast<unsigned>(month);
    const unsigned d = static_cast<unsigned>(day);
    y -= m <= 2;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return static_cast<long long>(era) * 146097 +
           static_cast<long long>(doe) - 719468;
}

void appendUtf8(std::string& out, unsigned int cp) {
    if (cp <= 0x7F) {
        out.push_back(static_cast<char>(cp));
    } else if (cp <= 0x7FF) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp <= 0xFFFF) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

struct JsonValue {
    enum Type { NUL, BOOL, NUMBER, STRING, ARRAY, OBJECT };

    Type type;
    bool boolean;
    double number;
    std::string text;
    std::vector<JsonValue> array;
    std::vector<std::pair<std::string, JsonValue> > object;

    JsonValue() : type(NUL), boolean(false), number(0.0) {}

    const JsonValue* find(const std::string& key) const {
        for (std::size_t i = 0; i < object.size(); ++i) {
            if (object[i].first == key) return &object[i].second;
        }
        return 0;
    }
};

class JsonParser {
public:
    explicit JsonParser(const std::string& input) : input_(input), pos_(0), depth_(0) {}

    bool parse(JsonValue& out, std::string& error) {
        skipWs();
        if (!parseValue(out, error)) return false;
        skipWs();
        if (pos_ != input_.size()) return fail(error, "trailing characters after JSON value");
        return true;
    }

private:
    const std::string& input_;
    std::size_t pos_;
    int depth_;

    void skipWs() {
        while (pos_ < input_.size()) {
            const char c = input_[pos_];
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
                ++pos_;
            } else {
                break;
            }
        }
    }

    bool parseValue(JsonValue& out, std::string& error) {
        skipWs();
        if (pos_ >= input_.size()) return fail(error, "unexpected end of JSON");
        const char c = input_[pos_];
        if (c == '{') return parseObject(out, error);
        if (c == '[') return parseArray(out, error);
        if (c == '"') {
            out.type = JsonValue::STRING;
            return parseString(out.text, error);
        }
        if (c == 't') return parseLiteral("true", JsonValue::BOOL, true, out, error);
        if (c == 'f') return parseLiteral("false", JsonValue::BOOL, false, out, error);
        if (c == 'n') return parseLiteral("null", JsonValue::NUL, false, out, error);
        if (c == '-' || isDigit(c)) return parseNumber(out, error);
        return fail(error, "unexpected character in JSON");
    }

    bool parseLiteral(const char* literal, JsonValue::Type type, bool boolean,
                      JsonValue& out, std::string& error) {
        const std::size_t len = std::strlen(literal);
        if (input_.compare(pos_, len, literal) != 0) return fail(error, "invalid JSON literal");
        pos_ += len;
        out.type = type;
        out.boolean = boolean;
        return true;
    }

    bool parseObject(JsonValue& out, std::string& error) {
        if (++depth_ > kMaxJsonDepth) return fail(error, "JSON nesting too deep");
        out.type = JsonValue::OBJECT;
        ++pos_;  // {
        skipWs();
        if (pos_ < input_.size() && input_[pos_] == '}') {
            ++pos_;
            --depth_;
            return true;
        }
        while (true) {
            skipWs();
            if (pos_ >= input_.size() || input_[pos_] != '"') {
                return fail(error, "object key must be a string");
            }
            std::string key;
            if (!parseString(key, error)) return false;
            skipWs();
            if (pos_ >= input_.size() || input_[pos_] != ':') {
                return fail(error, "missing ':' after object key");
            }
            ++pos_;
            JsonValue value;
            if (!parseValue(value, error)) return false;
            for (std::size_t i = 0; i < out.object.size(); ++i) {
                if (out.object[i].first == key) return fail(error, "duplicate object key: " + key);
            }
            out.object.push_back(std::make_pair(key, value));
            skipWs();
            if (pos_ >= input_.size()) return fail(error, "unterminated object");
            if (input_[pos_] == '}') {
                ++pos_;
                --depth_;
                return true;
            }
            if (input_[pos_] != ',') return fail(error, "expected ',' or '}' in object");
            ++pos_;
        }
    }

    bool parseArray(JsonValue& out, std::string& error) {
        if (++depth_ > kMaxJsonDepth) return fail(error, "JSON nesting too deep");
        out.type = JsonValue::ARRAY;
        ++pos_;  // [
        skipWs();
        if (pos_ < input_.size() && input_[pos_] == ']') {
            ++pos_;
            --depth_;
            return true;
        }
        while (true) {
            JsonValue value;
            if (!parseValue(value, error)) return false;
            out.array.push_back(value);
            skipWs();
            if (pos_ >= input_.size()) return fail(error, "unterminated array");
            if (input_[pos_] == ']') {
                ++pos_;
                --depth_;
                return true;
            }
            if (input_[pos_] != ',') return fail(error, "expected ',' or ']' in array");
            ++pos_;
        }
    }

    bool parseString(std::string& out, std::string& error) {
        if (pos_ >= input_.size() || input_[pos_] != '"') return fail(error, "expected string");
        ++pos_;
        out.clear();
        while (pos_ < input_.size()) {
            const unsigned char c = static_cast<unsigned char>(input_[pos_++]);
            if (c == '"') return true;
            if (c < 0x20) return fail(error, "control character in string");
            if (c != '\\') {
                out.push_back(static_cast<char>(c));
                continue;
            }
            if (pos_ >= input_.size()) return fail(error, "unterminated escape");
            const char esc = input_[pos_++];
            switch (esc) {
                case '"': out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case '/': out.push_back('/'); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                case 'u': {
                    unsigned int cp = 0;
                    if (!parseHex4(cp, error)) return false;
                    if (cp >= 0xD800 && cp <= 0xDBFF) {
                        if (pos_ + 2 > input_.size() || input_[pos_] != '\\' ||
                            input_[pos_ + 1] != 'u') {
                            return fail(error, "missing low surrogate");
                        }
                        pos_ += 2;
                        unsigned int low = 0;
                        if (!parseHex4(low, error)) return false;
                        if (low < 0xDC00 || low > 0xDFFF) return fail(error, "invalid low surrogate");
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                    } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                        return fail(error, "unexpected low surrogate");
                    }
                    appendUtf8(out, cp);
                    break;
                }
                default:
                    return fail(error, "invalid escape sequence");
            }
        }
        return fail(error, "unterminated string");
    }

    bool parseHex4(unsigned int& out, std::string& error) {
        if (pos_ + 4 > input_.size()) return fail(error, "short \\u escape");
        out = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = input_[pos_++];
            out <<= 4;
            if (c >= '0' && c <= '9') out |= static_cast<unsigned int>(c - '0');
            else if (c >= 'a' && c <= 'f') out |= static_cast<unsigned int>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') out |= static_cast<unsigned int>(c - 'A' + 10);
            else return fail(error, "invalid hex digit");
        }
        return true;
    }

    bool parseNumber(JsonValue& out, std::string& error) {
        const std::size_t start = pos_;
        if (input_[pos_] == '-') ++pos_;
        if (pos_ >= input_.size()) return fail(error, "short number");
        if (input_[pos_] == '0') {
            ++pos_;
            if (pos_ < input_.size() && isDigit(input_[pos_])) {
                return fail(error, "leading zero in number");
            }
        } else if (input_[pos_] >= '1' && input_[pos_] <= '9') {
            while (pos_ < input_.size() && isDigit(input_[pos_])) ++pos_;
        } else {
            return fail(error, "invalid number");
        }
        if (pos_ < input_.size() && input_[pos_] == '.') {
            ++pos_;
            if (pos_ >= input_.size() || !isDigit(input_[pos_])) {
                return fail(error, "fraction requires digits");
            }
            while (pos_ < input_.size() && isDigit(input_[pos_])) ++pos_;
        }
        if (pos_ < input_.size() && (input_[pos_] == 'e' || input_[pos_] == 'E')) {
            ++pos_;
            if (pos_ < input_.size() && (input_[pos_] == '+' || input_[pos_] == '-')) ++pos_;
            if (pos_ >= input_.size() || !isDigit(input_[pos_])) {
                return fail(error, "exponent requires digits");
            }
            while (pos_ < input_.size() && isDigit(input_[pos_])) ++pos_;
        }
        const std::string token = input_.substr(start, pos_ - start);
        errno = 0;
        char* end = 0;
        const double value = std::strtod(token.c_str(), &end);
        if (end != token.c_str() + token.size() || errno == ERANGE || !std::isfinite(value)) {
            return fail(error, "number out of range");
        }
        out.type = JsonValue::NUMBER;
        out.number = value;
        return true;
    }
};

bool readInt(const JsonValue& value, const char* field, long long min,
             long long max, long long& out, std::string& error) {
    if (value.type != JsonValue::NUMBER) return fail(error, std::string(field) + " must be a number");
    if (!std::isfinite(value.number) || std::floor(value.number) != value.number) {
        return fail(error, std::string(field) + " must be an integer");
    }
    if (value.number < static_cast<double>(min) || value.number > static_cast<double>(max)) {
        return fail(error, std::string(field) + " is out of range");
    }
    out = static_cast<long long>(value.number);
    return true;
}

bool readString(const JsonValue& value, const char* field, std::string& out,
                std::string& error) {
    if (value.type != JsonValue::STRING) return fail(error, std::string(field) + " must be a string");
    out = value.text;
    return true;
}

bool readBool(const JsonValue& value, const char* field, bool& out,
              std::string& error) {
    if (value.type != JsonValue::BOOL) return fail(error, std::string(field) + " must be a boolean");
    out = value.boolean;
    return true;
}

bool checkKeys(const JsonValue& object, std::initializer_list<const char*> allowed,
               std::string& error) {
    for (std::size_t i = 0; i < object.object.size(); ++i) {
        bool ok = false;
        for (const char* key : allowed) {
            if (object.object[i].first == key) {
                ok = true;
                break;
            }
        }
        if (!ok) return fail(error, "unknown field: " + object.object[i].first);
    }
    return true;
}

bool validateEventList(const std::vector<Event>& events, std::string& error) {
    if (events.size() > kMaxEventsPerDay) return fail(error, "too many events in one day");
    std::set<std::uint32_t> ids;
    std::set<int> times;
    for (std::size_t i = 0; i < events.size(); ++i) {
        if (!validateEvent(events[i], error)) return false;
        if (!ids.insert(events[i].id).second) return fail(error, "duplicate event id");
        const int key = static_cast<int>(events[i].hour) * 60 + events[i].minute;
        if (!times.insert(key).second) return fail(error, "duplicate event time");
    }
    return true;
}

bool assignMissingIds(std::vector<Event>& events, std::string& error) {
    std::set<std::uint32_t> used;
    std::uint32_t maxId = 0;
    for (std::size_t i = 0; i < events.size(); ++i) {
        if (events[i].id != 0) {
            if (!used.insert(events[i].id).second) return fail(error, "duplicate event id");
            if (events[i].id > maxId) maxId = events[i].id;
        }
    }
    for (std::size_t i = 0; i < events.size(); ++i) {
        if (events[i].id != 0) continue;
        if (maxId == 0xFFFFFFFFu) return fail(error, "event id space exhausted");
        events[i].id = ++maxId;
        if (!used.insert(events[i].id).second) return fail(error, "duplicate event id");
    }
    return true;
}

bool parseEvent(const JsonValue& value, Event& event, std::string& error) {
    if (value.type != JsonValue::OBJECT) return fail(error, "event must be an object");
    if (!checkKeys(value, {"id", "hour", "min", "track", "loop", "vol", "en", "desc"}, error)) {
        return false;
    }
    event = Event();
    const JsonValue* id = value.find("id");
    if (id) {
        long long v = 0;
        if (!readInt(*id, "id", 0, 0xFFFFFFFFLL, v, error)) return false;
        event.id = static_cast<std::uint32_t>(v);
    }
    const JsonValue* hour = value.find("hour");
    const JsonValue* minute = value.find("min");
    if (!hour) return fail(error, "event.hour is required");
    if (!minute) return fail(error, "event.min is required");
    long long v = 0;
    if (!readInt(*hour, "hour", 0, 23, v, error)) return false;
    event.hour = static_cast<std::uint8_t>(v);
    if (!readInt(*minute, "min", 0, 59, v, error)) return false;
    event.minute = static_cast<std::uint8_t>(v);

    const JsonValue* track = value.find("track");
    if (track) {
        if (!readInt(*track, "track", 1, kMaxTrack, v, error)) return false;
        event.track = static_cast<std::uint8_t>(v);
    }
    const JsonValue* loop = value.find("loop");
    if (loop) {
        if (!readInt(*loop, "loop", 1, kMaxLoop, v, error)) return false;
        event.loop = static_cast<std::uint8_t>(v);
    }
    const JsonValue* volume = value.find("vol");
    if (volume) {
        if (!readInt(*volume, "vol", 0, kMaxVolume, v, error)) return false;
        event.volume = static_cast<std::uint8_t>(v);
    }
    const JsonValue* enabled = value.find("en");
    if (enabled) {
        if (!readBool(*enabled, "en", event.enabled, error)) return false;
    }
    const JsonValue* desc = value.find("desc");
    if (desc) {
        if (!readString(*desc, "desc", event.description, error)) return false;
    }
    if (event.description.size() > kMaxDescBytes) {
        return fail(error, "event description is too long");
    }
    return true;
}

bool parseEventArray(const JsonValue& value, std::vector<Event>& events,
                     std::string& error) {
    if (value.type != JsonValue::ARRAY) return fail(error, "events must be an array");
    if (value.array.size() > kMaxEventsPerDay) return fail(error, "too many events in one day");
    events.clear();
    for (std::size_t i = 0; i < value.array.size(); ++i) {
        Event event;
        if (!parseEvent(value.array[i], event, error)) return false;
        events.push_back(event);
    }
    if (!assignMissingIds(events, error)) return false;
    return validateEventList(events, error);
}

bool parseTemplates(const JsonValue& value,
                    std::map<std::string, std::vector<Event> >& templates,
                    std::string& error) {
    if (value.type != JsonValue::OBJECT) return fail(error, "templates must be an object");
    templates.clear();
    for (std::size_t i = 0; i < value.object.size(); ++i) {
        const std::string& name = value.object[i].first;
        if (name.empty()) return fail(error, "template name must not be empty");
        std::vector<Event> events;
        if (!parseEventArray(value.object[i].second, events, error)) return false;
        templates[name] = events;
    }
    return true;
}

bool parseDayV2(const JsonValue& value, Day& day, std::string& error) {
    if (value.type != JsonValue::OBJECT) return fail(error, "v2 day must be an object");
    if (!checkKeys(value, {"template", "events"}, error)) return false;
    day = Day();
    const JsonValue* tmpl = value.find("template");
    if (tmpl) {
        if (!readString(*tmpl, "template", day.templateName, error)) return false;
        if (day.templateName.empty()) return fail(error, "template name must not be empty");
    }
    const JsonValue* events = value.find("events");
    if (events) {
        if (!parseEventArray(*events, day.events, error)) return false;
        day.hasOwnEvents = true;
    } else if (tmpl) {
        day.hasOwnEvents = false;
    } else {
        day.hasOwnEvents = true;
        day.events.clear();
    }
    return true;
}

bool parseDayV1(const JsonValue& value, Day& day, std::string& error) {
    day = Day();
    if (value.type == JsonValue::STRING) {
        if (value.text.empty()) return fail(error, "template name must not be empty");
        day.templateName = value.text;
        day.hasOwnEvents = false;
        return true;
    }
    if (value.type == JsonValue::ARRAY) {
        if (!parseEventArray(value, day.events, error)) return false;
        day.hasOwnEvents = true;
        return true;
    }
    return fail(error, "v1 day must be a template name or an event array");
}

bool materialize(Course& course, std::size_t dayIndex, std::string& error) {
    if (dayIndex >= course.days.size()) return fail(error, "day index out of range");
    Day& day = course.days[dayIndex];
    if (day.hasOwnEvents) return true;
    if (day.templateName.empty()) return fail(error, "day has neither template nor events");
    std::map<std::string, std::vector<Event> >::const_iterator it =
        course.templates.find(day.templateName);
    if (it == course.templates.end()) return fail(error, "unknown template: " + day.templateName);
    day.events = it->second;
    day.hasOwnEvents = true;
    day.templateName.clear();
    return true;
}

bool nextFreeId(const std::vector<Event>& events, std::uint32_t& out,
                std::string& error) {
    std::uint32_t maxId = 0;
    for (std::size_t i = 0; i < events.size(); ++i) {
        if (events[i].id > maxId) maxId = events[i].id;
    }
    if (maxId == 0xFFFFFFFFu) return fail(error, "event id space exhausted");
    out = maxId + 1;
    return true;
}

std::string escapeJson(const std::string& text) {
    std::string out;
    out.reserve(text.size() + 8);
    for (std::size_t i = 0; i < text.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    static const char* hex = "0123456789abcdef";
                    out += "\\u00";
                    out.push_back(hex[(c >> 4) & 0x0F]);
                    out.push_back(hex[c & 0x0F]);
                } else {
                    out.push_back(static_cast<char>(c));
                }
        }
    }
    return out;
}

void writeEvent(std::string& out, const Event& event) {
    out += "{\"id\":";
    out += std::to_string(event.id);
    out += ",\"hour\":";
    out += std::to_string(static_cast<int>(event.hour));
    out += ",\"min\":";
    out += std::to_string(static_cast<int>(event.minute));
    out += ",\"track\":";
    out += std::to_string(static_cast<int>(event.track));
    out += ",\"loop\":";
    out += std::to_string(static_cast<int>(event.loop));
    out += ",\"vol\":";
    out += std::to_string(static_cast<int>(event.volume));
    out += ",\"en\":";
    out += event.enabled ? "true" : "false";
    out += ",\"desc\":\"";
    out += escapeJson(event.description);
    out += "\"}";
}

void writeEventArray(std::string& out, const std::vector<Event>& events) {
    out += '[';
    for (std::size_t i = 0; i < events.size(); ++i) {
        if (i) out += ',';
        writeEvent(out, events[i]);
    }
    out += ']';
}

}  // namespace

Event::Event()
    : id(0), hour(0), minute(0), track(kDefaultTrack), loop(kDefaultLoop),
      volume(kDefaultVolume), enabled(true) {}

Day::Day() : hasOwnEvents(false) {}

Course::Course() : generation(0) {}

Date::Date() : year(1970), month(1), day(1) {}

bool isLeapYear(int year) { return isLeapYearImpl(year); }

int daysInMonth(int year, int month) { return daysInMonthImpl(year, month); }

bool parseDate(const std::string& text, Date& out, std::string& error) {
    std::string digits;
    if (text.size() == 10 && text[4] == '-' && text[7] == '-') {
        digits = text.substr(0, 4) + text.substr(5, 2) + text.substr(8, 2);
    } else if (text.size() == 8) {
        digits = text;
    } else {
        return fail(error, "date must be YYYY-MM-DD or YYYYMMDD");
    }
    for (std::size_t i = 0; i < digits.size(); ++i) {
        if (!isDigit(digits[i])) return fail(error, "date contains non-digit characters");
    }
    const int year = std::atoi(digits.substr(0, 4).c_str());
    const int month = std::atoi(digits.substr(4, 2).c_str());
    const int day = std::atoi(digits.substr(6, 2).c_str());
    if (year < 1970 || year > 9999) return fail(error, "year is out of range");
    if (month < 1 || month > 12) return fail(error, "month is out of range");
    if (day < 1 || day > daysInMonthImpl(year, month)) return fail(error, "day is out of range");
    out.year = year;
    out.month = month;
    out.day = day;
    return true;
}

std::string formatDate(const Date& date) {
    char buffer[11];
    std::snprintf(buffer, sizeof(buffer), "%04d-%02d-%02d", date.year, date.month, date.day);
    return std::string(buffer);
}

long long civilDaysBetween(const Date& from, const Date& to) {
    return daysFromCivil(to.year, to.month, to.day) -
           daysFromCivil(from.year, from.month, from.day);
}

Date addCivilDays(const Date& date, long long days) {
    long long z = daysFromCivil(date.year, date.month, date.day) + days;
    z += 719468;
    const long long era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = static_cast<unsigned>(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const int y = static_cast<int>(yoe) + static_cast<int>(era * 400);
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    const unsigned d = doy - (153 * mp + 2) / 5 + 1;
    const unsigned m = mp < 10 ? mp + 3 : mp - 9;
    Date out;
    out.year = y + (m <= 2 ? 1 : 0);
    out.month = static_cast<int>(m);
    out.day = static_cast<int>(d);
    return out;
}

bool computeDayIndex(int activatedDay, const Date& activationDate,
                     const Date& today, int courseLength, int& out,
                     std::string& error) {
    if (courseLength < 1 || courseLength > static_cast<int>(kMaxDays)) {
        return fail(error, "course length is out of range");
    }
    if (activatedDay < 0 || activatedDay >= courseLength) {
        return fail(error, "activated day is out of range");
    }
    const long long diff = civilDaysBetween(activationDate, today);
    if (diff < 0) return fail(error, "today is before activation date");
    const long long raw = static_cast<long long>(activatedDay) + diff;
    out = static_cast<int>(raw % courseLength);
    return true;
}

int nextDayIndex(int currentDay, int courseLength) {
    if (courseLength <= 0) return -1;
    if (currentDay < 0 || currentDay >= courseLength) return -1;
    return (currentDay + 1) % courseLength;
}

bool validateEvent(const Event& event, std::string& error) {
    if (event.id == 0) return fail(error, "event id must be non-zero");
    if (event.hour > 23) return fail(error, "event hour is out of range");
    if (event.minute > 59) return fail(error, "event minute is out of range");
    if (event.track < 1 || event.track > kMaxTrack) return fail(error, "event track is out of range");
    if (event.loop < 1 || event.loop > kMaxLoop) return fail(error, "event loop is out of range");
    if (event.volume > kMaxVolume) return fail(error, "event volume is out of range");
    if (event.description.size() > kMaxDescBytes) return fail(error, "event description is too long");
    return true;
}

bool validate(const Course& course, std::string& error) {
    if (course.days.empty()) return fail(error, "course must contain at least one day");
    if (course.days.size() > kMaxDays) return fail(error, "course has too many days");
    for (std::map<std::string, std::vector<Event> >::const_iterator it =
             course.templates.begin(); it != course.templates.end(); ++it) {
        if (it->first.empty()) return fail(error, "template name must not be empty");
        if (!validateEventList(it->second, error)) return false;
    }
    for (std::size_t i = 0; i < course.days.size(); ++i) {
        const Day& day = course.days[i];
        if (!day.templateName.empty() &&
            course.templates.find(day.templateName) == course.templates.end()) {
            return fail(error, "unknown template: " + day.templateName);
        }
        if (day.hasOwnEvents) {
            if (!validateEventList(day.events, error)) return false;
        } else {
            if (day.templateName.empty()) return fail(error, "day has neither template nor events");
            if (course.templates.find(day.templateName) == course.templates.end()) {
                return fail(error, "unknown template: " + day.templateName);
            }
            if (!day.events.empty()) return fail(error, "template day must not carry events");
        }
    }
    return true;
}

bool resolvedEvents(const Course& course, std::size_t dayIndex,
                    std::vector<Event>& out, std::string& error) {
    if (dayIndex >= course.days.size()) return fail(error, "day index out of range");
    const Day& day = course.days[dayIndex];
    if (day.hasOwnEvents) {
        out = day.events;
        return true;
    }
    if (day.templateName.empty()) return fail(error, "day has neither template nor events");
    std::map<std::string, std::vector<Event> >::const_iterator it =
        course.templates.find(day.templateName);
    if (it == course.templates.end()) return fail(error, "unknown template: " + day.templateName);
    out = it->second;
    return true;
}

bool insertBefore(Course& course, std::size_t index, bool copySource,
                  std::string& error) {
    if (course.days.size() >= kMaxDays) return fail(error, "course already has 47 days");
    if (index >= course.days.size()) return fail(error, "day index out of range");
    Day day;
    day.hasOwnEvents = true;
    if (copySource) {
        if (!resolvedEvents(course, index, day.events, error)) return false;
    }
    course.days.insert(course.days.begin() + static_cast<std::ptrdiff_t>(index), day);
    return true;
}

bool insertAfter(Course& course, std::size_t index, bool copySource,
                 std::string& error) {
    if (course.days.size() >= kMaxDays) return fail(error, "course already has 47 days");
    if (index >= course.days.size()) return fail(error, "day index out of range");
    Day day;
    day.hasOwnEvents = true;
    if (copySource) {
        if (!resolvedEvents(course, index, day.events, error)) return false;
    }
    course.days.insert(course.days.begin() + static_cast<std::ptrdiff_t>(index + 1), day);
    return true;
}

bool deleteDay(Course& course, std::size_t index, std::string& error) {
    if (course.days.size() <= 1) return fail(error, "cannot delete the last day");
    if (index >= course.days.size()) return fail(error, "day index out of range");
    course.days.erase(course.days.begin() + static_cast<std::ptrdiff_t>(index));
    return true;
}

bool clearDay(Course& course, std::size_t index, std::string& error) {
    if (index >= course.days.size()) return fail(error, "day index out of range");
    if (!materialize(course, index, error)) return false;
    course.days[index].events.clear();
    return true;
}

bool addEvent(Course& course, std::size_t dayIndex, Event event,
              std::string& error) {
    if (dayIndex >= course.days.size()) return fail(error, "day index out of range");
    if (!materialize(course, dayIndex, error)) return false;
    std::vector<Event>& events = course.days[dayIndex].events;
    if (events.size() >= kMaxEventsPerDay) return fail(error, "day already has 32 events");
    if (event.id == 0) {
        if (!nextFreeId(events, event.id, error)) return false;
    }
    if (!validateEvent(event, error)) return false;
    for (std::size_t i = 0; i < events.size(); ++i) {
        if (events[i].id == event.id) return fail(error, "duplicate event id");
        if (events[i].hour == event.hour && events[i].minute == event.minute) {
            return fail(error, "duplicate event time");
        }
    }
    events.push_back(event);
    return true;
}

bool updateEvent(Course& course, std::size_t dayIndex, std::uint32_t id,
                 const Event& event, std::string& error) {
    if (dayIndex >= course.days.size()) return fail(error, "day index out of range");
    if (!materialize(course, dayIndex, error)) return false;
    std::vector<Event>& events = course.days[dayIndex].events;
    Event updated = event;
    updated.id = id;
    if (!validateEvent(updated, error)) return false;
    for (std::size_t i = 0; i < events.size(); ++i) {
        if (events[i].id == id) {
            for (std::size_t j = 0; j < events.size(); ++j) {
                if (j == i) continue;
                if (events[j].hour == updated.hour && events[j].minute == updated.minute) {
                    return fail(error, "duplicate event time");
                }
            }
            events[i] = updated;
            return true;
        }
    }
    return fail(error, "event id not found");
}

bool removeEvent(Course& course, std::size_t dayIndex, std::uint32_t id,
                 std::string& error) {
    if (dayIndex >= course.days.size()) return fail(error, "day index out of range");
    if (!materialize(course, dayIndex, error)) return false;
    std::vector<Event>& events = course.days[dayIndex].events;
    for (std::size_t i = 0; i < events.size(); ++i) {
        if (events[i].id == id) {
            events.erase(events.begin() + static_cast<std::ptrdiff_t>(i));
            return true;
        }
    }
    return fail(error, "event id not found");
}

bool setEventEnabled(Course& course, std::size_t dayIndex, std::uint32_t id,
                     bool enabled, std::string& error) {
    if (dayIndex >= course.days.size()) return fail(error, "day index out of range");
    if (!materialize(course, dayIndex, error)) return false;
    std::vector<Event>& events = course.days[dayIndex].events;
    for (std::size_t i = 0; i < events.size(); ++i) {
        if (events[i].id == id) {
            events[i].enabled = enabled;
            return true;
        }
    }
    return fail(error, "event id not found");
}

bool fromJson(const std::string& json, Course& out, std::string& error) {
    JsonValue root;
    JsonParser parser(json);
    if (!parser.parse(root, error)) return false;
    if (root.type != JsonValue::OBJECT) return fail(error, "course JSON must be an object");
    if (!checkKeys(root,
                   {"format", "version", "name", "generation", "course_length",
                    "templates", "days"},
                   error)) {
        return false;
    }
    const JsonValue* format = root.find("format");
    if (!format) return fail(error, "missing format");
    std::string formatText;
    if (!readString(*format, "format", formatText, error)) return false;
    if (formatText != kFormat) return fail(error, "unsupported format");

    const JsonValue* versionValue = root.find("version");
    if (!versionValue) return fail(error, "missing version");
    long long version = 0;
    if (!readInt(*versionValue, "version", 1, 2, version, error)) return false;

    Course course;
    const JsonValue* name = root.find("name");
    if (name) {
        if (!readString(*name, "name", course.name, error)) return false;
    }
    const JsonValue* generation = root.find("generation");
    if (generation) {
        long long v = 0;
        if (!readInt(*generation, "generation", 0, 0xFFFFFFFFLL, v, error)) return false;
        course.generation = static_cast<std::uint32_t>(v);
    }

    const JsonValue* templates = root.find("templates");
    if (templates) {
        if (!parseTemplates(*templates, course.templates, error)) return false;
    }

    const JsonValue* days = root.find("days");
    if (!days) return fail(error, "missing days");
    if (days->type != JsonValue::ARRAY) return fail(error, "days must be an array");
    if (days->array.empty()) return fail(error, "course must contain at least one day");
    if (days->array.size() > kMaxDays) return fail(error, "course has too many days");
    for (std::size_t i = 0; i < days->array.size(); ++i) {
        Day day;
        if (version == 2) {
            if (!parseDayV2(days->array[i], day, error)) return false;
        } else {
            if (!parseDayV1(days->array[i], day, error)) return false;
        }
        course.days.push_back(day);
    }

    const JsonValue* length = root.find("course_length");
    if (length) {
        long long v = 0;
        if (!readInt(*length, "course_length", 1, static_cast<long long>(kMaxDays), v, error)) {
            return false;
        }
        if (static_cast<std::size_t>(v) != course.days.size()) {
            return fail(error, "course_length does not match days");
        }
    }

    if (!validate(course, error)) return false;
    out = course;
    return true;
}

bool formatVersion(const std::string& json, int& out, std::string& error) {
    JsonValue root;
    JsonParser parser(json);
    if (!parser.parse(root, error)) return false;
    if (root.type != JsonValue::OBJECT) return fail(error, "course JSON must be an object");
    const JsonValue* format = root.find("format");
    if (!format) return fail(error, "missing format");
    std::string formatText;
    if (!readString(*format, "format", formatText, error)) return false;
    if (formatText != kFormat) return fail(error, "unsupported format");
    const JsonValue* versionValue = root.find("version");
    if (!versionValue) return fail(error, "missing version");
    long long version = 0;
    if (!readInt(*versionValue, "version", 1, 2, version, error)) return false;
    out = static_cast<int>(version);
    return true;
}

std::string toJsonV2(const Course& course, bool expandTemplates,
                     std::string& error) {
    if (!validate(course, error)) return std::string();
    std::string out;
    out.reserve(512 + course.days.size() * 96);
    out += "{\"format\":\"";
    out += kFormat;
    out += "\",\"version\":2,\"name\":\"";
    out += escapeJson(course.name);
    out += "\",\"generation\":";
    out += std::to_string(course.generation);
    out += ",\"course_length\":";
    out += std::to_string(course.days.size());

    if (!expandTemplates && !course.templates.empty()) {
        out += ",\"templates\":{";
        bool first = true;
        for (std::map<std::string, std::vector<Event> >::const_iterator it =
                 course.templates.begin(); it != course.templates.end(); ++it) {
            if (!first) out += ',';
            first = false;
            out += '"';
            out += escapeJson(it->first);
            out += "\":";
            writeEventArray(out, it->second);
        }
        out += '}';
    }

    out += ",\"days\":[";
    for (std::size_t i = 0; i < course.days.size(); ++i) {
        if (i) out += ',';
        const Day& day = course.days[i];
        if (!expandTemplates && !day.hasOwnEvents && !day.templateName.empty()) {
            out += "{\"template\":\"";
            out += escapeJson(day.templateName);
            out += "\"}";
        } else {
            std::vector<Event> events;
            if (!resolvedEvents(course, i, events, error)) return std::string();
            out += "{\"events\":";
            writeEventArray(out, events);
            out += '}';
        }
    }
    out += "]}";
    return out;
}

Course makeReferenceCourse() {
    Course course;
    course.name = "14-day reference";
    course.generation = 1;

    Event arrival;
    arrival.id = 1;
    arrival.hour = 19;
    arrival.minute = 45;
    arrival.description = "Arrival";
    course.templates["arrival"].push_back(arrival);

    Event practice;
    practice.id = 1;
    practice.hour = 4;
    practice.minute = 0;
    practice.description = "Wake-up";
    course.templates["practice"].push_back(practice);

    Event departure;
    departure.id = 1;
    departure.hour = 12;
    departure.minute = 0;
    departure.description = "Departure";
    course.templates["departure"].push_back(departure);

    Event service;
    service.id = 1;
    service.hour = 7;
    service.minute = 0;
    service.description = "Service breakfast";
    course.templates["service"].push_back(service);

    Day arrivalDay;
    arrivalDay.templateName = "arrival";
    arrivalDay.hasOwnEvents = false;
    course.days.push_back(arrivalDay);

    for (int i = 0; i < 10; ++i) {
        Day practiceDay;
        practiceDay.templateName = "practice";
        practiceDay.hasOwnEvents = false;
        course.days.push_back(practiceDay);
    }

    Day departureDay;
    departureDay.templateName = "departure";
    departureDay.hasOwnEvents = false;
    course.days.push_back(departureDay);

    // Day 12 and day 13 are independent copies of the service schedule.
    Day serviceDay;
    serviceDay.hasOwnEvents = true;
    serviceDay.events = course.templates["service"];
    course.days.push_back(serviceDay);
    Day serviceDayCopy;
    serviceDayCopy.hasOwnEvents = true;
    serviceDayCopy.events = course.templates["service"];
    course.days.push_back(serviceDayCopy);

    return course;
}

}  // namespace course
