#include "course_editor.h"

#include <cstdio>
#include <cstring>
#include <map>
#include <new>
#include <string>
#include <utility>
#include <vector>

namespace course_editor {
namespace {

// Event JSON is emitted with the same keys the event editor and the v2 course
// format already use, so the GUI can feed it straight back into saveDay().
void appendEvent(std::string& out, const course::Event& event) {
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
    // Escape exactly like the course model does for v2 output.
    for (std::size_t i = 0; i < event.description.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(event.description[i]);
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buffer[8];
                    std::snprintf(buffer, sizeof(buffer), "\\u%04x", c);
                    out += buffer;
                } else {
                    out.push_back(static_cast<char>(c));
                }
        }
    }
    out += "\"}";
}

void appendEvents(std::string& out, const std::vector<course::Event>& events) {
    out += '[';
    for (std::size_t i = 0; i < events.size(); ++i) {
        if (i) out += ',';
        appendEvent(out, events[i]);
    }
    out += ']';
}

std::string trim(const std::string& text) {
    std::size_t first = 0;
    while (first < text.size() &&
           (text[first] == ' ' || text[first] == '\t' ||
            text[first] == '\r' || text[first] == '\n')) {
        ++first;
    }
    std::size_t last = text.size();
    while (last > first &&
           (text[last - 1] == ' ' || text[last - 1] == '\t' ||
            text[last - 1] == '\r' || text[last - 1] == '\n')) {
        --last;
    }
    return text.substr(first, last - first);
}

// ── Bounded JSON scanning ──────────────────────────────────────────────────
// The ESP32 has no PSRAM. Parsing a whole course document into a tree needs
// several hundred KB, so the input is scanned once here for the byte range of
// every day and template; each range is then wrapped in a tiny document and
// parsed by the course model on its own. Only one day exists in RAM at a time
// and the input buffer is never copied as a whole.

struct Slice {
    std::size_t begin;
    std::size_t length;
    Slice() : begin(0), length(0) {}
};

struct TemplateSlice {
    std::string name;
    Slice body;
};

struct ScannedCourse {
    std::string name;
    int version;
    bool hasCourseLength;
    long long courseLength;
    std::vector<Slice> days;
    std::vector<TemplateSlice> templates;

    ScannedCourse() : version(0), hasCourseLength(false), courseLength(0) {}
};

const std::size_t kMaxTemplates = 64;
// One day is parsed on its own, so its raw size bounds the working set. The
// model allows at most 32 events per day with 96-byte descriptions.
const std::size_t kMaxDaySliceBytes = 8 * 1024;
const std::size_t kMaxNameBytes = 128;
const std::size_t kMaxTemplateNameBytes = 32;
const int kMaxNesting = 32;

class Scanner {
public:
    Scanner(const char* data, std::size_t size) : data_(data), size_(size), pos_(0) {}

    bool scan(ScannedCourse& out, std::string& error);

    // v1 day that is a template name string.
    bool scanTemplateName(std::string& name, std::string& error) {
        if (!readString(&name, kMaxTemplateNameBytes, error)) return false;
        if (name.empty()) return fail(error, "template name must not be empty");
        skipWs();
        if (!atEnd()) return fail(error, "trailing data after the template name");
        return true;
    }

    // v2 day object: reports whether it references a template and its name.
    // "events" wins over "template", matching parseDayV2() in the model.
    bool scanDayObject(std::string& templateName, bool& isTemplate, std::string& error) {
        skipWs();
        if (atEnd() || data_[pos_] != '{') return fail(error, "v2 day must be an object");
        ++pos_;
        skipWs();
        bool sawEvents = false;
        bool sawTemplate = false;
        std::string name;
        if (peek() != '}') {
            for (;;) {
                std::string key;
                if (!readString(&key, 16, error)) return false;
                if (!expect(':', error)) return false;
                skipWs();
                if (key == "events") {
                    sawEvents = true;
                    if (!skipValue(1, error)) return false;
                } else if (key == "template") {
                    sawTemplate = true;
                    if (!readString(&name, kMaxTemplateNameBytes, error)) return false;
                    if (name.empty()) return fail(error, "template name must not be empty");
                } else {
                    return fail(error, "unknown field: " + key);
                }
                skipWs();
                if (peek() == ',') {
                    ++pos_;
                    skipWs();
                    continue;
                }
                break;
            }
        }
        if (!expect('}', error)) return false;
        skipWs();
        if (!atEnd()) return fail(error, "trailing data after the day object");
        isTemplate = sawTemplate && !sawEvents;
        templateName = name;
        return true;
    }

private:
    const char* data_;
    std::size_t size_;
    std::size_t pos_;

    bool atEnd() const { return pos_ >= size_; }
    char peek() const { return atEnd() ? '\0' : data_[pos_]; }

