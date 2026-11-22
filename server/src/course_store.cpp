#include "course_store.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <Preferences.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace course_store {
namespace {

const char* kCourseDir = "/course";
const char* kNamespace = "gong";
const char* kCalendarKey = "course_state";
// NVS keys are limited to 15 characters (NVS_KEY_NAME_MAX_SIZE is 16 including
// the terminator); a longer key makes nvs_set_* fail with KEY_TOO_LONG on the
// device, which is why this one is short. See tests/host/nvs.h, which now
// enforces the same limit on the host.
const char* kBootstrapKey = "crs_boot";
const char* kLastFireKey = "last_fire";
const char* kMetaBase = "meta";

std::string toStd(const String& value) {
    return std::string(value.c_str(), value.length());
}

String toArduino(const std::string& value) {
    return String(value.c_str());
}

std::string hex32(std::uint32_t value) {
    char buffer[9];
    std::snprintf(buffer, sizeof(buffer), "%08lx",
                  static_cast<unsigned long>(value));
    return std::string(buffer);
}

bool parseU32(const std::string& text, std::uint32_t& out) {
    if (text.empty()) return false;
    errno = 0;
    char* end = nullptr;
    const unsigned long value = std::strtoul(text.c_str(), &end, 10);
    if (errno == ERANGE || end == text.c_str() || *end != '\0') return false;
    if (value > 0xFFFFFFFFul) return false;
    out = static_cast<std::uint32_t>(value);
    return true;
}

bool parseHex32(const std::string& text, std::uint32_t& out) {
    if (text.empty()) return false;
    errno = 0;
    char* end = nullptr;
    const unsigned long value = std::strtoul(text.c_str(), &end, 16);
    if (errno == ERANGE || end == text.c_str() || *end != '\0') return false;
    if (value > 0xFFFFFFFFul) return false;
    out = static_cast<std::uint32_t>(value);
    return true;
}

std::vector<std::string> splitSemicolon(const std::string& text) {
    std::vector<std::string> parts;
    std::string current;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == ';') {
            parts.push_back(current);
            current.clear();
        } else {
            current.push_back(text[i]);
        }
    }
    parts.push_back(current);
    return parts;
}

// Reads a file into `json` with a single allocation: the CRC header is parsed
// in place and erased from the same buffer, so a multi-KB templates.json is not
// copied twice on a device without PSRAM.
bool readFileWithCrc(const std::string& path, std::string& json,
                     std::string& error) {
    error.clear();
    File file = LittleFS.open(path.c_str(), "r");
    if (!file) {
        error = "cannot open " + path;
        return false;
    }
    std::string raw;
    const size_t length = file.size();
    raw.resize(length);
    size_t got = 0;
    if (length > 0) {
        got = file.readBytes(&raw[0], length);
        raw.resize(got);
    }
    file.close();

    const std::size_t newline = raw.find('\n');
    if (newline == std::string::npos) {
        error = "missing CRC header";
        return false;
    }
    std::string header = raw.substr(0, newline);
    if (!header.empty() && header[header.size() - 1] == '\r') {
        header.erase(header.size() - 1);
    }
    if (header.size() < 7 || header.compare(0, 6, "CRC32 ") != 0) {
        error = "invalid CRC header";
        return false;
    }
    std::uint32_t expected = 0;
    if (!parseHex32(header.substr(6), expected)) {
        error = "invalid CRC value";
        return false;
    }
    raw.erase(0, newline + 1);
    if (CourseStore::crc32(raw) != expected) {
        error = "CRC mismatch";
        return false;
    }
    json.swap(raw);
    return true;
}

// Incremental CRC so a file can be verified while it is streamed, without
// materialising it in RAM. Same polynomial and bit order as crc32().
std::uint32_t crc32Update(std::uint32_t crc, const void* data, std::size_t len) {
    const std::uint8_t* bytes = static_cast<const std::uint8_t*>(data);
    for (std::size_t i = 0; i < len; ++i) {
        crc ^= bytes[i];
        for (int bit = 0; bit < 8; ++bit) {
            const std::uint32_t mask =
                static_cast<std::uint32_t>(-(static_cast<int>(crc & 1u)));
            crc = (crc >> 1) ^ (0xEDB88320u & mask);
        }
    }
    return crc;
}

const std::size_t kIoChunk = 256;

// Verifies a file's CRC header and content by streaming it: no copy of the file
// is created, which matters for a multi-KB templates.json on a device without
// PSRAM.
bool verifyFileStreamed(const std::string& path, std::uint32_t expected,
                        std::size_t expectedSize, std::string& error) {
    File check = LittleFS.open(path.c_str(), "r");
    if (!check) {
        error = "cannot re-open " + path;
        return false;
    }
    std::string line;
    for (;;) {
        const int c = check.read();
        if (c < 0) break;
        if (c == '\n') break;
        line.push_back(static_cast<char>(c));
    }
    std::uint32_t stored = 0;
    if (line.size() < 7 || line.compare(0, 6, "CRC32 ") != 0 ||
        !parseHex32(line.substr(6), stored)) {
        check.close();
        error = "verification failed for " + path;
        return false;
    }
    std::uint32_t crc = 0xFFFFFFFFu;
    std::size_t total = 0;
    char buffer[kIoChunk];
    for (;;) {
        const size_t got = check.readBytes(buffer, sizeof(buffer));
        if (got == 0) break;
        total += got;
        crc = crc32Update(crc, buffer, got);
    }
    check.close();
    if (~crc != stored || total != expectedSize || stored != expected) {
        error = "verification mismatch for " + path;
        return false;
    }
    return true;
}

// Streams `json` with a CRC header and verifies it by re-reading in chunks.
// No envelope string and no full read-back buffer: peak RAM is the caller's
// buffer plus one 256-byte chunk, which matters for a multi-KB templates file.
bool writeFileWithCrc(const std::string& path, const std::string& json,
                      std::string& error) {
    error.clear();
    File file = LittleFS.open(path.c_str(), "w");
    if (!file) {
        error = "cannot create " + path;
        return false;
    }
    const std::uint32_t expected = CourseStore::crc32(json);
    char header[24];
    const int headerLen = std::snprintf(header, sizeof(header), "CRC32 %08lx\n",
                                        static_cast<unsigned long>(expected));
    if (headerLen <= 0 ||
        file.write(reinterpret_cast<const uint8_t*>(header),
                   static_cast<std::size_t>(headerLen)) !=
            static_cast<std::size_t>(headerLen)) {
        file.close();
        LittleFS.remove(path.c_str());
        error = "short write to " + path;
        return false;
    }
    for (std::size_t offset = 0; offset < json.size(); offset += kIoChunk) {
        const std::size_t count =
            (json.size() - offset < kIoChunk) ? (json.size() - offset) : kIoChunk;
        if (file.write(reinterpret_cast<const uint8_t*>(json.data() + offset), count) !=
            count) {
            file.close();
            LittleFS.remove(path.c_str());
            error = "short write to " + path;
            return false;
        }
    }
    file.close();

    if (!verifyFileStreamed(path, expected, json.size(), error)) {
        LittleFS.remove(path.c_str());
        return false;
    }
    return true;
}

std::string generationDir(std::uint32_t generation) {
    return std::string(kCourseDir) + "/g" + std::to_string(generation);
}

std::string basePath(std::uint32_t generation, const std::string& name) {
    return generationDir(generation) + "/" + name;
}

std::string dayBase(std::uint32_t generation, std::uint8_t day) {
    char name[8];
    std::snprintf(name, sizeof(name), "d%02u", static_cast<unsigned>(day));
    return basePath(generation, name);
}

std::string metaBase(std::uint32_t generation) {
    return basePath(generation, kMetaBase);
}

std::string templateBase(std::uint32_t generation) {
    return basePath(generation, "templates");
}

