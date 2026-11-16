#pragma once

#include <stdint.h>

namespace active_day {

struct Record {
    uint8_t day = 0;
    uint32_t date = 0;  // YYYYMMDD; zero is explicitly unbound.
};

enum class Status { Boot, Empty, Loaded, Ready, Unknown, Invalid };
enum class Source { Error, Empty, Active, Legacy };
enum class FilePresence { Present, ConfirmedAbsent, Error };
enum class Phase { Idle, SchemaSet, SchemaCommitted, Prepared, SetAcked, CommitAcked, Verified };

bool validDate(uint32_t date);
bool parseDate(const char* text, uint32_t& date);
void formatDate(uint32_t date, char (&out)[11]);
bool encode(const Record& record, uint8_t dayCount, uint64_t& raw);
bool decode(uint64_t raw, uint8_t dayCount, Record& record);
// Top-level schedule paths on the default LittleFS.begin(false) mount.
FilePresence probeFile(const char* path);

// Callers serialize access with the scheduler's state mutex. No global NVS
// initialization, erasure, legacy deletion or background retries live here.
class Store {
public:
    explicit Store(uint8_t dayCount) : dayCount_(dayCount) {}
    Source load(bool filesystemAvailable, Record& record);
    bool readActive(Record& record, uint64_t& raw);
    bool persist(const Record& record, bool recovery = false);
    void published() noexcept { status_ = Status::Ready; phase_ = Phase::Idle; }
    void unknown() noexcept { status_ = Status::Unknown; phase_ = Phase::Idle; }
    Status status() const { return status_; }
    Phase phase() const { return phase_; }

private:
    bool classify(int error);
    Source readLegacyFile(bool filesystemAvailable, Record& record);
    uint8_t dayCount_;
    Status status_ = Status::Boot;
    Phase phase_ = Phase::Idle;
    bool schemaPresent_ = false;
};

}  // namespace active_day