    void skipWs() {
        while (!atEnd()) {
            const char c = data_[pos_];
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
                ++pos_;
            } else {
                break;
            }
        }
    }

    bool fail(std::string& error, const std::string& what) const {
        error = what + " at byte " + std::to_string(pos_);
        return false;
    }

    bool expect(char c, std::string& error) {
        skipWs();
        if (atEnd() || data_[pos_] != c) {
            return fail(error, std::string("expected '") + c + "'");
        }
        ++pos_;
        return true;
    }

    // Reads a JSON string. When `out` is null the value is validated and
    // skipped. `cap` bounds the decoded size so a hostile file cannot make us
    // build an arbitrarily large string.
    bool readString(std::string* out, std::size_t cap, std::string& error) {
        skipWs();
        if (atEnd() || data_[pos_] != '"') return fail(error, "expected a string");
        ++pos_;
        std::size_t decoded = 0;
        while (true) {
            if (atEnd()) return fail(error, "unterminated string");
            const unsigned char c = static_cast<unsigned char>(data_[pos_++]);
            if (c == '"') return true;
            if (c < 0x20) return fail(error, "control character in string");
            if (c == '\\') {
                if (atEnd()) return fail(error, "unterminated escape");
                const char e = data_[pos_++];
                unsigned int code = 0;
                switch (e) {
                    case '"': code = '"'; break;
                    case '\\': code = '\\'; break;
                    case '/': code = '/'; break;
                    case 'b': code = '\b'; break;
                    case 'f': code = '\f'; break;
                    case 'n': code = '\n'; break;
                    case 'r': code = '\r'; break;
                    case 't': code = '\t'; break;
                    case 'u': {
                        if (size_ - pos_ < 4) return fail(error, "truncated \\u escape");
                        code = 0;
                        for (int i = 0; i < 4; ++i) {
                            const char h = data_[pos_++];
                            unsigned int digit = 0;
                            if (h >= '0' && h <= '9') digit = static_cast<unsigned int>(h - '0');
                            else if (h >= 'a' && h <= 'f') digit = static_cast<unsigned int>(h - 'a' + 10);
                            else if (h >= 'A' && h <= 'F') digit = static_cast<unsigned int>(h - 'A' + 10);
                            else return fail(error, "bad \\u escape");
                            code = code * 16 + digit;
                        }
                        break;
                    }
                    default:
                        return fail(error, "bad escape");
                }
                if (out) {
                    if (code < 0x80) {
                        if (++decoded > cap) return fail(error, "string is too long");
                        out->push_back(static_cast<char>(code));
                    } else if (code < 0x800) {
                        if ((decoded += 2) > cap) return fail(error, "string is too long");
                        out->push_back(static_cast<char>(0xC0 | (code >> 6)));
                        out->push_back(static_cast<char>(0x80 | (code & 0x3F)));
                    } else {
                        if ((decoded += 3) > cap) return fail(error, "string is too long");
                        out->push_back(static_cast<char>(0xE0 | (code >> 12)));
                        out->push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
                        out->push_back(static_cast<char>(0x80 | (code & 0x3F)));
                    }
                }
                continue;
            }
            if (out) {
                if (++decoded > cap) return fail(error, "string is too long");
                out->push_back(static_cast<char>(c));
            }
        }
    }

    bool readNumber(std::string& text, std::string& error) {
        skipWs();
        const std::size_t begin = pos_;
        if (!atEnd() && (data_[pos_] == '-' || data_[pos_] == '+')) ++pos_;
        bool digits = false;
        while (!atEnd() && data_[pos_] >= '0' && data_[pos_] <= '9') { ++pos_; digits = true; }
        if (!atEnd() && data_[pos_] == '.') {
            ++pos_;
            while (!atEnd() && data_[pos_] >= '0' && data_[pos_] <= '9') { ++pos_; digits = true; }
        }
        if (!atEnd() && (data_[pos_] == 'e' || data_[pos_] == 'E')) {
            ++pos_;
            if (!atEnd() && (data_[pos_] == '-' || data_[pos_] == '+')) ++pos_;
            while (!atEnd() && data_[pos_] >= '0' && data_[pos_] <= '9') { ++pos_; }
        }
        if (!digits) return fail(error, "expected a number");
        text.assign(data_ + begin, pos_ - begin);
        return true;
    }

    // Integer field with range check, matching readInt() in the course model.
    bool readInt(long long& out, long long min, long long max, const char* field,
                 std::string& error) {
        std::string text;
        if (!readNumber(text, error)) return false;
        if (text.find('.') != std::string::npos || text.find('e') != std::string::npos ||
            text.find('E') != std::string::npos) {
            return fail(error, std::string(field) + " must be an integer");
        }
        bool negative = false;
        std::size_t i = 0;
        if (text[0] == '-') { negative = true; i = 1; }
        else if (text[0] == '+') { i = 1; }
        long long value = 0;
        for (; i < text.size(); ++i) {
            if (value > (0x7FFFFFFFFFFFFFFFLL - 9) / 10) {
                return fail(error, std::string(field) + " is out of range");
            }
            value = value * 10 + (text[i] - '0');
        }
        if (negative) value = -value;
        if (value < min || value > max) {
            return fail(error, std::string(field) + " is out of range");
        }
        out = value;
        return true;
    }

    bool skipLiteral(const char* literal, std::string& error) {
        const std::size_t len = std::strlen(literal);
        if (size_ - pos_ < len || std::strncmp(data_ + pos_, literal, len) != 0) {
            return fail(error, "bad literal");
        }
        pos_ += len;
        return true;
    }

    bool skipValue(int depth, std::string& error) {
        if (depth > kMaxNesting) return fail(error, "JSON is nested too deeply");
        skipWs();
        if (atEnd()) return fail(error, "unexpected end of document");
        const char c = data_[pos_];
        if (c == '"') return readString(nullptr, 0, error);
        if (c == '{') {
            ++pos_;
            skipWs();
            if (peek() == '}') { ++pos_; return true; }
            for (;;) {
                if (!readString(nullptr, 0, error)) return false;
                if (!expect(':', error)) return false;
                if (!skipValue(depth + 1, error)) return false;
                skipWs();
                if (peek() == ',') { ++pos_; continue; }
                break;
            }
            return expect('}', error);
        }
        if (c == '[') {
            ++pos_;
            skipWs();
            if (peek() == ']') { ++pos_; return true; }
            for (;;) {
                if (!skipValue(depth + 1, error)) return false;
                skipWs();
                if (peek() == ',') { ++pos_; continue; }
                break;
            }
            return expect(']', error);
        }
        if (c == 't') return skipLiteral("true", error);
        if (c == 'f') return skipLiteral("false", error);
        if (c == 'n') return skipLiteral("null", error);
        std::string number;
        return readNumber(number, error);
    }

    bool scanTemplates(ScannedCourse& out, std::string& error) {
        if (!expect('{', error)) return false;
        skipWs();
        if (peek() == '}') { ++pos_; return true; }
        for (;;) {
            TemplateSlice entry;
            if (!readString(&entry.name, kMaxTemplateNameBytes, error)) return false;
            if (entry.name.empty()) return fail(error, "template name must not be empty");
            if (!expect(':', error)) return false;
            skipWs();
            entry.body.begin = pos_;
            if (!skipValue(1, error)) return false;
            entry.body.length = pos_ - entry.body.begin;
            if (out.templates.size() >= kMaxTemplates) {
                return fail(error, "too many templates");
            }
            out.templates.push_back(entry);
            skipWs();
            if (peek() == ',') { ++pos_; continue; }
            break;
        }
        return expect('}', error);
    }

    bool scanDays(ScannedCourse& out, std::string& error) {
        if (!expect('[', error)) return false;
        skipWs();
        if (peek() == ']') { ++pos_; return true; }
        for (;;) {
            Slice slice;
            skipWs();
            slice.begin = pos_;
            if (!skipValue(1, error)) return false;
            slice.length = pos_ - slice.begin;
            if (out.days.size() >= course::kMaxDays) {
                return fail(error, "course has too many days");
            }
            if (slice.length > kMaxDaySliceBytes) {
                return fail(error, "day " + std::to_string(out.days.size()) +
                                       " is too large (max 8 KB)");
            }
            out.days.push_back(slice);
            skipWs();
            if (peek() == ',') { ++pos_; continue; }
            break;
        }
        return expect(']', error);
    }
};