std::string escapeText(const std::string& text) {
    std::string out;
    out.reserve(text.size() + 8);
    for (std::size_t i = 0; i < text.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
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
    return out;
}

struct TemplateSlice {
    std::string name;
    std::size_t begin;
    std::size_t length;
    TemplateSlice() : begin(0), length(0) {}
};

// Reads templates.json into byte ranges: each template body is parsed on its
// own through the course model, so only one template is materialised in RAM at
// a time (a 32-event template costs the same peak as a 32-event day).
class TemplateScanner {
public:
    explicit TemplateScanner(const std::string& text) : text_(text), pos_(0) {}

    bool scan(std::vector<TemplateSlice>& out, std::string& error) {
        ws();
        if (!expect('{', error)) return false;
        bool sawFormat = false;
        bool sawVersion = false;
        bool sawTemplates = false;
        ws();
        if (peek() != '}') {
            for (;;) {
                std::string key;
                if (!readString(&key, 24, error)) return false;
                if (!expect(':', error)) return false;
                ws();
                if (key == "format") {
                    std::string value;
                    if (!readString(&value, 32, error)) return false;
                    if (value != "gong-course-templates") {
                        return fail(error, "unsupported templates format");
                    }
                    sawFormat = true;
                } else if (key == "version") {
                    std::string value;
                    if (!readNumber(value, error)) return false;
                    if (value != "1") return fail(error, "unsupported templates version");
                    sawVersion = true;
                } else if (key == "templates") {
                    if (!readTemplates(out, error)) return false;
                    sawTemplates = true;
                } else {
                    return fail(error, "unknown field: " + key);
                }
                ws();
                if (peek() == ',') {
                    ++pos_;
                    ws();
                    continue;
                }
                break;
            }
        }
        if (!expect('}', error)) return false;
        ws();
        if (pos_ != text_.size()) return fail(error, "trailing data");
        if (!sawFormat || !sawVersion || !sawTemplates) return fail(error, "missing fields");
        return true;
    }

private:
    const std::string& text_;
    std::size_t pos_;

    bool fail(std::string& error, const std::string& what) const {
        error = what + " at byte " + std::to_string(pos_);
        return false;
    }
    void ws() {
        while (pos_ < text_.size()) {
            const char c = text_[pos_];
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
                ++pos_;
            } else {
                break;
            }
        }
    }
    char peek() const { return pos_ < text_.size() ? text_[pos_] : '\0'; }
    bool expect(char c, std::string& error) {
        ws();
        if (pos_ >= text_.size() || text_[pos_] != c) {
            return fail(error, std::string("expected '") + c + "'");
        }
        ++pos_;
        return true;
    }

    bool readString(std::string* out, std::size_t cap, std::string& error) {
        ws();
        if (peek() != '"') return fail(error, "expected a string");
        ++pos_;
        std::size_t written = 0;
        for (;;) {
            if (pos_ >= text_.size()) return fail(error, "unterminated string");
            const char c = text_[pos_++];
            if (c == '"') break;
            if (static_cast<unsigned char>(c) < 0x20) return fail(error, "control character in string");
            if (c != '\\') {
                if (out && written++ >= cap) return fail(error, "string too long");
                if (out) out->push_back(c);
                continue;
            }
            if (pos_ >= text_.size()) return fail(error, "truncated escape");
            const char esc = text_[pos_++];
            char decoded = esc;
            if (esc == 'u') {
                if (pos_ + 4 > text_.size()) return fail(error, "truncated \\u escape");
                unsigned code = 0;
                for (int i = 0; i < 4; ++i) {
                    const char h = text_[pos_++];
                    code <<= 4;
                    if (h >= '0' && h <= '9') code |= static_cast<unsigned>(h - '0');
                    else if (h >= 'a' && h <= 'f') code |= static_cast<unsigned>(h - 'a' + 10);
                    else if (h >= 'A' && h <= 'F') code |= static_cast<unsigned>(h - 'A' + 10);
                    else return fail(error, "invalid \\u escape");
                }
                // Names are ASCII in practice; encode the BMP code point as UTF-8.
                if (code < 0x80) {
                    decoded = static_cast<char>(code);
                    if (out && written++ >= cap) return fail(error, "string too long");
                    if (out) out->push_back(decoded);
                } else if (code < 0x800) {
                    if (out && written + 2 > cap) return fail(error, "string too long");
                    if (out) {
                        out->push_back(static_cast<char>(0xC0 | (code >> 6)));
                        out->push_back(static_cast<char>(0x80 | (code & 0x3F)));
                        written += 2;
                    }
                } else {
                    if (out && written + 3 > cap) return fail(error, "string too long");
                    if (out) {
                        out->push_back(static_cast<char>(0xE0 | (code >> 12)));
                        out->push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
                        out->push_back(static_cast<char>(0x80 | (code & 0x3F)));
                        written += 3;
                    }
                }
                continue;
            }
            if (esc == 'b') decoded = '\b';
            else if (esc == 'f') decoded = '\f';
            else if (esc == 'n') decoded = '\n';
            else if (esc == 'r') decoded = '\r';
            else if (esc == 't') decoded = '\t';
            else if (esc != '"' && esc != '\\' && esc != '/') return fail(error, "invalid escape");
            if (out && written++ >= cap) return fail(error, "string too long");
            if (out) out->push_back(decoded);
        }
        if (out == nullptr) return true;
        return true;
    }

    bool readNumber(std::string& out, std::string& error) {
        ws();
        out.clear();
        while (pos_ < text_.size()) {
            const char c = text_[pos_];
            if ((c >= '0' && c <= '9') || c == '-' || c == '+' || c == '.' ||
                c == 'e' || c == 'E') {
                out.push_back(c);
                ++pos_;
            } else {
                break;
            }
        }
        if (out.empty()) return fail(error, "expected a number");
        return true;
    }

    bool skipValue(int depth, std::string& error) {
        if (depth > 32) return fail(error, "nesting too deep");
        ws();
        const char c = peek();
        if (c == '"') return readString(nullptr, 0, error);
        if (c == '{' || c == '[') {
            const char open = c;
            const char close = (c == '{') ? '}' : ']';
            ++pos_;
            ws();
            if (peek() == close) {
                ++pos_;
                return true;
            }
            for (;;) {
                if (open == '{') {
                    if (!readString(nullptr, 0, error)) return false;
                    if (!expect(':', error)) return false;
                }
                if (!skipValue(depth + 1, error)) return false;
                ws();
                if (peek() == ',') {
                    ++pos_;
                    ws();
                    continue;
                }
                break;
            }
            return expect(close, error);
        }
        if (c == 't' || c == 'f' || c == 'n') {
            const char* literal = (c == 't') ? "true" : (c == 'f') ? "false" : "null";
            const std::size_t len = std::strlen(literal);
            if (pos_ + len > text_.size() || text_.compare(pos_, len, literal) != 0) {
                return fail(error, "invalid literal");
            }
            pos_ += len;
            return true;
        }
        std::string ignored;
        return readNumber(ignored, error);
    }

    bool readTemplates(std::vector<TemplateSlice>& out, std::string& error) {
        if (!expect('{', error)) return false;
        ws();
        if (peek() == '}') {
            ++pos_;
            return true;
        }
        for (;;) {
            TemplateSlice slice;
            if (!readString(&slice.name, kMaxTemplateNameBytes, error)) return false;
            if (slice.name.empty()) return fail(error, "template name must not be empty");
            if (!expect(':', error)) return false;
            ws();
            if (peek() != '[') return fail(error, "template body must be an array");
            slice.begin = pos_;
            if (!skipValue(1, error)) return false;
            slice.length = pos_ - slice.begin;
            if (out.size() >= CourseStore::kMaxTemplates) {
                return fail(error, "too many templates");
            }
            out.push_back(slice);
            ws();
            if (peek() == ',') {
                ++pos_;
                ws();
                continue;
            }
            break;
        }
        return expect('}', error);
    }
};

// One loaded template set, tagged with the generation it belongs to. `loaded`
// is only true after a complete successful load.
struct TemplateSet {
    std::uint32_t generation;
    bool loaded;
    std::size_t events;
    std::map<std::string, std::vector<course::Event> > items;

    TemplateSet() : generation(0), loaded(false), events(0) {}

    void clear() {
        generation = 0;
        loaded = false;
        events = 0;
        items.clear();
    }
};

// Two slots. The active slot belongs to the generation the scheduler is
// executing and is never evicted by editor/draft work; the draft slot serves
// every other generation (validation, browsing, editing a draft). This is what
// keeps a running schedule alive while the operator looks at another course.
struct TemplateCacheState {
    TemplateSet active;
    TemplateSet draft;
};

TemplateCacheState& templateCache() {
    static TemplateCacheState state;
    return state;
}

void clearTemplateCache() {
    templateCache().active.clear();
    templateCache().draft.clear();
}

// The set used by the cache accessors: the active one when present.
const TemplateSet* activeOrDraftSet() {
    const TemplateCacheState& cache = templateCache();
    if (cache.active.loaded) return &cache.active;
    if (cache.draft.loaded) return &cache.draft;
    return nullptr;
}

// The set belonging to `generation`, or null. A set from another generation is
// never returned, so a stale entry cannot be served.
const TemplateSet* findTemplateSet(std::uint32_t generation) {
    const TemplateCacheState& cache = templateCache();
    if (generation == 0) return nullptr;
    if (cache.active.loaded && cache.active.generation == generation) return &cache.active;
    if (cache.draft.loaded && cache.draft.generation == generation) return &cache.draft;
    return nullptr;
}

bool loadTemplateFile(std::uint32_t generation,
                      std::map<std::string, std::vector<course::Event> >& out,
                      std::size_t& totalEvents, std::string& error) {
    out.clear();
    totalEvents = 0;
    std::string raw;
    if (!readFileWithCrc(templateBase(generation) + ".json", raw, error)) return false;

    std::vector<TemplateSlice> slices;
    TemplateScanner scanner(raw);
    if (!scanner.scan(slices, error)) return false;

    std::map<std::string, std::vector<course::Event> > parsed;
    std::size_t events = 0;
    for (std::size_t i = 0; i < slices.size(); ++i) {
        const TemplateSlice& slice = slices[i];
        if (parsed.find(slice.name) != parsed.end()) {
            error = "duplicate template: " + slice.name;
            return false;
        }
        // Parse the body event by event: peak RAM is one event (~5 KB for a
        // 32-event template) instead of the ~82 KB the whole-array tree needs.
        std::vector<course::Event> parsedEvents;
        std::string parseError;
        if (!course::parseEventArrayBounded(raw.data() + slice.begin, slice.length,
                                            parsedEvents, parseError)) {
            error = "template " + slice.name + ": " + parseError;
            return false;
        }
        events += parsedEvents.size();
        if (events > CourseStore::kMaxTemplateEvents) {
            error = "too many template events";
            return false;
        }
        parsed[slice.name] = parsedEvents;
    }
    out.swap(parsed);
    totalEvents = events;
    return true;
}

bool templatesFilePresent(std::uint32_t generation) {
    const std::string base = templateBase(generation);
    return LittleFS.exists((base + ".json").c_str()) ||
           LittleFS.exists((base + ".tmp").c_str()) ||
           LittleFS.exists((base + ".bak").c_str());
}

bool ensureGenerationDir(std::uint32_t generation) {
    LittleFS.mkdir(kCourseDir);
    LittleFS.mkdir(generationDir(generation).c_str());
    return true;
}

bool writeAtomic(const std::string& base, const std::string& json,
                 std::string& error) {
    const std::string path = base + ".json";
    const std::string tmp = base + ".tmp";
    const std::string bak = base + ".bak";

    if (!writeFileWithCrc(tmp, json, error)) return false;

    if (LittleFS.exists(path.c_str())) {
        if (LittleFS.exists(bak.c_str())) {
            if (!LittleFS.remove(bak.c_str())) {
                LittleFS.remove(tmp.c_str());
                error = "cannot remove old backup " + bak;
                return false;
            }
        }
        if (!LittleFS.rename(path.c_str(), bak.c_str())) {
            LittleFS.remove(tmp.c_str());
            error = "cannot back up " + path;
            return false;
        }
    }

    if (!LittleFS.rename(tmp.c_str(), path.c_str())) {
        if (LittleFS.exists(bak.c_str())) {
            LittleFS.rename(bak.c_str(), path.c_str());
        }
        error = "cannot publish " + path;
        return false;
    }
    LittleFS.remove(bak.c_str());
    return true;
}

bool recoverBase(const std::string& base, bool allowTmpRestore,
                 std::string& error) {
    const std::string path = base + ".json";
    const std::string tmp = base + ".tmp";
    const std::string bak = base + ".bak";

    std::string json;
    if (LittleFS.exists(path.c_str())) {
        if (readFileWithCrc(path, json, error)) {
            LittleFS.remove(tmp.c_str());
            LittleFS.remove(bak.c_str());
            return true;
        }
        if (LittleFS.exists(bak.c_str())) {
            std::string backup;
            if (readFileWithCrc(bak, backup, error)) {
                if (!LittleFS.remove(path.c_str())) {
                    error = "cannot remove damaged " + path;
                    return false;
                }
                if (!LittleFS.rename(bak.c_str(), path.c_str())) {
                    error = "cannot restore " + bak;
                    return false;
                }
                LittleFS.remove(tmp.c_str());
                return true;
            }
        }
        if (allowTmpRestore && LittleFS.exists(tmp.c_str())) {
            std::string pending;
            if (readFileWithCrc(tmp, pending, error)) {
                if (!LittleFS.remove(path.c_str())) {
                    error = "cannot remove damaged " + path;
                    return false;
                }
                if (!LittleFS.rename(tmp.c_str(), path.c_str())) {
                    error = "cannot publish " + tmp;
                    return false;
                }
                return true;
            }
        }
        error = "damaged file without valid backup: " + path;
        return false;
    }

    if (LittleFS.exists(bak.c_str())) {
        std::string backup;
        if (readFileWithCrc(bak, backup, error)) {
            if (!LittleFS.rename(bak.c_str(), path.c_str())) {
                error = "cannot restore " + bak;
                return false;
            }
            LittleFS.remove(tmp.c_str());
            return true;
        }
    }

    if (allowTmpRestore && LittleFS.exists(tmp.c_str())) {
        std::string pending;
        if (readFileWithCrc(tmp, pending, error)) {
            if (!LittleFS.rename(tmp.c_str(), path.c_str())) {
                error = "cannot publish " + tmp;
                return false;
            }
            return true;
        }
    }

    error = "missing file: " + path;
    return false;
}

bool parseMetaJson(const std::string& json, CourseMeta& out, std::string& error) {
    DynamicJsonDocument doc(2048);
    const DeserializationError result = deserializeJson(doc, json);
    if (result) {
        error = "meta JSON parse error";
        return false;
    }
    if (doc.overflowed()) {
        error = "meta JSON too large";
        return false;
    }
    if (!doc["format"].is<const char*>() ||
        std::string(doc["format"].as<const char*>()) != "gong-course-store") {
        error = "invalid meta format";
        return false;
    }
    if (!doc["version"].is<std::uint32_t>() ||
        doc["version"].as<std::uint32_t>() != 1) {
        error = "invalid meta version";
        return false;
    }
    if (!doc["generation"].is<std::uint32_t>() ||
        !doc["course_length"].is<std::uint32_t>()) {
        error = "invalid meta fields";
        return false;
    }
    const std::uint32_t generation = doc["generation"].as<std::uint32_t>();
    const std::uint32_t length = doc["course_length"].as<std::uint32_t>();
    if (generation == 0 || length < 1 || length > course::kMaxDays) {
        error = "meta values out of range";
        return false;
    }
    if (!doc["name"].is<const char*>()) {
        error = "missing meta name";
        return false;
    }
    out.generation = generation;
    out.course_length = static_cast<std::uint8_t>(length);
    out.name = doc["name"].as<const char*>();
    out.layout = kLayoutExpanded;
    if (doc.containsKey("layout")) {
        if (!doc["layout"].is<const char*>() ||
            std::string(doc["layout"].as<const char*>()) != "tpl1") {
            // An unknown layout would make day reference files unreadable, so
            // it is refused instead of being silently ignored.
            error = "unsupported generation layout";
            return false;
        }
        out.layout = kLayoutTemplates;
    }
    return true;
}

std::string makeMetaJson(const CourseMeta& meta, std::string& error) {
    DynamicJsonDocument doc(2048);
    doc["format"] = "gong-course-store";
    doc["version"] = 1;
    doc["generation"] = meta.generation;
    doc["course_length"] = meta.course_length;
    doc["name"] = meta.name;
    if (meta.layout == kLayoutTemplates) doc["layout"] = "tpl1";
    if (doc.overflowed()) {
        error = "meta JSON too large";
        return std::string();
    }
    String output;
    serializeJson(doc, output);
    return toStd(output);
}

bool readCalendarBlob(const std::string& blob, CalendarState& out,
                      std::string& error) {
    const std::vector<std::string> parts = splitSemicolon(blob);
    if (parts.size() != 9) {
        error = "calendar field count mismatch";
        return false;
    }
    if (parts[0] != "v1") {
        error = "unsupported calendar version";
        return false;
    }
    std::uint32_t active = 0;
    std::uint32_t draft = 0;
    std::uint32_t date = 0;
    std::uint32_t day = 0;
    std::uint32_t length = 0;
    std::uint32_t flags = 0;
    std::uint32_t failures = 0;
    std::uint32_t expectedCrc = 0;
    if (!parseU32(parts[1], active) || !parseU32(parts[2], draft) ||
        !parseU32(parts[3], date) || !parseU32(parts[4], day) ||
        !parseU32(parts[5], length) || !parseU32(parts[6], flags) ||
        !parseU32(parts[7], failures) || !parseHex32(parts[8], expectedCrc)) {
        error = "calendar numeric parse error";
        return false;
    }
    std::string payload;
    for (std::size_t i = 0; i + 1 < parts.size(); ++i) {
        if (i) payload += ';';
        payload += parts[i];
    }
    if (CourseStore::crc32(payload) != expectedCrc) {
        error = "calendar CRC mismatch";
        return false;
    }
    if (day > 255 || length > 255 || flags > 255 || failures > 255) {
        error = "calendar value out of range";
        return false;
    }
    out.active_generation = active;
    out.draft_generation = draft;
    out.activation_date = date;
    out.day_index = static_cast<std::uint8_t>(day);
    out.course_length = static_cast<std::uint8_t>(length);
    out.flags = static_cast<std::uint8_t>(flags);
    out.failure_count = static_cast<std::uint8_t>(failures);
    return true;
}