bool Scanner::scan(ScannedCourse& out, std::string& error) {
    skipWs();
    if (!expect('{', error)) return false;
    skipWs();
    bool sawFormat = false;
    bool sawDays = false;
    if (peek() != '}') {
        for (;;) {
            std::string key;
            if (!readString(&key, 18, error)) return false;
            if (!expect(':', error)) return false;
            skipWs();
            if (key == "format") {
                std::string format;
                if (!readString(&format, 16, error)) return false;
                if (format != "gong-course") return fail(error, "unsupported format");
                sawFormat = true;
            } else if (key == "version") {
                long long version = 0;
                if (!readInt(version, 1, 2, "version", error)) return false;
                if (out.version == 0) out.version = static_cast<int>(version);
            } else if (key == "name") {
                std::string name;
                if (!readString(&name, kMaxNameBytes, error)) return false;
                if (out.name.empty()) out.name = name;
            } else if (key == "generation") {
                long long generation = 0;
                if (!readInt(generation, 0, 0xFFFFFFFFLL, "generation", error)) return false;
            } else if (key == "course_length") {
                long long length = 0;
                if (!readInt(length, 1, static_cast<long long>(course::kMaxDays),
                             "course_length", error)) {
                    return false;
                }
                if (!out.hasCourseLength) {
                    out.hasCourseLength = true;
                    out.courseLength = length;
                }
            } else if (key == "templates") {
                if (!out.templates.empty()) {
                    if (!skipValue(1, error)) return false;
                } else if (!scanTemplates(out, error)) {
                    return false;
                }
            } else if (key == "days") {
                if (sawDays) {
                    if (!skipValue(1, error)) return false;
                } else if (!scanDays(out, error)) {
                    return false;
                }
                sawDays = true;
            } else {
                return fail(error, "unknown field: " + key);
            }
            skipWs();
            if (peek() == ',') {
                ++pos_;
                skipWs();
                if (peek() == '}') return fail(error, "trailing comma");
                continue;
            }
            break;
        }
    }
    if (!expect('}', error)) return false;
    skipWs();
    if (!atEnd()) return fail(error, "trailing data after the document");

    if (!sawFormat) return fail(error, "missing format");
    if (out.version == 0) return fail(error, "missing version");
    if (!sawDays || out.days.empty()) return fail(error, "course must contain at least one day");
    if (out.hasCourseLength && static_cast<std::size_t>(out.courseLength) != out.days.size()) {
        return fail(error, "course_length does not match days");
    }
    return true;
}

std::string escapeJsonText(const std::string& text) {
    std::string out;
    out.reserve(text.size() + 4);
    for (std::size_t i = 0; i < text.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        if (c == '"' || c == '\\') {
            out.push_back('\\');
            out.push_back(static_cast<char>(c));
        } else if (c < 0x20) {
            out += "\\u00";
            static const char* hex = "0123456789abcdef";
            out.push_back(hex[(c >> 4) & 0x0F]);
            out.push_back(hex[c & 0x0F]);
        } else {
            out.push_back(static_cast<char>(c));
        }
    }
    return out;
}

// Classifies a day slice and, for a template reference, returns its name.
bool classifyDay(const char* data, const ScannedCourse& doc, std::size_t index,
                 bool& isTemplate, std::string& templateName, std::string& error) {
    const Slice& slice = doc.days[index];
    std::size_t pos = slice.begin;
    const std::size_t end = slice.begin + slice.length;
    while (pos < end && (data[pos] == ' ' || data[pos] == '\t' || data[pos] == '\r' ||
                         data[pos] == '\n')) {
        ++pos;
    }
    if (pos >= end) {
        error = "day payload is empty";
        return false;
    }
    const char first = data[pos];
    if (doc.version == 1) {
        if (first == '[') {
            isTemplate = false;
            return true;
        }
        if (first != '"') {
            error = "v1 day must be a template name or an event array";
            return false;
        }
        isTemplate = true;
        Scanner reader(data + slice.begin, slice.length);
        return reader.scanTemplateName(templateName, error);
    }
    if (first != '{') {
        error = "v2 day must be an object";
        return false;
    }
    Scanner reader(data + slice.begin, slice.length);
    return reader.scanDayObject(templateName, isTemplate, error);
}

// Wraps one raw slice into the smallest valid course document and parses it
// with the course model. Peak memory is one day, not the whole course.
bool parseDaySlice(const char* data, const ScannedCourse& doc, std::size_t index,
                   course::Day& out, std::string& error) {
    bool isTemplate = false;
    std::string templateName;
    std::string kindError;
    if (!classifyDay(data, doc, index, isTemplate, templateName, kindError)) {
        error = "day " + std::to_string(index) + ": " + kindError;
        return false;
    }
    const Slice& slice = doc.days[index];
    const TemplateSlice* tmpl = nullptr;
    if (isTemplate) {
        for (std::size_t i = 0; i < doc.templates.size(); ++i) {
            if (doc.templates[i].name == templateName) { tmpl = &doc.templates[i]; break; }
        }
        if (!tmpl) {
            error = "day " + std::to_string(index) + ": unknown template: " + templateName;
            return false;
        }
    }

    std::string wrapped;
    wrapped.reserve(slice.length + (tmpl ? tmpl->body.length : 0) + 192);
    wrapped += "{\"format\":\"gong-course\",\"version\":";
    wrapped += std::to_string(doc.version);
    wrapped += ",\"templates\":{";
    if (tmpl) {
        wrapped += '"';
        wrapped += escapeJsonText(tmpl->name);
        wrapped += "\":";
        wrapped.append(data + tmpl->body.begin, tmpl->body.length);
    }
    wrapped += "},\"days\":[";
    wrapped.append(data + slice.begin, slice.length);
    wrapped += "]}";

    course::Course one;
    std::string parseError;
    if (!course::fromJson(wrapped, one, parseError)) {
        error = "day " + std::to_string(index) + ": " + parseError;
        return false;
    }
    if (one.days.size() != 1) {
        error = "day " + std::to_string(index) + ": unexpected payload";
        return false;
    }
    // Resolve before moving: resolvedEvents() reads one.days[0].
    std::vector<course::Event> events;
    if (!course::resolvedEvents(one, 0, events, parseError)) {
        error = "day " + std::to_string(index) + ": " + parseError;
        return false;
    }
    out = std::move(one.days[0]);
    out.hasOwnEvents = true;
    out.templateName.clear();
    out.events = std::move(events);
    return true;
}

// Validates a template that may never be referenced, so a broken template is
// still reported instead of being silently ignored.
bool validateTemplate(const char* data, const ScannedCourse& doc, std::size_t index,
                      std::string& error) {
    const TemplateSlice& tmpl = doc.templates[index];
    std::string wrapped;
    wrapped.reserve(tmpl.body.length + 160);
    wrapped += "{\"format\":\"gong-course\",\"version\":2,\"templates\":{\"";
    wrapped += escapeJsonText(tmpl.name);
    wrapped += "\":";
    wrapped.append(data + tmpl.body.begin, tmpl.body.length);
    wrapped += "},\"days\":[{\"template\":\"";
    wrapped += escapeJsonText(tmpl.name);
    wrapped += "\"}]}";
    course::Course check;
    std::string parseError;
    if (!course::fromJson(wrapped, check, parseError)) {
        error = "template " + tmpl.name + ": " + parseError;
        return false;
    }
    return true;
}

// Full bounded validation of a course document.
bool scanCourse(const char* data, std::size_t size, ScannedCourse& doc,
                std::string& error) {
    Scanner scanner(data, size);
    if (!scanner.scan(doc, error)) return false;
    for (std::size_t i = 0; i < doc.templates.size(); ++i) {
        if (!validateTemplate(data, doc, i, error)) return false;
    }
    return true;
}

bool loadCalendar(course_store::CalendarState& cal, std::string& error) {
    course_store::CourseStore store;
    if (!store.hasCalendar()) {
        error = "no course has been published";
        return false;
    }
    return store.loadCalendar(cal, error);
}