std::string makeCalendarBlob(const CalendarState& state) {
    std::string payload = "v1;";
    payload += std::to_string(state.active_generation);
    payload += ';';
    payload += std::to_string(state.draft_generation);
    payload += ';';
    payload += std::to_string(state.activation_date);
    payload += ';';
    payload += std::to_string(static_cast<unsigned>(state.day_index));
    payload += ';';
    payload += std::to_string(static_cast<unsigned>(state.course_length));
    payload += ';';
    payload += std::to_string(static_cast<unsigned>(state.flags));
    payload += ';';
    payload += std::to_string(static_cast<unsigned>(state.failure_count));
    return payload + ";" + hex32(CourseStore::crc32(payload));
}

bool readLastFireBlob(const std::string& blob, LastFire& out,
                      std::string& error) {
    const std::vector<std::string> parts = splitSemicolon(blob);
    if (parts.size() != 8) {
        error = "last_fire field count mismatch";
        return false;
    }
    if (parts[0] != "v1") {
        error = "unsupported last_fire version";
        return false;
    }
    std::uint32_t generation = 0;
    std::uint32_t day = 0;
    std::uint32_t minute = 0;
    std::uint32_t event = 0;
    std::uint32_t date = 0;
    std::uint32_t timestamp = 0;
    std::uint32_t expectedCrc = 0;
    if (!parseU32(parts[1], generation) || !parseU32(parts[2], day) ||
        !parseU32(parts[3], minute) || !parseU32(parts[4], event) ||
        !parseU32(parts[5], date) || !parseU32(parts[6], timestamp) ||
        !parseHex32(parts[7], expectedCrc)) {
        error = "last_fire numeric parse error";
        return false;
    }
    std::string payload;
    for (std::size_t i = 0; i + 1 < parts.size(); ++i) {
        if (i) payload += ';';
        payload += parts[i];
    }
    if (CourseStore::crc32(payload) != expectedCrc) {
        error = "last_fire CRC mismatch";
        return false;
    }
    if (day > 255 || minute > 1439) {
        error = "last_fire value out of range";
        return false;
    }
    out.generation = generation;
    out.day_index = static_cast<std::uint8_t>(day);
    out.minute = static_cast<std::uint16_t>(minute);
    out.event_id = event;
    out.date = date;
    out.timestamp = timestamp;
    return true;
}

std::string makeLastFireBlob(const LastFire& value) {
    std::string payload = "v1;";
    payload += std::to_string(value.generation);
    payload += ';';
    payload += std::to_string(static_cast<unsigned>(value.day_index));
    payload += ';';
    payload += std::to_string(static_cast<unsigned>(value.minute));
    payload += ';';
    payload += std::to_string(value.event_id);
    payload += ';';
    payload += std::to_string(value.date);
    payload += ';';
    payload += std::to_string(value.timestamp);
    return payload + ";" + hex32(CourseStore::crc32(payload));
}

}  // namespace

CalendarState::CalendarState()
    : active_generation(0),
      draft_generation(0),
      activation_date(0),
      day_index(0),
      course_length(0),
      flags(0),
      failure_count(0) {}

LastFire::LastFire()
    : generation(0),
      day_index(0),
      minute(0),
      event_id(0),
      date(0),
      timestamp(0) {}

CourseMeta::CourseMeta()
    : generation(0), course_length(0), name(), layout(kLayoutExpanded) {}

const std::size_t CourseStore::kMaxTemplates;
const std::size_t CourseStore::kMaxTemplateEvents;

CourseStore::CourseStore() {}

std::uint32_t CourseStore::crc32(const std::string& data) {
    std::uint32_t crc = 0xFFFFFFFFu;
    for (std::size_t i = 0; i < data.size(); ++i) {
        crc ^= static_cast<std::uint8_t>(data[i]);
        for (int bit = 0; bit < 8; ++bit) {
            const std::uint32_t mask =
                static_cast<std::uint32_t>(-(static_cast<int>(crc & 1u)));
            crc = (crc >> 1) ^ (0xEDB88320u & mask);
        }
    }
    return ~crc;
}

namespace {
bool readCalendarIfPresent(CalendarState& state, bool& present,
                           std::string& error) {
    present = false;
    error.clear();
    Preferences prefs;
    if (!prefs.begin(kNamespace, false)) {
        error = "cannot open NVS";
        return false;
    }
    if (!prefs.isKey(kCalendarKey)) {
        prefs.end();
        return true;
    }
    present = true;
    String blob = prefs.getString(kCalendarKey, "");
    prefs.end();
    return readCalendarBlob(toStd(blob), state, error);
}

}  // namespace

bool CourseStore::begin(std::string& error) {
    error.clear();
    if (!LittleFS.mkdir(kCourseDir)) {
        // mkdir returns false when the directory already exists on some
        // Arduino cores; that is not an error.
    }
    return true;
}

bool CourseStore::hasCalendar() {
    Preferences prefs;
    if (!prefs.begin(kNamespace, true)) return false;
    const bool present = prefs.isKey(kCalendarKey);
    prefs.end();
    return present;
}

std::uint32_t CourseStore::loadBootstrapGeneration() {
    Preferences prefs;
    if (!prefs.begin(kNamespace, true)) return 0;
    const std::uint32_t generation = prefs.getUInt(kBootstrapKey, 0);
    prefs.end();
    return generation;
}

bool CourseStore::saveBootstrapGeneration(std::uint32_t generation,
                                          std::string& error) {
    error.clear();
    Preferences prefs;
    if (!prefs.begin(kNamespace, false)) {
        error = "cannot open NVS";
        return false;
    }
    bool ok = true;
    if (generation == 0) {
        // remove() reports false for an absent key; clearing nothing is fine.
        if (prefs.isKey(kBootstrapKey)) ok = prefs.remove(kBootstrapKey);
    } else {
        ok = prefs.putUInt(kBootstrapKey, generation) == sizeof(generation);
    }
    prefs.end();
    if (!ok) {
        error = "bootstrap pointer write failed";
        return false;
    }
    return true;
}

bool CourseStore::loadCalendar(CalendarState& out, std::string& error) {
    error.clear();
    Preferences prefs;
    if (!prefs.begin(kNamespace, true)) {
        error = "cannot open NVS";
        return false;
    }
    if (!prefs.isKey(kCalendarKey)) {
        prefs.end();
        error = "calendar not found";
        return false;
    }
    const String raw = prefs.getString(kCalendarKey, "");
    prefs.end();
    return readCalendarBlob(toStd(raw), out, error);
}

bool CourseStore::saveCalendar(const CalendarState& state, std::string& error) {
    error.clear();
    const std::string blob = makeCalendarBlob(state);
    Preferences prefs;
    if (!prefs.begin(kNamespace, false)) {
        error = "cannot open NVS";
        return false;
    }
    const size_t written = prefs.putString(kCalendarKey, toArduino(blob));
    prefs.end();
    if (written != blob.size()) {
        error = "calendar NVS write failed";
        return false;
    }
    return true;
}

bool CourseStore::loadMeta(std::uint32_t generation, CourseMeta& out,
                           std::string& error) {
    error.clear();
    if (generation == 0) {
        error = "invalid generation";
        return false;
    }
    std::string json;
    if (!readFileWithCrc(metaBase(generation) + ".json", json, error)) {
        return false;
    }
    if (!parseMetaJson(json, out, error)) return false;
    if (out.generation != generation) {
        error = "meta generation mismatch";
        return false;
    }
    return true;
}

bool CourseStore::loadDay(std::uint32_t generation, std::uint8_t day,
                          course::Day& out, std::string& error) {
    error.clear();
    if (generation == 0 || day >= course::kMaxDays) {
        error = "day index out of range";
        return false;
    }
    std::string json;
    if (!readFileWithCrc(dayBase(generation, day) + ".json", json, error)) {
        return false;
    }
    // A reference day cannot be validated against an empty template map, so
    // the structural parse allows the unresolved name and loadDay() below
    // resolves it from the generation's own template set.
    course::Course one;
    if (!course::fromJsonUnresolved(json, one, error)) return false;
    if (one.generation != generation) {
        error = "day generation mismatch";
        return false;
    }
    if (one.days.size() != 1) {
        error = "day file must contain one day";
        return false;
    }
    course::Day resolved = one.days[0];
    if (!resolved.hasOwnEvents) {
        // A reference day: the events live once in templates.json. Without a
        // cache that belongs to this exact generation the day is unusable —
        // returning an empty day here would ring nothing while looking healthy.
        if (resolved.templateName.empty()) {
            error = "day has neither template nor events";
            return false;
        }
        const TemplateSet* set = findTemplateSet(generation);
        if (set == nullptr) {
            error = "templates for generation " + std::to_string(generation) +
                    " are not loaded";
            return false;
        }
        const std::map<std::string, std::vector<course::Event> >& items = set->items;
        std::map<std::string, std::vector<course::Event> >::const_iterator it =
            items.find(resolved.templateName);
        if (it == items.end()) {
            error = "unknown template: " + resolved.templateName;
            return false;
        }
        resolved.events = it->second;
        resolved.hasOwnEvents = true;
        resolved.templateName.clear();
    }
    out = resolved;
    return true;
}