// The generation the operator is editing: the private draft when one exists,
// otherwise the active generation itself (read-only until the first edit).
bool readGeneration(course_store::CalendarState& cal, std::uint32_t& generation,
                    std::string& error) {
    course_store::CourseStore store;
    if (!loadCalendar(cal, error)) return false;
    if (cal.active_generation == 0) {
        error = "no active generation";
        return false;
    }
    if (cal.draft_generation != 0 &&
        cal.draft_generation != cal.active_generation &&
        store.generationExists(cal.draft_generation)) {
        generation = cal.draft_generation;
        return true;
    }
    generation = cal.active_generation;
    return true;
}

// Copy-on-write: the active generation is never edited in place. The first
// draft write clones it and moves the NVS draft pointer to the clone.
bool ensureDraft(course_store::CalendarState& cal, std::uint32_t& generation,
                 std::string& error) {
    course_store::CourseStore store;
    if (!loadCalendar(cal, error)) return false;
    if (cal.active_generation == 0) {
        error = "no active generation";
        return false;
    }
    if (cal.draft_generation != 0 &&
        cal.draft_generation != cal.active_generation &&
        store.generationExists(cal.draft_generation)) {
        generation = cal.draft_generation;
        return true;
    }
    std::uint32_t cloned = 0;
    if (!store.cloneGeneration(cal.active_generation, cloned, error)) return false;
    course_store::CalendarState next = cal;
    next.draft_generation = cloned;
    if (!store.saveCalendar(next, error)) {
        std::string ignored;
        store.deleteGeneration(cloned, ignored);
        return false;
    }
    cal = next;
    generation = cloned;
    return true;
}

// Commits a freshly built generation as the draft. The active generation and
// its calendar binding are untouched.
bool commitDraft(course_store::CalendarState& cal, std::uint32_t built,
                 std::uint32_t previousDraft, std::string& error) {
    course_store::CourseStore store;
    course_store::CalendarState next = cal;
    next.draft_generation = built;
    if (!store.saveCalendar(next, error)) {
        std::string ignored;
        store.deleteGeneration(built, ignored);
        return false;
    }
    cal = next;
    if (previousDraft != 0 && previousDraft != cal.active_generation &&
        previousDraft != built) {
        std::string ignored;
        store.deleteGeneration(previousDraft, ignored);
    }
    return true;
}

bool readMeta(std::uint32_t generation, course_store::CourseMeta& meta,
              std::string& error) {
    course_store::CourseStore store;
    return store.loadMeta(generation, meta, error);
}

// The generation the operator is editing. With a published course that is its
// private draft; before the first Apply it is the bootstrap draft recorded in
// its own NVS key, because course_state does not exist yet.
bool editingGeneration(std::uint32_t& generation, bool& published,
                       std::string& error) {
    error.clear();
    course_store::CourseStore store;
    if (!store.hasCalendar()) {
        published = false;
        const std::uint32_t bootstrap = store.loadBootstrapGeneration();
        if (bootstrap == 0 || !store.generationExists(bootstrap)) {
            error = "no course has been published";
            return false;
        }
        generation = bootstrap;
        // A template draft cannot be read without its own template set.
        return store.prepareTemplates(generation, error);
    }
    published = true;
    course_store::CalendarState cal;
    if (!loadCalendar(cal, error)) return false;
    if (!readGeneration(cal, generation, error)) return false;
    return store.prepareTemplates(generation, error);
}

// A generation that may be written to. With a published course that is always
// a private draft generation (copy-on-write on the first edit), so the active
// generation is never modified in place. Before the first Apply the bootstrap
// generation is already private and is used as is.
bool ensureEditableGeneration(course_store::CalendarState& cal,
                              std::uint32_t& generation, bool& published,
                              std::string& error) {
    error.clear();
    course_store::CourseStore store;
    if (store.hasCalendar()) {
        published = true;
        if (!ensureDraft(cal, generation, error)) return false;
        // ensureDraft() may have cloned the active generation; a template
        // generation is only editable once its own template set is loaded.
        return store.prepareTemplates(generation, error);
    }
    return editingGeneration(generation, published, error);
}

// Publishes a freshly built generation as the bootstrap draft. The NVS pointer
// is written last, so an interrupted build leaves only an unreferenced
// generation and no calendar at all.
bool commitBootstrap(std::uint32_t built, std::uint32_t previous,
                     std::string& error) {
    course_store::CourseStore store;
    if (!store.saveBootstrapGeneration(built, error)) {
        std::string ignored;
        store.deleteGeneration(built, ignored);
        return false;
    }
    if (previous != 0 && previous != built) {
        std::string ignored;
        store.deleteGeneration(previous, ignored);
    }
    return true;
}

// Builds a complete generation from a resolved course and publishes its meta
// last. Used by the presets and by import, both before and after the first
// Apply. Returns false with any partially written generation removed.
bool buildGeneration(const course::Course& source, std::uint32_t& built,
                     std::string& error) {
    course_store::CourseStore store;
    if (!store.createGeneration(built, error)) return false;
    const std::uint8_t length = static_cast<std::uint8_t>(source.days.size());
    for (std::uint8_t i = 0; i < length; ++i) {
        course::Day dayData;
        dayData.hasOwnEvents = true;
        if (!course::resolvedEvents(source, i, dayData.events, error)) {
            std::string ignored;
            store.deleteGeneration(built, ignored);
            return false;
        }
        if (!store.saveDayAtomic(built, i, dayData, error)) {
            std::string ignored;
            store.deleteGeneration(built, ignored);
            return false;
        }
    }
    course_store::CourseMeta meta;
    meta.generation = built;
    meta.course_length = length;
    meta.name = source.name;
    if (!store.saveMeta(meta, error)) {
        std::string ignored;
        store.deleteGeneration(built, ignored);
        return false;
    }
    if (!store.validateGeneration(built, length, error)) {
        std::string ignored;
        store.deleteGeneration(built, ignored);
        return false;
    }
    return true;
}

// Either commits the built generation as a published draft or as the bootstrap
// draft, depending on whether a course has been published already.
bool commitEditingGeneration(bool published, std::uint32_t built,
                             course_store::CalendarState& cal,
                             std::uint32_t source, std::string& error) {
    if (published) return commitDraft(cal, built, source, error);
    return commitBootstrap(built, source, error);
}

// Builds an empty day (no events) for presets and "insert without copy".
course::Day emptyDay() {
    course::Day day;
    day.hasOwnEvents = true;
    return day;
}

// Streaming export state. The Web layer pulls one chunk at a time so the whole
// 47-day course never has to exist as one string.
struct ExportState {
    bool active;
    // layout = kLayoutTemplates: emit the compact v1 form (unique templates plus
    // day references) instead of the expanded v2 days.
    bool compact;
    // 0 = header not sent yet, 1 = streaming templates, 2 = streaming days,
    // 3 = closing chunk.
    std::uint8_t phase;
    std::uint32_t generation;
    std::uint8_t length;
    std::uint8_t next;      // expanded: day index; compact: template fragments sent
    std::uint8_t day;       // compact: day index
    std::string name;
    std::vector<std::string> templateNames;

    ExportState() : active(false), compact(false), phase(0), generation(0), length(0),
                    next(0), day(0) {}
};

ExportState& exportState() {
    static ExportState state;
    return state;
}

}  // namespace

// Internal, bounded implementations. Declared here because the public wrappers
// below (which convert a bad_alloc into a normal error) call them first.
static bool inspectCourseBody(const char* data, std::size_t size, Inspection& out,
                              std::string& error);