bool CourseStore::saveDayAtomic(std::uint32_t generation, std::uint8_t day,
                                const course::Day& dayData,
                                std::string& error) {
    error.clear();
    if (generation == 0 || day >= course::kMaxDays) {
        error = "day index out of range";
        return false;
    }
    if (!dayData.hasOwnEvents) {
        error = "day must be materialised before storage";
        return false;
    }
    course::Day stored = dayData;
    stored.templateName.clear();
    course::Course one;
    one.name = "day";
    one.generation = generation;
    one.days.push_back(stored);
    if (!course::validate(one, error)) return false;
    const std::string json = course::toJsonV2(one, true, error);
    if (json.empty()) return false;
    return writeAtomic(dayBase(generation, day), json, error);
}

bool CourseStore::applyCourse(const course::Course& course,
                              std::uint32_t& newGeneration,
                              CalendarState& outState, std::string& error) {
    error.clear();
    if (!course::validate(course, error)) return false;
    if (course.days.empty() || course.days.size() > course::kMaxDays) {
        error = "course length out of range";
        return false;
    }

    CalendarState oldState;
    bool hasOld = false;
    if (!readCalendarIfPresent(oldState, hasOld, error)) return false;

    std::uint32_t generation = 1;
    if (hasOld && oldState.active_generation > 0) {
        generation = oldState.active_generation + 1;
    }
    for (unsigned guard = 0; guard < 10000; ++guard) {
        if (!LittleFS.exists((metaBase(generation) + ".json").c_str())) break;
        ++generation;
    }
    if (generation == 0) generation = 1;
    if (!ensureGenerationDir(generation)) {
        error = "cannot create generation directory";
        return false;
    }

    for (std::size_t i = 0; i < course.days.size(); ++i) {
        std::vector<course::Event> events;
        if (!course::resolvedEvents(course, i, events, error)) return false;
        course::Day flat;
        flat.hasOwnEvents = true;
        flat.events = events;
        if (!saveDayAtomic(generation, static_cast<std::uint8_t>(i), flat,
                           error)) {
            return false;
        }
    }

    CourseMeta meta;
    meta.generation = generation;
    meta.course_length = static_cast<std::uint8_t>(course.days.size());
    meta.name = course.name;
    std::string metaJson = makeMetaJson(meta, error);
    if (metaJson.empty()) return false;
    if (!writeAtomic(metaBase(generation), metaJson, error)) return false;

    if (!validateGeneration(generation, meta.course_length, error)) return false;

    CalendarState next;
    next.active_generation = generation;
    next.draft_generation = generation;
    next.activation_date = 0;
    next.day_index = 0;
    next.course_length = meta.course_length;
    next.flags = kApplied;
    next.failure_count = 0;
    if (!saveCalendar(next, error)) return false;

    if (hasOld) {
        if (oldState.active_generation > 0 &&
            oldState.active_generation != generation) {
            deleteGeneration(oldState.active_generation, error);
        }
        if (oldState.draft_generation > 0 &&
            oldState.draft_generation != generation &&
            oldState.draft_generation != oldState.active_generation) {
            deleteGeneration(oldState.draft_generation, error);
        }
    }
    error.clear();
    newGeneration = generation;
    outState = next;
    return true;
}

bool CourseStore::saveDraftCourse(const course::Course& course,
                                  std::uint32_t& draftGeneration,
                                  CalendarState& outState,
                                  std::string& error) {
    error.clear();
    if (!course::validate(course, error)) return false;
    if (course.days.empty() || course.days.size() > course::kMaxDays) {
        error = "course length out of range";
        return false;
    }

    CalendarState oldState;
    bool hasOld = false;
    if (!readCalendarIfPresent(oldState, hasOld, error)) return false;

    std::uint32_t generation = 1;
    if (hasOld) {
        generation = oldState.active_generation;
        if (oldState.draft_generation > generation) {
            generation = oldState.draft_generation;
        }
        ++generation;
    }
    for (unsigned guard = 0; guard < 10000; ++guard) {
        if (!LittleFS.exists((metaBase(generation) + ".json").c_str())) break;
        ++generation;
    }
    if (!ensureGenerationDir(generation)) {
        error = "cannot create generation directory";
        return false;
    }

    for (std::size_t i = 0; i < course.days.size(); ++i) {
        std::vector<course::Event> events;
        if (!course::resolvedEvents(course, i, events, error)) return false;
        course::Day flat;
        flat.hasOwnEvents = true;
        flat.events = events;
        if (!saveDayAtomic(generation, static_cast<std::uint8_t>(i), flat,
                           error)) {
            return false;
        }
    }

    CourseMeta meta;
    meta.generation = generation;
    meta.course_length = static_cast<std::uint8_t>(course.days.size());
    meta.name = course.name;
    std::string metaJson = makeMetaJson(meta, error);
    if (metaJson.empty()) return false;
    if (!writeAtomic(metaBase(generation), metaJson, error)) return false;
    if (!validateGeneration(generation, meta.course_length, error)) return false;

    CalendarState next = oldState;
    next.draft_generation = generation;
    if (!hasOld) {
        next.active_generation = 0;
        next.activation_date = 0;
        next.day_index = 0;
        next.course_length = meta.course_length;
        next.flags = 0;
        next.failure_count = 0;
    }
    if (!saveCalendar(next, error)) return false;

    if (hasOld && oldState.draft_generation > 0 &&
        oldState.draft_generation != generation &&
        oldState.draft_generation != oldState.active_generation) {
        deleteGeneration(oldState.draft_generation, error);
    }
    error.clear();
    draftGeneration = generation;
    outState = next;
    return true;
}

bool CourseStore::applyDraft(std::uint32_t draftGeneration,
                             CalendarState& outState, std::string& error) {
    error.clear();
    if (draftGeneration == 0) {
        error = "invalid draft generation";
        return false;
    }
    CourseMeta meta;
    if (!loadMeta(draftGeneration, meta, error)) return false;
    if (!validateGeneration(draftGeneration, meta.course_length, error)) {
        return false;
    }

    CalendarState oldState;
    bool hasOld = false;
    if (!readCalendarIfPresent(oldState, hasOld, error)) return false;

    CalendarState next;
    next.active_generation = draftGeneration;
    next.draft_generation = draftGeneration;
    next.activation_date = 0;
    next.day_index = 0;
    next.course_length = meta.course_length;
    next.flags = kApplied;
    next.failure_count = 0;
    if (!saveCalendar(next, error)) return false;

    if (hasOld && oldState.active_generation > 0 &&
        oldState.active_generation != draftGeneration) {
        deleteGeneration(oldState.active_generation, error);
    }
    if (hasOld && oldState.draft_generation > 0 &&
        oldState.draft_generation != draftGeneration &&
        oldState.draft_generation != oldState.active_generation) {
        deleteGeneration(oldState.draft_generation, error);
    }
    error.clear();
    outState = next;
    return true;
}

bool CourseStore::cloneGeneration(std::uint32_t from,
                                  std::uint32_t& outGeneration,
                                  std::string& error) {
    error.clear();
    if (from == 0) {
        error = "invalid generation";
        return false;
    }
    CourseMeta meta;
    if (!loadMeta(from, meta, error)) return false;
    if (meta.course_length == 0 || meta.course_length > course::kMaxDays) {
        error = "generation length out of range";
        return false;
    }

    std::uint32_t generation = from + 1;
    for (unsigned guard = 0; guard < 10000; ++guard) {
        if (!LittleFS.exists((metaBase(generation) + ".json").c_str())) break;
        ++generation;
    }
    if (generation == 0) generation = 1;
    if (!ensureGenerationDir(generation)) {
        error = "cannot create generation directory";
        return false;
    }

    std::string scratch;
    if (meta.layout == kLayoutTemplates) {
        // Copy a template generation verbatim: the two files keep the same
        // layout (references stay references), which is both cheaper and
        // exactly equivalent. Resolving here would materialise every day and
        // then contradict the tpl1 marker.
        if (templatesFilePresent(from)) {
            std::string tplJson;
            if (!readFileWithCrc(templateBase(from) + ".json", tplJson, error) ||
                !writeAtomic(templateBase(generation), tplJson, error)) {
                deleteGeneration(generation, scratch);
                return false;
            }
        }
        for (std::uint8_t day = 0; day < meta.course_length; ++day) {
            std::string dayJson;
            if (!readFileWithCrc(dayBase(from, day) + ".json", dayJson, error)) {
                deleteGeneration(generation, scratch);
                return false;
            }
            course::Course one;
            if (!course::fromJsonUnresolved(dayJson, one, error) ||
                one.days.size() != 1) {
                deleteGeneration(generation, scratch);
                return false;
            }
            std::string copy;
            if (one.days[0].hasOwnEvents) {
                one.generation = generation;
                copy = course::toJsonV2(one, true, error);
            } else {
                // A reference stays a reference; only the generation id is
                // rewritten, so the clone does not materialise the course.
                copy = "{\"format\":\"gong-course\",\"version\":2,\"name\":\"day\","
                       "\"generation\":" +
                       std::to_string(generation) +
                       ",\"course_length\":1,\"days\":[{\"template\":\"" +
                       escapeText(one.days[0].templateName) + "\"}]}";
            }
            if (copy.empty() || !writeAtomic(dayBase(generation, day), copy, error)) {
                deleteGeneration(generation, scratch);
                return false;
            }
        }
    } else {
        for (std::uint8_t day = 0; day < meta.course_length; ++day) {
            course::Day data;
            if (!loadDay(from, day, data, error)) {
                deleteGeneration(generation, scratch);
                return false;
            }
            if (!saveDayAtomic(generation, day, data, error)) {
                deleteGeneration(generation, scratch);
                return false;
            }
        }
    }

    CourseMeta newMeta = meta;
    newMeta.generation = generation;
    const std::string metaJson = makeMetaJson(newMeta, error);
    if (metaJson.empty()) {
        deleteGeneration(generation, scratch);
        return false;
    }
    if (!writeAtomic(metaBase(generation), metaJson, error)) {
        deleteGeneration(generation, scratch);
        return false;
    }
    if (!validateGeneration(generation, meta.course_length, error)) {
        deleteGeneration(generation, scratch);
        return false;
    }
    error.clear();
    outGeneration = generation;
    return true;
}

bool CourseStore::createGeneration(std::uint32_t& outGeneration,
                                   std::string& error) {
    error.clear();
    std::uint32_t generation = 1;
    CalendarState state;
    bool present = false;
    if (!readCalendarIfPresent(state, present, error)) return false;
    if (present) {
        generation = state.active_generation > state.draft_generation
                         ? state.active_generation
                         : state.draft_generation;
        ++generation;
    }
    if (generation == 0) generation = 1;
    for (unsigned guard = 0; guard < 10000; ++guard) {
        const std::string meta = metaBase(generation) + ".json";
        const std::string first = dayBase(generation, 0) + ".json";
        if (!LittleFS.exists(meta.c_str()) &&
            !LittleFS.exists((metaBase(generation) + ".tmp").c_str()) &&
            !LittleFS.exists(first.c_str())) {
            break;
        }
        ++generation;
    }
    if (!ensureGenerationDir(generation)) {
        error = "cannot create generation directory";
        return false;
    }
    error.clear();
    outGeneration = generation;
    return true;
}

bool CourseStore::saveMeta(const CourseMeta& meta, std::string& error) {
    error.clear();
    if (meta.generation == 0) {
        error = "invalid generation";
        return false;
    }
    if (meta.course_length == 0 || meta.course_length > course::kMaxDays) {
        error = "course length out of range";
        return false;
    }
    const std::string json = makeMetaJson(meta, error);
    if (json.empty()) return false;
    return writeAtomic(metaBase(meta.generation), json, error);
}

bool CourseStore::generationExists(std::uint32_t generation) {
    if (generation == 0) return false;
    return LittleFS.exists((metaBase(generation) + ".json").c_str());
}

bool CourseStore::validateGeneration(std::uint32_t generation,
                                     std::uint8_t expectedLength,
                                     std::string& error) {
    error.clear();
    CourseMeta meta;
    if (!loadMeta(generation, meta, error)) return false;
    if (meta.course_length != expectedLength) {
        error = "course length mismatch";
        return false;
    }
    if (templatesFilePresent(generation) &&
        !prepareTemplates(generation, error)) {
        return false;
    }
    for (std::uint8_t i = 0; i < meta.course_length; ++i) {
        course::Day day;
        if (!loadDay(generation, i, day, error)) return false;
    }
    return true;
}

bool CourseStore::deleteGeneration(std::uint32_t generation,
                                   std::string& error) {
    error.clear();
    if (generation == 0) return true;
    for (std::uint8_t i = 0; i < course::kMaxDays; ++i) {
        const std::string base = dayBase(generation, i);
        LittleFS.remove((base + ".json").c_str());
        LittleFS.remove((base + ".tmp").c_str());
        LittleFS.remove((base + ".bak").c_str());
    }
    const std::string meta = metaBase(generation);
    LittleFS.remove((meta + ".json").c_str());
    LittleFS.remove((meta + ".tmp").c_str());
    LittleFS.remove((meta + ".bak").c_str());
    const std::string tpl = templateBase(generation);
    LittleFS.remove((tpl + ".json").c_str());
    LittleFS.remove((tpl + ".tmp").c_str());
    LittleFS.remove((tpl + ".bak").c_str());
    if (templateCache().active.generation == generation) templateCache().active.clear();
    if (templateCache().draft.generation == generation) templateCache().draft.clear();
    return true;
}

bool CourseStore::setPendingGeneration(std::uint32_t generation,
                                       std::string& error) {
    error.clear();
    const std::string path = std::string(kCourseDir) + "/pending_gen";
    if (generation == 0) {
        LittleFS.remove(path.c_str());
        return true;
    }
    File file = LittleFS.open(path.c_str(), "w");
    if (!file) {
        error = "cannot write pending generation";
        return false;
    }
    const std::string text = std::to_string(generation) + "\n";
    const size_t written = file.write(
        reinterpret_cast<const uint8_t*>(text.data()), text.size());
    file.close();
    if (written != text.size()) {
        LittleFS.remove(path.c_str());
        error = "short write to pending generation";
        return false;
    }
    return true;
}

bool CourseStore::reclaimPendingGeneration(std::string& error) {
    error.clear();
    const std::string path = std::string(kCourseDir) + "/pending_gen";
    if (!LittleFS.exists(path.c_str())) return true;
    File file = LittleFS.open(path.c_str(), "r");
    if (!file) {
        error = "cannot read pending generation";
        return false;
    }
    const String raw = file.readString();
    file.close();
    LittleFS.remove(path.c_str());
    std::uint32_t generation = 0;
    if (!parseU32(toStd(raw).substr(0, toStd(raw).find('\n')), generation)) {
        // A corrupt marker must not block the scheduler.
        return true;
    }
    return deleteGeneration(generation, error);
}

bool CourseStore::recover(std::string& error) {
    error.clear();
    CalendarState state;
    bool present = false;
    if (!readCalendarIfPresent(state, present, error)) return false;
    if (!present) {
        error.clear();
        return true;
    }
    if (state.active_generation == 0 || state.course_length == 0) {
        error = "calendar has no active generation";
        return false;
    }
    if (!recoverBase(metaBase(state.active_generation), false, error)) {
        return false;
    }
    // A template generation cannot be validated before templates.json is
    // repaired, so it is restored right after meta (same tmp/bak rules).
    if (templatesFilePresent(state.active_generation) &&
        !recoverBase(templateBase(state.active_generation), true, error)) {
        return false;
    }
    for (std::uint8_t i = 0; i < state.course_length; ++i) {
        if (!recoverBase(dayBase(state.active_generation, i), true, error)) {
            return false;
        }
    }
    if (state.draft_generation != 0 &&
        state.draft_generation != state.active_generation) {
        // Draft recovery is best-effort: a broken draft must not stop the
        // active course from running.
        std::string ignored;
        recoverBase(metaBase(state.draft_generation), false, ignored);
        if (templatesFilePresent(state.draft_generation)) {
            recoverBase(templateBase(state.draft_generation), true, ignored);
        }
        for (std::uint8_t i = 0; i < state.course_length; ++i) {
            recoverBase(dayBase(state.draft_generation, i), true, ignored);
        }
    }
    // Recovery touched the files on disk; never keep a set loaded before it.
    clearTemplateCache();
    return validateGeneration(state.active_generation, state.course_length,
                              error);
}

namespace {

// Replaces writeFileWithCrc() for content that only exists as byte ranges in a
// caller-owned buffer (the import stream). The CRC is computed in a first pass
// over the same ranges, so nothing proportional to the file is allocated.
bool writePartsStreamed(const std::string& base, const CourseStore::RawPart* parts,
                        std::size_t count, std::string& error) {
    const std::string path = base + ".json";
    const std::string tmp = base + ".tmp";
    const std::string bak = base + ".bak";

    std::uint32_t crc = 0xFFFFFFFFu;
    std::size_t total = 0;
    for (std::size_t i = 0; i < count; ++i) {
        crc = crc32Update(crc, parts[i].data, parts[i].size);
        total += parts[i].size;
    }
    const std::uint32_t stored = ~crc;

    File file = LittleFS.open(tmp.c_str(), "w");
    if (!file) {
        error = "cannot create " + tmp;
        return false;
    }
    char header[24];
    const int headerLen = std::snprintf(header, sizeof(header), "CRC32 %08lx\n",
                                        static_cast<unsigned long>(stored));
    bool ok = headerLen > 0 &&
              file.write(reinterpret_cast<const uint8_t*>(header),
                         static_cast<std::size_t>(headerLen)) ==
                  static_cast<std::size_t>(headerLen);
    for (std::size_t i = 0; ok && i < count; ++i) {
        ok = file.write(reinterpret_cast<const uint8_t*>(parts[i].data), parts[i].size) ==
             parts[i].size;
    }
    file.close();
    if (!ok) {
        LittleFS.remove(tmp.c_str());
        error = "short write to " + tmp;
        return false;
    }

    if (LittleFS.exists(path.c_str())) {
        if (LittleFS.exists(bak.c_str())) LittleFS.remove(bak.c_str());
        if (!LittleFS.rename(path.c_str(), bak.c_str())) {
            LittleFS.remove(tmp.c_str());
            error = "cannot back up " + path;
            return false;
        }
    }
    if (!LittleFS.rename(tmp.c_str(), path.c_str())) {
        if (LittleFS.exists(bak.c_str())) LittleFS.rename(bak.c_str(), path.c_str());
        error = "cannot publish " + path;
        return false;
    }
    LittleFS.remove(bak.c_str());

    if (!verifyFileStreamed(path, stored, total, error)) {
        LittleFS.remove(path.c_str());
        return false;
    }
    return true;
}

}  // namespace