static bool importCourseBody(const char* data, std::size_t size, std::string& error);

Inspection::Inspection()
    : length(0), events(0), version(0) {}

bool inspectCourse(const char* data, std::size_t size, Inspection& out,
                   std::string& error) {
    error.clear();
    out = Inspection();
    // A device without PSRAM can run out of heap on a hostile file; that must
    // become a normal validation error, never an uncaught bad_alloc.
    try {
        return inspectCourseBody(data, size, out, error);
    } catch (const std::bad_alloc&) {
        error = "not enough memory to validate this course";
        return false;
    } catch (...) {
        error = "validation failed";
        return false;
    }
}

static bool inspectCourseBody(const char* data, std::size_t size, Inspection& out,
                              std::string& error) {
    error.clear();
    out = Inspection();
    ScannedCourse doc;
    if (!scanCourse(data, size, doc, error)) return false;
    std::uint32_t events = 0;
    for (std::size_t i = 0; i < doc.days.size(); ++i) {
        course::Day day;
        if (!parseDaySlice(data, doc, i, day, error)) return false;
        events += static_cast<std::uint32_t>(day.events.size());
    }
    if (events > 0xFFFF) {
        error = "course has too many events";
        return false;
    }
    out.name = doc.name;
    out.length = static_cast<std::uint8_t>(doc.days.size());
    out.events = static_cast<std::uint16_t>(events);
    out.version = static_cast<std::uint8_t>(doc.version);
    return true;
}

bool inspectCourse(const std::string& json, Inspection& out, std::string& error) {
    return inspectCourse(json.data(), json.size(), out, error);
}

Info::Info()
    : hasCourse(false),
      activeGeneration(0),
      draftGeneration(0),
      activationDate(0),
      length(0),
      flags(0),
      failureCount(0),
      draftDirty(false) {}

bool info(Info& out, std::string& error) {
    error.clear();
    course_store::CourseStore store;
    if (!store.hasCalendar()) {
        // No published course yet. A bootstrap draft may still exist; report
        // it so the editor can be used before the first Apply.
        out = Info();
        const std::uint32_t bootstrap = store.loadBootstrapGeneration();
        if (bootstrap == 0 || !store.generationExists(bootstrap)) return true;
        course_store::CourseMeta meta;
        if (!readMeta(bootstrap, meta, error)) return false;
        out.draftGeneration = bootstrap;
        out.length = meta.course_length;
        out.name = meta.name;
        out.draftDirty = true;
        return true;
    }
    course_store::CalendarState cal;
    if (!store.loadCalendar(cal, error)) return false;
    out.hasCourse = true;
    out.activeGeneration = cal.active_generation;
    out.draftGeneration = cal.draft_generation;
    out.activationDate = cal.activation_date;
    out.length = cal.course_length;
    out.flags = cal.flags;
    out.failureCount = cal.failure_count;
    out.draftDirty = cal.draft_generation != 0 &&
                     cal.draft_generation != cal.active_generation;
    std::uint32_t generation = 0;
    if (!readGeneration(cal, generation, error)) return false;
    course_store::CourseMeta meta;
    if (!readMeta(generation, meta, error)) return false;
    out.name = meta.name;
    out.length = meta.course_length;
    return true;
}

bool dayJson(std::uint8_t day, std::string& out, std::string& error) {
    error.clear();
    std::uint32_t generation = 0;
    bool published = false;
    if (!editingGeneration(generation, published, error)) return false;
    course_store::CourseMeta meta;
    if (!readMeta(generation, meta, error)) return false;
    if (day >= meta.course_length) {
        error = "day index out of range";
        return false;
    }
    course::Day dayData;
    course_store::CourseStore store;
    if (!store.loadDay(generation, day, dayData, error)) return false;
    out.clear();
    appendEvents(out, dayData.events);
    return true;
}

bool saveDay(std::uint8_t day, const std::string& json, std::string& error) {
    error.clear();
    course_store::CalendarState cal;
    std::uint32_t generation = 0;
    bool published = false;
    if (!ensureEditableGeneration(cal, generation, published, error)) return false;
    course_store::CourseMeta meta;
    if (!readMeta(generation, meta, error)) return false;
    if (day >= meta.course_length) {
        error = "day index out of range";
        return false;
    }

    // Accept either a bare event array or a {"events":[...]} day object and
    // validate it through the course model's own v2 parser.
    const std::string trimmed = trim(json);
    if (trimmed.empty()) {
        error = "empty day payload";
        return false;
    }
    std::string eventsJson;
    if (trimmed[0] == '[') {
        eventsJson = trimmed;
    } else if (trimmed[0] == '{') {
        eventsJson = trimmed;
    } else {
        error = "day payload must be an array or object";
        return false;
    }
    std::string wrapped = "{\"format\":\"gong-course\",\"version\":2,";
    wrapped += "\"name\":\"day\",\"course_length\":1,\"days\":[";
    if (trimmed[0] == '[') {
        wrapped += "{\"events\":";
        wrapped += eventsJson;
        wrapped += "}";
    } else {
        wrapped += eventsJson;
    }
    wrapped += "]}";

    course::Course parsed;
    std::string parseError;
    if (!course::fromJson(wrapped, parsed, parseError)) {
        error = "invalid day: " + parseError;
        return false;
    }
    if (parsed.days.size() != 1) {
        error = "invalid day payload";
        return false;
    }
    course::Day dayData = parsed.days[0];
    dayData.hasOwnEvents = true;
    dayData.templateName.clear();
    course_store::CourseStore store;
    if (!store.saveDayAtomic(generation, day, dayData, error)) return false;
    error.clear();
    return true;
}

bool clearDay(std::uint8_t day, std::string& error) {
    course_store::CalendarState cal;
    std::uint32_t generation = 0;
    bool published = false;
    if (!ensureEditableGeneration(cal, generation, published, error)) return false;
    course_store::CourseMeta meta;
    if (!readMeta(generation, meta, error)) return false;
    if (day >= meta.course_length) {
        error = "day index out of range";
        return false;
    }
    course_store::CourseStore store;
    return store.saveDayAtomic(generation, day, emptyDay(), error);
}