namespace {

// Writing (or deleting) one generation must only drop *its* cached set. The
// active slot may belong to another generation that is currently running: an
// import of a new draft must not force the running course to reload its
// templates at the next midnight tick.
void invalidateTemplatesFor(std::uint32_t generation) {
    if (generation == 0) return;
    if (templateCache().active.generation == generation) templateCache().active.clear();
    if (templateCache().draft.generation == generation) templateCache().draft.clear();
}

}  // namespace

bool CourseStore::writeTemplatesParts(std::uint32_t generation, const RawPart* parts,
                                      std::size_t count, std::string& error) {
    error.clear();
    if (generation == 0) {
        error = "invalid generation";
        return false;
    }
    if (parts == nullptr || count == 0) {
        error = "no template fragments";
        return false;
    }
    ensureGenerationDir(generation);
    if (!writePartsStreamed(templateBase(generation), parts, count, error)) return false;
    invalidateTemplatesFor(generation);
    return true;
}

bool CourseStore::writeTemplates(
    std::uint32_t generation,
    const std::map<std::string, std::vector<course::Event> >& templates,
    std::string& error) {
    error.clear();
    if (generation == 0) {
        error = "invalid generation";
        return false;
    }
    if (templates.size() > kMaxTemplates) {
        error = "too many templates";
        return false;
    }
    for (std::map<std::string, std::vector<course::Event> >::const_iterator it =
             templates.begin();
         it != templates.end(); ++it) {
        if (it->first.empty()) {
            error = "template name must not be empty";
            return false;
        }
        if (it->first.size() > kMaxTemplateNameBytes) {
            error = "template name too long";
            return false;
        }
    }
    std::size_t events = 0;
    const std::string json = course::toJsonTemplates(templates, events, error);
    if (json.empty()) return false;
    if (events > kMaxTemplateEvents) {
        error = "too many template events";
        return false;
    }
    ensureGenerationDir(generation);
    if (!writeAtomic(templateBase(generation), json, error)) return false;
    // Only this generation's cached set is stale; a running course keeps its own.
    invalidateTemplatesFor(generation);
    return true;
}

namespace {

// Loads the template set of `generation` into `slot`. On any error the slot is
// left empty, so a partial or foreign set can never be served.
bool loadIntoSlot(CourseStore& store, std::uint32_t generation, TemplateSet& slot,
                  std::string& error) {
    slot.clear();
    CourseMeta meta;
    if (!store.loadMeta(generation, meta, error)) return false;
    if (meta.layout != kLayoutTemplates) {
        // Expanded generation: nothing to resolve. The marker is still recorded
        // so loadDay() can tell "no templates required" from "not loaded".
        slot.generation = generation;
        slot.loaded = true;
        slot.events = 0;
        return true;
    }
    std::map<std::string, std::vector<course::Event> > items;
    std::size_t events = 0;
    if (!loadTemplateFile(generation, items, events, error)) {
        slot.clear();
        return false;
    }
    slot.items.swap(items);
    slot.events = events;
    slot.generation = generation;
    slot.loaded = true;
    return true;
}

}  // namespace

bool CourseStore::prepareTemplates(std::uint32_t generation, std::string& error) {
    error.clear();
    if (generation == 0) {
        error = "invalid generation";
        return false;
    }
    if (findTemplateSet(generation) != nullptr) return true;
    // Editor/browsing loads go to the draft slot: the active set is never
    // evicted, so a running schedule keeps resolving its own templates.
    return loadIntoSlot(*this, generation, templateCache().draft, error);
}

bool CourseStore::prepareActiveTemplates(std::uint32_t generation,
                                         std::string& error) {
    error.clear();
    if (generation == 0) {
        error = "invalid generation";
        return false;
    }
    TemplateCacheState& cache = templateCache();
    if (cache.active.loaded && cache.active.generation == generation) return true;
    if (!loadIntoSlot(*this, generation, cache.active, error)) return false;
    // A draft set of another generation stays where it is; it can no longer
    // shadow the active one because lookups match on generation.
    return true;
}

bool CourseStore::templatesReady(std::uint32_t generation) const {
    return findTemplateSet(generation) != nullptr;
}

void CourseStore::invalidateTemplates() { clearTemplateCache(); }


std::size_t CourseStore::cachedTemplateCount() const {
    const TemplateSet* set = activeOrDraftSet();
    return set == nullptr ? 0 : set->items.size();
}

std::size_t CourseStore::cachedTemplateEvents() const {
    const TemplateSet* set = activeOrDraftSet();
    return set == nullptr ? 0 : set->events;
}

std::uint32_t CourseStore::cachedTemplateGeneration() const {
    const TemplateSet* set = activeOrDraftSet();
    return set == nullptr ? 0 : set->generation;
}

namespace {

// A day reference is tiny: the events live once in templates.json. The
// generation id is stamped in so loadDay() can reject a file moved between
// generations.
std::string makeDayReferenceJson(std::uint32_t generation,
                                 const std::string& templateName) {
    return "{\"format\":\"gong-course\",\"version\":2,\"name\":\"day\",\"generation\":" +
           std::to_string(generation) + ",\"course_length\":1,\"days\":[{\"template\":\"" +
           escapeText(templateName) + "\"}]}";
}

// Collects the templates that are actually referenced by the days. Unused
// definitions are not stored: the on-flash size must follow the course, not
// the file it came from.
bool referencedTemplates(const course::Course& course,
                         std::map<std::string, std::vector<course::Event> >& out,
                         std::size_t& uniqueEvents, std::string& error) {
    out.clear();
    uniqueEvents = 0;
    for (std::size_t i = 0; i < course.days.size(); ++i) {
        const course::Day& day = course.days[i];
        if (day.hasOwnEvents) continue;
        if (day.templateName.empty()) {
            error = "day has neither template nor events";
            return false;
        }
        if (out.find(day.templateName) != out.end()) continue;
        std::map<std::string, std::vector<course::Event> >::const_iterator it =
            course.templates.find(day.templateName);
        if (it == course.templates.end()) {
            error = "unknown template: " + day.templateName;
            return false;
        }
        if (out.size() >= CourseStore::kMaxTemplates) {
            error = "too many templates";
            return false;
        }
        uniqueEvents += it->second.size();
        if (uniqueEvents > CourseStore::kMaxTemplateEvents) {
            error = "too many template events";
            return false;
        }
        out[it->first] = it->second;
    }
    return true;
}

}  // namespace

bool CourseStore::writeCompactGeneration(const course::Course& course,
                                         std::uint32_t& outGeneration,
                                         std::string& error) {
    error.clear();
    outGeneration = 0;
    if (!course::validate(course, error)) return false;
    if (course.days.empty() || course.days.size() > course::kMaxDays) {
        error = "course length out of range";
        return false;
    }

    std::map<std::string, std::vector<course::Event> > templates;
    std::size_t uniqueEvents = 0;
    if (!referencedTemplates(course, templates, uniqueEvents, error)) return false;
    if (templates.empty()) {
        // Nothing to deduplicate: the caller must use applyCourse().
        error = "course has no template references";
        return false;
    }

    std::uint32_t generation = 0;
    if (!createGeneration(generation, error)) return false;

    // templates.json first, days next, meta last: an interrupted build can
    // never look like a complete generation.
    if (!writeTemplates(generation, templates, error)) {
        std::string scratch;
        deleteGeneration(generation, scratch);
        return false;
    }
    for (std::size_t i = 0; i < course.days.size(); ++i) {
        const course::Day& day = course.days[i];
        const std::uint8_t index = static_cast<std::uint8_t>(i);
        const bool ok = day.hasOwnEvents
                            ? saveDayAtomic(generation, index, day, error)
                            : putDayReference(generation, index, day.templateName, error);
        if (!ok) {
            std::string scratch;
            deleteGeneration(generation, scratch);
            return false;
        }
    }

    if (!finishCompactGeneration(generation,
                                static_cast<std::uint8_t>(course.days.size()),
                                course.name, error)) {
        std::string scratch;
        deleteGeneration(generation, scratch);
        return false;
    }
    error.clear();
    outGeneration = generation;
    return true;
}