bool insertDay(std::uint8_t day, bool copyEvents, std::string& error) {
    error.clear();
    course_store::CalendarState cal;
    std::uint32_t source = 0;
    bool published = false;
    if (!ensureEditableGeneration(cal, source, published, error)) return false;
    course_store::CourseMeta meta;
    if (!readMeta(source, meta, error)) return false;
    if (day > meta.course_length) {
        error = "day index out of range";
        return false;
    }
    if (meta.course_length >= course::kMaxDays) {
        error = "course is already at the maximum length";
        return false;
    }

    course_store::CourseStore store;
    std::uint32_t built = 0;
    if (!store.createGeneration(built, error)) return false;

    // Copy the affected days one at a time: only one day is in RAM.
    for (std::uint8_t i = 0; i < meta.course_length; ++i) {
        course::Day dayData;
        if (!store.loadDay(source, i, dayData, error)) {
            std::string ignored;
            store.deleteGeneration(built, ignored);
            return false;
        }
        const std::uint8_t target = i < day ? i : static_cast<std::uint8_t>(i + 1);
        if (!store.saveDayAtomic(built, target, dayData, error)) {
            std::string ignored;
            store.deleteGeneration(built, ignored);
            return false;
        }
    }

    course::Day inserted;
    if (copyEvents && day < meta.course_length) {
        if (!store.loadDay(source, day, inserted, error)) {
            std::string ignored;
            store.deleteGeneration(built, ignored);
            return false;
        }
    } else {
        inserted = emptyDay();
    }
    if (!store.saveDayAtomic(built, day, inserted, error)) {
        std::string ignored;
        store.deleteGeneration(built, ignored);
        return false;
    }

    course_store::CourseMeta nextMeta = meta;
    nextMeta.generation = built;
    nextMeta.course_length = static_cast<std::uint8_t>(meta.course_length + 1);
    if (!store.saveMeta(nextMeta, error)) {
        std::string ignored;
        store.deleteGeneration(built, ignored);
        return false;
    }
    if (!store.validateGeneration(built, nextMeta.course_length, error)) {
        std::string ignored;
        store.deleteGeneration(built, ignored);
        return false;
    }
    return commitEditingGeneration(published, built, cal, source, error);
}

bool deleteDay(std::uint8_t day, std::string& error) {
    error.clear();
    course_store::CalendarState cal;
    std::uint32_t source = 0;
    bool published = false;
    if (!ensureEditableGeneration(cal, source, published, error)) return false;
    course_store::CourseMeta meta;
    if (!readMeta(source, meta, error)) return false;
    if (day >= meta.course_length) {
        error = "day index out of range";
        return false;
    }
    if (meta.course_length <= 1) {
        error = "cannot delete the last remaining day";
        return false;
    }

    course_store::CourseStore store;
    std::uint32_t built = 0;
    if (!store.createGeneration(built, error)) return false;
    for (std::uint8_t i = 0; i < meta.course_length; ++i) {
        if (i == day) continue;
        course::Day dayData;
        if (!store.loadDay(source, i, dayData, error)) {
            std::string ignored;
            store.deleteGeneration(built, ignored);
            return false;
        }
        const std::uint8_t target = i < day ? i : static_cast<std::uint8_t>(i - 1);
        if (!store.saveDayAtomic(built, target, dayData, error)) {
            std::string ignored;
            store.deleteGeneration(built, ignored);
            return false;
        }
    }
    course_store::CourseMeta nextMeta = meta;
    nextMeta.generation = built;
    nextMeta.course_length = static_cast<std::uint8_t>(meta.course_length - 1);
    if (!store.saveMeta(nextMeta, error)) {
        std::string ignored;
        store.deleteGeneration(built, ignored);
        return false;
    }
    if (!store.validateGeneration(built, nextMeta.course_length, error)) {
        std::string ignored;
        store.deleteGeneration(built, ignored);
        return false;
    }
    return commitEditingGeneration(published, built, cal, source, error);
}

bool setPreset(std::uint8_t days, std::string& error) {
    error.clear();
    if (days < 1 || days > course::kMaxDays) {
        error = "course length out of range";
        return false;
    }
    course_store::CourseStore store;
    const bool published = store.hasCalendar();
    course_store::CalendarState cal;
    std::uint32_t source = 0;
    if (published) {
        if (!ensureDraft(cal, source, error)) return false;
    } else {
        // Nothing to inherit: the preset becomes the bootstrap draft and no
        // calendar is written until Apply.
        source = store.loadBootstrapGeneration();
        if (source != 0 && !store.generationExists(source)) source = 0;
    }

    course::Course preset;
    if (days == 14) {
        preset = course::makeReferenceCourse();
    } else {
        preset.name = "Course " + std::to_string(static_cast<int>(days)) + " days";
        preset.generation = 0;
        for (std::uint8_t i = 0; i < days; ++i) {
            preset.days.push_back(emptyDay());
        }
    }
    if (!course::validate(preset, error)) return false;

    std::uint32_t built = 0;
    if (!buildGeneration(preset, built, error)) return false;
    return commitEditingGeneration(published, built, cal, source, error);
}

bool renameCourse(const std::string& name, std::string& error) {
    error.clear();
    course_store::CalendarState cal;
    std::uint32_t generation = 0;
    bool published = false;
    if (!ensureEditableGeneration(cal, generation, published, error)) return false;
    course_store::CourseMeta meta;
    if (!readMeta(generation, meta, error)) return false;
    meta.name = name;
    course_store::CourseStore store;
    return store.saveMeta(meta, error);
}

bool importCourse(const char* data, std::size_t size, std::string& error) {
    error.clear();
    try {
        return importCourseBody(data, size, error);
    } catch (const std::bad_alloc&) {
        // Any partially written generation is unreferenced: the calendar was
        // not touched, so the published course is still intact.
        error = "not enough memory to import this course";
        return false;
    } catch (...) {
        error = "import failed";
        return false;
    }
}

static bool importCourseBody(const char* data, std::size_t size, std::string& error) {
    error.clear();
    // Scan and validate first: nothing is written if the file is malformed.
    ScannedCourse doc;
    if (!scanCourse(data, size, doc, error)) return false;

    course_store::CourseStore store;
    const bool published = store.hasCalendar();
    course_store::CalendarState cal;
    std::uint32_t source = 0;
    if (published) {
        if (!ensureDraft(cal, source, error)) return false;
    } else {
        // A clean install: the imported course becomes the bootstrap draft.
        source = store.loadBootstrapGeneration();
        if (source != 0 && !store.generationExists(source)) source = 0;
    }

    // Each day is parsed and written on its own: the whole document is never
    // materialised as a tree, so an interrupted import leaves an unreferenced
    // generation and the published course untouched.
    std::uint32_t built = 0;
    if (!store.createGeneration(built, error)) return false;
    const std::uint8_t length = static_cast<std::uint8_t>(doc.days.size());
    for (std::uint8_t i = 0; i < length; ++i) {
        course::Day dayData;
        if (!parseDaySlice(data, doc, i, dayData, error)) {
            std::string ignored;
            store.deleteGeneration(built, ignored);
            return false;
        }
        if (!store.saveDayAtomic(built, i, dayData, error)) {
            std::string ignored;
            store.deleteGeneration(built, ignored);
            return false;
        }
    }
    course_store::CourseMeta meta;
    meta.generation = built;
    meta.course_length = length;
    meta.name = doc.name;
    if (!store.saveMeta(meta, error)) {
        std::string ignored;
        store.deleteGeneration(built, ignored);
        return false;
    }
    if (!store.validateGeneration(built, length, error)) {
        std::string ignored;
        store.deleteGeneration(built, ignored);
        return false;
    }
    return commitEditingGeneration(published, built, cal, source, error);
}