bool CourseStore::putDayReference(std::uint32_t generation, std::uint8_t day,
                                  const std::string& templateName,
                                  std::string& error) {
    error.clear();
    if (generation == 0 || day >= course::kMaxDays) {
        error = "day index out of range";
        return false;
    }
    if (templateName.empty()) {
        error = "template name must not be empty";
        return false;
    }
    if (templateName.size() > kMaxTemplateNameBytes) {
        error = "template name too long";
        return false;
    }
    return writeAtomic(dayBase(generation, day),
                       makeDayReferenceJson(generation, templateName), error);
}

bool CourseStore::finishCompactGeneration(std::uint32_t generation,
                                          std::uint8_t length,
                                          const std::string& name,
                                          std::string& error) {
    error.clear();
    if (generation == 0) {
        error = "invalid generation";
        return false;
    }
    if (length == 0 || length > course::kMaxDays) {
        error = "course length out of range";
        return false;
    }
    CourseMeta meta;
    meta.generation = generation;
    meta.course_length = length;
    meta.name = name;
    meta.layout = kLayoutTemplates;
    const std::string metaJson = makeMetaJson(meta, error);
    if (metaJson.empty() || !writeAtomic(metaBase(generation), metaJson, error)) {
        return false;
    }
    // Validates the layout marker, the template set and every day reference
    // against this generation only.
    return validateGeneration(generation, length, error);
}

bool CourseStore::applyCompactCourse(const course::Course& course,
                                     std::uint32_t& newGeneration,
                                     CalendarState& outState,
                                     std::string& error) {
    error.clear();
    CalendarState oldState;
    bool hasOld = false;
    if (!readCalendarIfPresent(oldState, hasOld, error)) return false;

    std::uint32_t generation = 0;
    if (!writeCompactGeneration(course, generation, error)) return false;

    CourseMeta meta;
    if (!loadMeta(generation, meta, error)) return false;

    // Same publish semantics as applyCourse(): APPLIED, no activation date,
    // no running flag, no automatic activation.
    CalendarState next;
    next.active_generation = generation;
    next.draft_generation = generation;
    next.activation_date = 0;
    next.day_index = 0;
    next.course_length = meta.course_length;
    next.flags = kApplied;
    next.failure_count = 0;
    if (!saveCalendar(next, error)) {
        std::string scratch;
        deleteGeneration(generation, scratch);
        return false;
    }

    if (hasOld) {
        if (oldState.active_generation > 0 &&
            oldState.active_generation != generation) {
            std::string scratch;
            deleteGeneration(oldState.active_generation, scratch);
        }
        if (oldState.draft_generation > 0 &&
            oldState.draft_generation != generation &&
            oldState.draft_generation != oldState.active_generation) {
            std::string scratch;
            deleteGeneration(oldState.draft_generation, scratch);
        }
    }
    error.clear();
    newGeneration = generation;
    outState = next;
    return true;
}

bool CourseStore::saveCompactDraft(const course::Course& course,
                                   std::uint32_t& draftGeneration,
                                   CalendarState& outState,
                                   std::string& error) {
    error.clear();
    CalendarState oldState;
    bool hasOld = false;
    if (!readCalendarIfPresent(oldState, hasOld, error)) return false;

    std::uint32_t generation = 0;
    if (!writeCompactGeneration(course, generation, error)) return false;

    CourseMeta meta;
    if (!loadMeta(generation, meta, error)) return false;

    CalendarState next = oldState;
    next.draft_generation = generation;
    if (!hasOld) {
        next.active_generation = 0;
        next.activation_date = 0;
        next.day_index = 0;
        next.course_length = meta.course_length;
        next.flags = 0;
        next.failure_count = 0;
    }
    if (!saveCalendar(next, error)) {
        std::string scratch;
        deleteGeneration(generation, scratch);
        return false;
    }
    if (hasOld && oldState.draft_generation > 0 &&
        oldState.draft_generation != generation &&
        oldState.draft_generation != oldState.active_generation) {
        std::string scratch;
        deleteGeneration(oldState.draft_generation, scratch);
    }
    error.clear();
    draftGeneration = generation;
    outState = next;
    return true;
}

namespace {

// Export cursor over templates.json. It parses the file with the same rules as
// the cache loader but keeps nothing: the template cache is left untouched, a
// day edit or a running course is not disturbed, and only one raw file plus one
// fragment is alive at a time.
struct TemplateExportState {
    bool active;
    std::string raw;
    std::vector<TemplateSlice> slices;
    std::size_t next;

    TemplateExportState() : active(false), next(0) {}
};

TemplateExportState& templateExport() {
    static TemplateExportState state;
    return state;
}

void clearTemplateExport() {
    TemplateExportState& state = templateExport();
    state.active = false;
    state.raw.clear();
    state.raw.shrink_to_fit();
    state.slices.clear();
    state.slices.shrink_to_fit();
    state.next = 0;
}

}  // namespace

bool CourseStore::templateExportNames(std::vector<std::string>& out) const {
    out.clear();
    const TemplateExportState& state = templateExport();
    if (!state.active) return false;
    for (std::size_t i = 0; i < state.slices.size(); ++i) {
        out.push_back(state.slices[i].name);
    }
    return true;
}

bool CourseStore::loadDayDescriptor(std::uint32_t generation, std::uint8_t day,
                                    bool& isReference, std::string& templateName,
                                    std::vector<course::Event>& events,
                                    std::string& error) {
    error.clear();
    isReference = false;
    templateName.clear();
    events.clear();
    if (generation == 0 || day >= course::kMaxDays) {
        error = "day index out of range";
        return false;
    }
    std::string json;
    if (!readFileWithCrc(dayBase(generation, day) + ".json", json, error)) return false;
    course::Course one;
    if (!course::fromJsonUnresolved(json, one, error)) return false;
    if (one.generation != generation) {
        error = "day generation mismatch";
        return false;
    }
    if (one.days.size() != 1) {
        error = "day file must contain one day";
        return false;
    }
    const course::Day& parsed = one.days[0];
    if (parsed.hasOwnEvents) {
        events = parsed.events;
        return true;
    }
    if (parsed.templateName.empty()) {
        error = "day has neither template nor events";
        return false;
    }
    isReference = true;
    templateName = parsed.templateName;
    return true;
}

bool CourseStore::openTemplateExport(std::uint32_t generation, std::string& error) {
    error.clear();
    clearTemplateExport();
    if (generation == 0) {
        error = "invalid generation";
        return false;
    }
    TemplateExportState& state = templateExport();
    if (!readFileWithCrc(templateBase(generation) + ".json", state.raw, error)) return false;
    TemplateScanner scanner(state.raw);
    if (!scanner.scan(state.slices, error)) {
        clearTemplateExport();
        return false;
    }
    state.next = 0;
    state.active = true;
    return true;
}

bool CourseStore::nextTemplateExportChunk(std::string& chunk, bool& done,
                                          std::string& error) {
    error.clear();
    done = false;
    chunk.clear();
    TemplateExportState& state = templateExport();
    if (!state.active) {
        error = "template export not started";
        return false;
    }
    if (state.next >= state.slices.size()) {
        clearTemplateExport();
        done = true;
        return true;
    }
    const TemplateSlice& slice = state.slices[state.next++];
    chunk.reserve(slice.name.size() + slice.length + 8);
    chunk += '"';
    chunk += escapeText(slice.name);
    chunk += "\":";
    chunk.append(state.raw, slice.begin, slice.length);
    return true;
}

bool CourseStore::loadLastFire(LastFire& out, std::string& error) {
    error.clear();
    Preferences prefs;
    if (!prefs.begin(kNamespace, true)) {
        error = "cannot open NVS";
        return false;
    }
    if (!prefs.isKey(kLastFireKey)) {
        prefs.end();
        error = "last_fire not found";
        return false;
    }
    const String raw = prefs.getString(kLastFireKey, "");
    prefs.end();
    return readLastFireBlob(toStd(raw), out, error);
}

bool CourseStore::saveLastFire(const LastFire& value, std::string& error) {
    error.clear();
    const std::string blob = makeLastFireBlob(value);
    Preferences prefs;
    if (!prefs.begin(kNamespace, false)) {
        error = "cannot open NVS";
        return false;
    }
    const size_t written = prefs.putString(kLastFireKey, toArduino(blob));
    prefs.end();
    if (written != blob.size()) {
        error = "last_fire NVS write failed";
        return false;
    }
    return true;
}

bool CourseStore::clearLastFire(std::string& error) {
    error.clear();
    Preferences prefs;
    if (!prefs.begin(kNamespace, false)) {
        error = "cannot open NVS";
        return false;
    }
    if (!prefs.isKey(kLastFireKey)) {
        prefs.end();
        return true;
    }
    const bool removed = prefs.remove(kLastFireKey);
    prefs.end();
    if (!removed) {
        error = "last_fire NVS remove failed";
        return false;
    }
    return true;
}

}  // namespace course_store