// Parses one template body into its event list. Same wrapper trick as
// parseDaySlice(): only this template exists in RAM.
static bool parseTemplateBody(const char* data, const ScannedCourse& doc,
                              std::size_t index, std::vector<course::Event>& out,
                              std::string& error) {
    const TemplateSlice& tmpl = doc.templates[index];
    std::string wrapped;
    wrapped.reserve(tmpl.body.length + 160);
    wrapped += "{\"format\":\"gong-course\",\"version\":2,\"templates\":{\"";
    wrapped += escapeJsonText(tmpl.name);
    wrapped += "\":";
    wrapped.append(data + tmpl.body.begin, tmpl.body.length);
    wrapped += "},\"days\":[{\"template\":\"";
    wrapped += escapeJsonText(tmpl.name);
    wrapped += "\"}]}";
    course::Course one;
    std::string parseError;
    if (!course::fromJson(wrapped, one, parseError)) {
        error = "template " + tmpl.name + ": " + parseError;
        return false;
    }
    std::map<std::string, std::vector<course::Event> >::const_iterator it =
        one.templates.find(tmpl.name);
    if (it == one.templates.end()) {
        error = "template " + tmpl.name + ": missing after parse";
        return false;
    }
    out = it->second;
    return true;
}

// Stage B: stores the referenced templates once and each day as a reference.
//
// NOT WIRED INTO THE HTTP HANDLERS ON PURPOSE. A template generation cannot be
// executed until stage E loads the template cache at boot / Apply / Activate,
// so the operational import (importCourseBody) keeps writing expanded days.
static bool importCompactBody(const char* data, std::size_t size, std::string& error) {
    error.clear();
    ScannedCourse doc;
    if (!scanCourse(data, size, doc, error)) return false;

    std::map<std::string, std::vector<course::Event> > used;
    std::vector<bool> isReference(doc.days.size(), false);
    std::vector<std::string> referenceName(doc.days.size());
    std::size_t uniqueEvents = 0;
    for (std::size_t i = 0; i < doc.days.size(); ++i) {
        bool reference = false;
        std::string name;
        if (!classifyDay(data, doc, i, reference, name, error)) return false;
        isReference[i] = reference;
        referenceName[i] = name;
        if (!reference) continue;
        if (used.find(name) != used.end()) continue;  // occurrences share one copy
        std::size_t templateIndex = 0;
        bool found = false;
        for (std::size_t t = 0; t < doc.templates.size(); ++t) {
            if (doc.templates[t].name == name) {
                templateIndex = t;
                found = true;
                break;
            }
        }
        if (!found) {
            error = "day " + std::to_string(i) + ": unknown template: " + name;
            return false;
        }
        std::vector<course::Event> events;
        if (!parseTemplateBody(data, doc, templateIndex, events, error)) return false;
        if (used.size() >= course_store::CourseStore::kMaxTemplates) {
            error = "too many templates";
            return false;
        }
        uniqueEvents += events.size();
        if (uniqueEvents > course_store::CourseStore::kMaxTemplateEvents) {
            error = "too many template events";
            return false;
        }
        used[name] = events;
    }
    if (used.empty()) {
        error = "course has no template references";
        return false;
    }

    course_store::CourseStore store;
    const bool published = store.hasCalendar();
    course_store::CalendarState cal;
    std::uint32_t source = 0;
    if (published) {
        if (!ensureDraft(cal, source, error)) return false;
    } else {
        source = store.loadBootstrapGeneration();
        if (source != 0 && !store.generationExists(source)) source = 0;
    }

    std::uint32_t built = 0;
    if (!store.createGeneration(built, error)) return false;
    std::string ignored;
    if (!store.writeTemplates(built, used, error)) {
        store.deleteGeneration(built, ignored);
        return false;
    }
    for (std::size_t i = 0; i < doc.days.size(); ++i) {
        const std::uint8_t index = static_cast<std::uint8_t>(i);
        if (isReference[i]) {
            if (!store.putDayReference(built, index, referenceName[i], error)) {
                store.deleteGeneration(built, ignored);
                return false;
            }
            continue;
        }
        course::Day dayData;
        if (!parseDaySlice(data, doc, i, dayData, error) ||
            !store.saveDayAtomic(built, index, dayData, error)) {
            store.deleteGeneration(built, ignored);
            return false;
        }
    }
    if (!store.finishCompactGeneration(built,
                                       static_cast<std::uint8_t>(doc.days.size()),
                                       doc.name, error)) {
        store.deleteGeneration(built, ignored);
        return false;
    }
    return commitEditingGeneration(published, built, cal, source, error);
}

bool importCourseCompact(const char* data, std::size_t size, std::string& error) {
    error.clear();
    try {
        return importCompactBody(data, size, error);
    } catch (const std::bad_alloc&) {
        error = "not enough memory to import this course";
        return false;
    } catch (...) {
        error = "import failed";
        return false;
    }
}

bool importCourseCompact(const std::string& json, std::string& error) {
    return importCourseCompact(json.data(), json.size(), error);
}

bool importCourse(const std::string& json, std::string& error) {
    return importCourse(json.data(), json.size(), error);
}

bool applyDraft(std::string& error) {
    error.clear();
    course_store::CourseStore store;
    if (!store.hasCalendar()) {
        // First Apply on a clean install: publish the bootstrap draft as
        // generation N with an APPLIED binding. The calendar is one NVS blob,
        // so the switch is a single atomic record and nothing rings until an
        // explicit Activate.
        const std::uint32_t bootstrap = store.loadBootstrapGeneration();
        if (bootstrap == 0 || !store.generationExists(bootstrap)) {
            error = "no draft course to apply";
            return false;
        }
        course_store::CourseMeta meta;
        if (!readMeta(bootstrap, meta, error)) return false;
        if (!store.validateGeneration(bootstrap, meta.course_length, error)) {
            return false;
        }
        course_store::CalendarState next;
        next.active_generation = bootstrap;
        next.draft_generation = bootstrap;
        next.activation_date = 0;
        next.day_index = 0;
        next.course_length = meta.course_length;
        next.flags = course_store::kApplied;
        next.failure_count = 0;
        if (!store.saveCalendar(next, error)) return false;
        std::string ignored;
        if (!store.saveBootstrapGeneration(0, ignored)) {
            // The pointer is ignored once the calendar exists; a leftover
            // value cannot resurrect the draft.
            error.clear();
        }
        return true;
    }
    course_store::CalendarState cal;
    if (!loadCalendar(cal, error)) return false;
    if (cal.active_generation == 0) {
        error = "no active generation";
        return false;
    }
    std::uint32_t generation = 0;
    if (!readGeneration(cal, generation, error)) return false;
    course_store::CourseMeta meta;
    if (!readMeta(generation, meta, error)) return false;
    if (!store.validateGeneration(generation, meta.course_length, error)) {
        return false;
    }

    // Reclaim whatever the previous Apply left behind before recording a new
    // fallback generation.
    std::string ignored;
    store.reclaimPendingGeneration(ignored);

    course_store::CalendarState next = cal;
    next.active_generation = generation;
    next.draft_generation = generation;
    next.activation_date = 0;
    next.day_index = 0;
    next.course_length = meta.course_length;
    next.flags = course_store::kApplied;
    next.failure_count = 0;
    if (!store.saveCalendar(next, error)) return false;

    if (cal.active_generation != generation) {
        // Keep the replaced generation until the new active day loads.
        if (!store.setPendingGeneration(cal.active_generation, ignored)) {
            // Not fatal: the new generation is already published and valid.
        }
    }
    if (cal.draft_generation != 0 && cal.draft_generation != cal.active_generation &&
        cal.draft_generation != generation) {
        store.deleteGeneration(cal.draft_generation, ignored);
    }
    error.clear();
    return true;
}

bool discardDraft(std::string& error) {
    error.clear();
    course_store::CourseStore store;
    if (!store.hasCalendar()) {
        // Only a bootstrap draft can exist here. Dropping it leaves the device
        // exactly as it was: no calendar, legacy behaviour untouched.
        const std::uint32_t bootstrap = store.loadBootstrapGeneration();
        if (bootstrap == 0) {
            error = "no draft course to discard";
            return false;
        }
        if (!store.saveBootstrapGeneration(0, error)) return false;
        std::string ignored;
        store.deleteGeneration(bootstrap, ignored);
        return true;
    }
    course_store::CalendarState cal;
    if (!loadCalendar(cal, error)) return false;
    if (cal.active_generation == 0) {
        error = "no active generation";
        return false;
    }
    const std::uint32_t previous = cal.draft_generation;
    course_store::CalendarState next = cal;
    next.draft_generation = cal.active_generation;
    if (!store.saveCalendar(next, error)) return false;
    if (previous != 0 && previous != cal.active_generation) {
        std::string ignored;
        store.deleteGeneration(previous, ignored);
    }
    error.clear();
    return true;
}

bool ensureInitialCourse(std::string& error) {
    error.clear();
    course_store::CourseStore store;
    if (store.hasCalendar()) {
        error = "course already present";
        return false;
    }
    if (store.loadBootstrapGeneration() != 0) {
        // The operator is preparing a course on this device. Publishing the
        // reference course over it would silently discard their draft.
        error = "a draft course is already pending";
        return false;
    }
    course::Course reference = course::makeReferenceCourse();
    if (!course::validate(reference, error)) return false;
    std::uint32_t generation = 0;
    course_store::CalendarState state;
    if (!store.applyCourse(reference, generation, state, error)) return false;
    error.clear();
    return true;
}

// Resolves the generation to export without touching the template cache: an
// export must not disturb a running course or invalidate a loaded set.
static bool exportGeneration(ExportSource source, std::uint32_t& generation,
                             std::string& error) {
    course_store::CourseStore store;
    if (source == kExportActive && store.hasCalendar()) {
        course_store::CalendarState cal;
        if (!loadCalendar(cal, error)) return false;
        if (cal.active_generation == 0 || !store.generationExists(cal.active_generation)) {
            error = "no active generation";
            return false;
        }
        generation = cal.active_generation;
        return true;
    }
    if (!store.hasCalendar()) {
        const std::uint32_t bootstrap = store.loadBootstrapGeneration();
        if (bootstrap == 0 || !store.generationExists(bootstrap)) {
            error = "no course has been published";
            return false;
        }
        generation = bootstrap;
        return true;
    }
    course_store::CalendarState cal;
    if (!loadCalendar(cal, error)) return false;
    return readGeneration(cal, generation, error);
}

bool beginExport(ExportSource source, std::string& error) {
    error.clear();
    ExportState& state = exportState();
    state.active = false;
    state.templateNames.clear();
    std::uint32_t generation = 0;
    course_store::CourseStore store;
    if (!exportGeneration(source, generation, error)) return false;
    course_store::CourseMeta meta;
    if (!readMeta(generation, meta, error)) return false;
    state.active = true;
    state.compact = meta.layout == course_store::kLayoutTemplates;
    state.phase = 0;
    state.generation = generation;
    state.length = meta.course_length;
    state.next = 0;
    state.day = 0;
    state.name = meta.name;
    if (state.compact) {
        // Fail here rather than halfway through the response when the template
        // file is unreadable.
        if (!store.openTemplateExport(generation, error) ||
            !store.templateExportNames(state.templateNames)) {
            state.active = false;
            return false;
        }
    }
    return true;
}

bool nextExportChunk(std::string& chunk, bool& done, std::string& error) {
    error.clear();
    done = false;
    ExportState& state = exportState();
    if (!state.active) {
        error = "export not started";
        return false;
    }
    course_store::CourseStore store;

    // ── Compact layout: v1 with one templates section and day references ──
    if (state.compact) {
        if (state.phase == 0) {
            chunk = "{\"format\":\"gong-course\",\"version\":1,\"name\":\"";
            chunk += escapeJsonText(state.name);
            chunk += "\",\"templates\":{";
            state.phase = 1;
            return true;
        }
        if (state.phase == 1) {
            bool done = false;
            std::string fragment;
            if (!store.nextTemplateExportChunk(fragment, done, error)) return false;
            if (done) {
                chunk = "},\"days\":[";
                state.phase = 2;
                return true;
            }
            if (state.next > 0) chunk += ',';
            ++state.next;
            chunk += fragment;
            return true;
        }
        if (state.phase == 2) {
            const std::uint8_t day = state.day;
            if (day < state.length) {
                bool isReference = false;
                std::string templateName;
                std::vector<course::Event> events;
                if (!store.loadDayDescriptor(state.generation, day, isReference,
                                             templateName, events, error)) {
                    return false;
                }
                if (day > 0) chunk += ',';
                if (isReference) {
                    bool known = false;
                    for (std::size_t i = 0; i < state.templateNames.size(); ++i) {
                        if (state.templateNames[i] == templateName) {
                            known = true;
                            break;
                        }
                    }
                    if (!known) {
                        error = "day " + std::to_string(day) +
                                " references a missing template: " + templateName;
                        state.active = false;
                        return false;
                    }
                    chunk += '"';
                    chunk += escapeJsonText(templateName);
                    chunk += '"';
                } else {
                    appendEvents(chunk, events);
                }
                ++state.day;
                return true;
            }
            chunk = "]}";
            state.active = false;
            done = true;
            return true;
        }
    }

    if (state.next == 0) {
        chunk = "{\"format\":\"gong-course\",\"version\":2,\"name\":\"";
        // Reuse the model's escaping by round-tripping the name through a
        // minimal v2 document is overkill; names are short and already
        // validated, so escape the few characters that matter.
        for (std::size_t i = 0; i < state.name.size(); ++i) {
            const char c = state.name[i];
            if (c == '"' || c == '\\') chunk.push_back('\\');
            if (c == '\n') { chunk += "\\n"; continue; }
            chunk.push_back(c);
        }
        chunk += "\",\"generation\":";
        chunk += std::to_string(state.generation);
        chunk += ",\"course_length\":";
        chunk += std::to_string(static_cast<int>(state.length));
        chunk += ",\"days\":[";
        ++state.next;
        return true;
    }
    const std::uint8_t day = static_cast<std::uint8_t>(state.next - 1);
    if (day < state.length) {
        course::Day dayData;
        if (!store.loadDay(state.generation, day, dayData, error)) return false;
        if (day > 0) chunk += ',';
        chunk += "{\"events\":";
        appendEvents(chunk, dayData.events);
        chunk += "}";
        ++state.next;
        return true;
    }
    chunk = "]}";
    state.active = false;
    done = true;
    return true;
}

}  // namespace course_editor
