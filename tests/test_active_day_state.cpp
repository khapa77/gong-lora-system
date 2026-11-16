// Tests the CURRENT production translation unit, not a rewritten scheduler.
// Target-specific clocks, NVS, FS and audio are replaced only in this host build.
#include "host/Arduino.h"
#include "host/LittleFS.h"
#include "host/Preferences.h"
#include <sys/stat.h>
#include <ArduinoJson.h>
#include <cstdlib>
#include <iostream>
#include <stdexcept>

extern "C" { bool gong_allocation_forbidden=false; }
void* operator new(size_t n) {
    if (host::forbidAllocation()) std::abort();
    if (host::failNewAfter()==0) { host::failNewAfter()=-1; throw std::bad_alloc(); }
    if (host::failNewAfter()>0) --host::failNewAfter();
    if (void* p=std::malloc(n?n:1)) return p;
    throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void* operator new[](size_t n) { return ::operator new(n); }
void operator delete[](void* p) noexcept { ::operator delete(p); }
// The regular build fences C++ new and the injected JSON pool. --malloc-fence
// also intercepts malloc/calloc/realloc on macOS. UBSan alone is not a C fence;
// the hook below is available when a working ASan runtime calls it.
extern "C" __attribute__((no_sanitize("address", "undefined")))
void __sanitizer_malloc_hook(const volatile void*, size_t) {
    if (host::forbidAllocation()) std::abort();
}

void convertToJson(const String& src, ArduinoJson::JsonVariant dst) {
    dst.set(static_cast<const std::string&>(src));
}
#define time host_time
#include "../server/src/schedule.cpp"
#undef time

static unsigned int audioCalls=0;
static std::vector<int> audioTracks;
static bool audioPlaying=false;
static bool filesystemAvailable=true;
static unsigned lockDepth=0;
void logPrintf(const char*, ...) {}
bool mp3_isPlaying() { return audioPlaying; }
bool sys_fsOk() { return filesystemAvailable; }
void sys_lock() { ++lockDepth; }
void sys_unlock() { if (!lockDepth) std::abort(); --lockDepth; }
bool mp3_trackExists(uint8_t track) { return track==1 || track==2; }
static void audioCallback(uint8_t track,uint8_t,uint8_t) {
    ++audioCalls; audioTracks.push_back(track);
}
static time_t at(int day,int hour=12,int minute=0,int second=0) {
    struct tm t={}; t.tm_year=126; t.tm_mon=10; t.tm_mday=day;
    t.tm_hour=hour; t.tm_min=minute; t.tm_sec=second; t.tm_isdst=-1;
    return mktime(&t);
}
static void require(bool ok,const std::string& why) {
    if (!ok) throw std::runtime_error(why);
}
static const host::Cell* durable(const char* key) {
    const auto it=host::nvs().durable.find(std::string("gong/")+key);
    return it==host::nvs().durable.end()?nullptr:&it->second;
}
static void resetRam() {
    for (auto& e:entries) e=ScheduleEntry{};
    count=0; nextId=1; activeDay=-1; activeDate="";
    scheduleFileState=ScheduleFileState::Unprepared;
    loadedRecord=active_day::Record(); loadedSource=active_day::Source::Error;
    schedPendingSave=false; lastFireTs=0; lastFiredKey=-1;
    activeStore=active_day::Store(DAY_COUNT); executionRunning=false; publishedActive=0;
    lastFiredMillis=0; lastTimeLog=0; firePrefs=Preferences();
    onScheduleTrigger=audioCallback;
}
static void fixture() {
    setenv("TZ","MSK-3",1); tzset();
    host::forbidAllocation()=false; host::fenceTriggered()=false;
    host::failNewAfter()=-1; host::failStrings()=false; host::failReserve()=false;
    host::failStringCopies()=false; host::failJsonPool()=false;
    host::nvs()=host::NvsModel(); host::files().clear(); host::fs()=host::FsFaults();
    host::clock()=at(16); host::ticks()=100000; resetRam();
    audioPlaying=false; filesystemAvailable=true; lockDepth=0;
    for (int day=0;day<DAY_COUNT;++day) {
        char path[16]; dayPath(day,path);
        host::files()[path]="[{\"id\":1,\"hour\":4,\"min\":0,\"track\":1,"
            "\"loop\":1,\"vol\":30,\"en\":true,\"desc\":\"day"+std::to_string(day)+"\"}]";
    }
    host::nvs().seed("lastfire",host::Cell::integer(host::Type::U32,1700000011U));
}
static void legacy(int day=3,const char* date="2026-11-16") {
    host::nvs().seed("aday",host::Cell::integer(host::Type::I32,static_cast<uint32_t>(day)));
    host::nvs().seed("adate",host::Cell::string(date));
}
static void bootAgain() {
    host::nvs().reboot(); resetRam(); sched_setup();
}
static bool liveIs(int day,const char* date) {
    return activeDay==day && activeDate==date && count==1 &&
        entries[0].description==String("day"+std::to_string(day));
}
// Independent fixture encoding of the accepted wire contract.
static uint64_t record(unsigned int day,uint32_t date,unsigned int version=1) {
    return (static_cast<uint64_t>(version)<<56) |
        (static_cast<uint64_t>(day)<<48) | date;
}
static void cutDuringSwitch() {
    host::clock()=at(17);
    host::nvs().inject("commit","@state",host::Action::CutAfter);
    bool interrupted=false;
    try { sched_activateDay(4); } catch (const host::PowerCut&) { interrupted=true; }
    require(interrupted,"fixture did not reach the active-day commit");
}
static void control_legacy_load() {
    legacy(); sched_setup(); require(liveIs(3,"2026-11-16"),"legacy state not loaded");
}
static void control_unbound_date_without_time() {
    legacy(3,""); host::clock()=0; sched_setup();
    require(liveIs(3,""),"an unbound legacy date must stay unbound without valid time");
}
static void control_first_boot() {
    sched_setup(); require(liveIs(0,"2026-11-16"),"genuinely empty state should activate Day 00");
}
static void control_successful_switch() {
    legacy(); sched_setup(); host::clock()=at(17);
    require(sched_activateDay(4) && liveIs(4,"2026-11-17"),"normal activation failed");
}
static void regression_power_cut_pair() {
    legacy(); sched_setup(); cutDuringSwitch();
    host::nvs().reboot(); resetRam(); firePrefs.begin("gong",false); loadActiveDay();
    const bool wholeOld=activeDay==3 && activeDate=="2026-11-16";
    const bool wholeNew=activeDay==4 && activeDate=="2026-11-17";
    require(wholeOld || wholeNew,"mixed recovered pair: day="+std::to_string(activeDay)+
        " date="+activeDate.c_str());
}
static void regression_double_advance() {
    legacy(); sched_setup(); cutDuringSwitch(); bootAgain();
    require(liveIs(4,"2026-11-17"),"course advanced twice after one date change: day="+
        std::to_string(activeDay)+" date="+activeDate.c_str());
}
static void regression_ram_before_persistence() {
    legacy(); sched_setup(); host::clock()=at(17); bool oldAtWrite=true;
    host::nvs().beforeWrite=[&]() { oldAtWrite=oldAtWrite && liveIs(3,"2026-11-16"); };
    sched_activateDay(4);
    require(oldAtWrite,"live schedule/day/date were published before the first NVS write");
}
static void regression_set_failure() {
    legacy(); sched_setup(); host::clock()=at(17);
    host::nvs().inject("set","@state",host::Action::ErrorBefore);
    const bool ok=sched_activateDay(4);
    require(!ok,"activation reported success after nvs_set failure");
    require(liveIs(3,"2026-11-16"),"failed write published the candidate state");
}
static void regression_commit_error_old() {
    legacy(); sched_setup(); host::clock()=at(17);
    host::nvs().inject("commit","@state",host::Action::ErrorBefore);
    require(!sched_activateDay(4),"commit failure with old durable value was ignored");
}
static void regression_commit_error_new() {
    legacy(); sched_setup(); host::clock()=at(17);
    host::nvs().inject("commit","@state",host::Action::ErrorDurable);
    require(!sched_activateDay(4),"an error ACK was treated as success even though durability is uncertain");
}
static void regression_no_automatic_progress_after_uncertain_write() {
    legacy(); sched_setup(); host::clock()=at(17);
    host::nvs().inject("commit","@state",host::Action::ErrorDurable);
    sched_activateDay(4);
    const unsigned int writes=host::nvs().writes;
    host::clock()=at(18);
    for (int i=0;i<20;++i) checkDateAdvance();
    require(host::nvs().writes==writes,"automatic course writes continued after an uncertain write");
}
static void regression_orphan_date() {
    host::nvs().seed("adate",host::Cell::string("2026-11-16")); sched_setup();
    require(sched_hasError() && count==0,"orphan adate silently became a working Day 00");
}
static void regression_invalid_legacy_day() {
    legacy(-2); sched_setup();
    require(sched_hasError() && count==0,"invalid negative day silently became a working Day 00");
}
static void regression_invalid_legacy_date() {
    legacy(3,"2026-02-31"); sched_setup();
    require(sched_hasError() && count==0,"invalid legacy calendar date was silently accepted/restamped");
}
static void regression_open_error() {
    legacy(); host::nvs().inject("open","gong",host::Action::ErrorBefore); sched_setup();
    require(sched_hasError() && count==0,"NVS open error was treated as empty storage");
}
static void regression_read_error() {
    legacy(); host::nvs().inject("get","aday",host::Action::ErrorBefore); sched_setup();
    require(sched_hasError() && count==0,"NVS read error was treated as empty storage");
}
static void contract_migration_retains_legacy() {
    legacy(); sched_setup();
    require(durable("active") && durable("active")->type==host::Type::U64 &&
        durable("active")->number==record(3,20261116),"legacy state was not migrated to the versioned active key");
    require(durable("aday")->number==3 && durable("adate")->text=="2026-11-16",
        "migration changed the legacy keys");
}
static void contract_active_precedence() {
    legacy(); host::nvs().seed("active",host::Cell::integer(host::Type::U64,record(4,20261116)));
    sched_setup(); require(liveIs(4,"2026-11-16"),"valid active key did not take precedence over stale legacy keys");
}
static void contract_unknown_version() {
    legacy(); host::nvs().seed("active",host::Cell::integer(host::Type::U64,record(4,20261116,2)));
    sched_setup(); require(sched_hasError() && count==0,"unknown active version silently fell back to legacy state");
}
static void contract_wrong_type() {
    legacy(); host::nvs().seed("active",host::Cell::string("not a uint64")); sched_setup();
    require(sched_hasError() && count==0,"wrong active key type silently fell back to legacy state");
}
static void contract_bad_record_no_day_zero() {
    host::nvs().seed("active",host::Cell::integer(host::Type::U64,record(99,20261116)));
    sched_setup(); require(sched_hasError() && count==0,"invalid active record silently became a working Day 00");
}
static void regression_file_import_failure_keeps_source() {
    host::files()["/activeday.conf"]="{\"day\":3,\"date\":\"2026-11-16\"}";
    host::nvs().inject("set","@state",host::Action::ErrorBefore); sched_setup();
    require(!host::nvs().fault.armed,"fixture did not reach the legacy import write");
    require(LittleFS.exists("/activeday.conf"),"legacy file removed despite failed NVS import");
}
static void contract_manual_retry_same_target() {
    legacy(); sched_setup(); host::clock()=at(17);
    host::nvs().inject("commit","@state",host::Action::ErrorDurable);
    require(!sched_activateDay(4),"uncertain activation must first report failure");
    const unsigned int writes=host::nvs().writes;
    for (int i=0;i<20;++i) checkDateAdvance();
    require(host::nvs().writes==writes,"latched error retried automatically");
    require(sched_recover(record(4,20261117)) && liveIs(4,"2026-11-17"),"explicit same-target recovery failed");
    require(!sched_isRunning(),"recovery must leave execution paused");
    require(sched_resume(),"explicit resume failed");
    checkDateAdvance();
    require(liveIs(4,"2026-11-17"),"manual recovery advanced the course a second time");
}
static void control_backward_date() {
    legacy(); sched_setup(); host::clock()=at(15); checkDateAdvance();
    require(liveIs(3,"2026-11-15"),"backward date changed the course day");
}
static void control_forward_skip() {
    legacy(); sched_setup(); host::clock()=at(19); checkDateAdvance();
    require(liveIs(6,"2026-11-19"),"forward date did not preserve elapsed-day advancement");
}
static void control_last_day() {
    legacy(DAY_COUNT-1); sched_setup(); host::clock()=at(17); checkDateAdvance();
    require(liveIs(DAY_COUNT-1,"2026-11-17"),"last course day changed unexpectedly");
}
static void control_cut_after_complete_pair() {
    legacy(); sched_setup(); host::clock()=at(17);
    host::nvs().inject("commit","@state",host::Action::CutAfter); bool cut=false;
    try { sched_activateDay(4); } catch (const host::PowerCut&) { cut=true; }
    require(cut,"fixture did not reach complete-state commit"); bootAgain();
    require(liveIs(4,"2026-11-17"),"reboot after a fully saved pair changed the selected day");
}
static void control_lastfire_untouched() {
    legacy(); sched_setup(); host::clock()=at(17); sched_activateDay(4);
    require(durable("lastfire") && durable("lastfire")->number==1700000011U,
        "day activation changed lastfire");
}
static void control_a01_same_minute() {
    legacy(); host::clock()=at(16,4,0,20);
    host::nvs().seed("lastfire",host::Cell::integer(host::Type::U32,at(16,4,0)));
    sched_setup(); sched_check(); require(audioCalls==0,"A01 reboot deduplication regressed");
}
static void control_a01_next_minute() {
    legacy(); host::clock()=at(16,4,0,20);
    host::files()["/day03.conf"]="[{\"hour\":4,\"min\":0,\"track\":1},{\"hour\":4,\"min\":1,\"track\":2}]";
    host::nvs().seed("lastfire",host::Cell::integer(host::Type::U32,at(16,4,0)));
    sched_setup(); sched_check(); host::clock()=at(16,4,1); sched_check();
    require(audioTracks==std::vector<int>({2}),"A01 suppressed the next minute or repeated the prior gong");
}
static void model_commit_error(bool newDurable) {
    const uint64_t old=record(3,20261116), next=record(4,20261117);
    host::nvs().seed("active",host::Cell::integer(host::Type::U64,old));
    nvs_handle_t h; require(nvs_open("gong",NVS_READWRITE,&h)==ESP_OK,"model open failed");
    require(nvs_set_u64(h,"active",next)==ESP_OK,"model set failed");
    host::nvs().inject("commit","active",newDurable?host::Action::ErrorDurable:host::Action::ErrorBefore);
    require(nvs_commit(h)!=ESP_OK,"model did not inject a commit error"); host::nvs().reboot();
    require(durable("active")->number==(newDurable?next:old),"incorrect model durable outcome");
}
static void model_commit_old() { model_commit_error(false); }
static void model_commit_new() { model_commit_error(true); }
static void model_set_error(bool newDurable) {
    const uint64_t old=record(3,20261116), next=record(4,20261117);
    host::nvs().seed("active",host::Cell::integer(host::Type::U64,old));
    nvs_handle_t h; nvs_open("gong",NVS_READWRITE,&h);
    host::nvs().inject("set","active",newDurable?host::Action::ErrorDurable:host::Action::ErrorBefore);
    require(nvs_set_u64(h,"active",next)!=ESP_OK,"model did not inject set error");
    host::nvs().reboot();
    require(durable("active")->number==(newDurable?next:old),"incorrect set-error durability outcome");
}
static void model_set_old() { model_set_error(false); }
static void model_set_new() { model_set_error(true); }
static void model_cut_before_commit() {
    const uint64_t old=record(3,20261116), next=record(4,20261117);
    host::nvs().seed("active",host::Cell::integer(host::Type::U64,old));
    nvs_handle_t h; nvs_open("gong",NVS_READWRITE,&h); nvs_set_u64(h,"active",next);
    host::nvs().inject("commit","active",host::Action::CutBefore); bool cut=false;
    try { nvs_commit(h); } catch (const host::PowerCut&) { cut=true; }
    host::nvs().reboot();
    require(cut && durable("active")->number==old,"uncommitted model value survived reboot");
}
static void model_read_error() {
    host::nvs().seed("active",host::Cell::integer(host::Type::U64,record(3,20261116)));
    nvs_handle_t h; nvs_open("gong",NVS_READWRITE,&h);
    host::nvs().inject("get","active",host::Action::ErrorBefore); uint64_t value=0;
    require(nvs_get_u64(h,"active",&value)==ESP_FAIL,"model did not inject read failure");
}
static void model_read_mismatch() {
    const uint64_t expected=record(3,20261116);
    host::nvs().seed("active",host::Cell::integer(host::Type::U64,expected));
    nvs_handle_t h; nvs_open("gong",NVS_READWRITE,&h);
    host::nvs().inject("get","active",host::Action::ReadMismatch); uint64_t value=0;
    require(nvs_get_u64(h,"active",&value)==ESP_OK && value!=expected,"model did not inject readback mismatch");
}
static void model_cut_after_commit() {
    const uint64_t next=record(4,20261117);
    nvs_handle_t h; nvs_open("gong",NVS_READWRITE,&h); nvs_set_u64(h,"active",next);
    host::nvs().inject("commit","active",host::Action::CutAfter); bool cut=false;
    try { nvs_commit(h); } catch (const host::PowerCut&) { cut=true; }
    host::nvs().reboot();
    require(cut && durable("active") && durable("active")->number==next,"commit/publish cut was not preserved by model");
}
struct Test { std::string group; std::string name; std::function<void()> run; };
static std::vector<Test> tests={
    {"control","legacy_load",control_legacy_load},
    {"control","unbound_date_without_time",control_unbound_date_without_time},
    {"control","first_boot",control_first_boot},
    {"control","successful_switch",control_successful_switch},
    {"regression","power_cut_pair",regression_power_cut_pair},
    {"regression","double_advance",regression_double_advance},
    {"regression","ram_before_persistence",regression_ram_before_persistence},
    {"regression","set_failure",regression_set_failure},
    {"regression","commit_error_old",regression_commit_error_old},
    {"regression","commit_error_new",regression_commit_error_new},
    {"regression","no_auto_progress_after_uncertain_write",regression_no_automatic_progress_after_uncertain_write},
    {"regression","orphan_date",regression_orphan_date},
    {"regression","invalid_legacy_day",regression_invalid_legacy_day},
    {"regression","invalid_legacy_date",regression_invalid_legacy_date},
    {"regression","open_error",regression_open_error},
    {"regression","read_error",regression_read_error},
    {"contract","migration_retains_legacy",contract_migration_retains_legacy},
    {"contract","active_precedence",contract_active_precedence},
    {"contract","unknown_version",contract_unknown_version},
    {"contract","wrong_type",contract_wrong_type},
    {"contract","bad_record_no_day_zero",contract_bad_record_no_day_zero},
    {"regression","file_import_failure_keeps_source",regression_file_import_failure_keeps_source},
    {"contract","manual_retry_same_target",contract_manual_retry_same_target},
    {"control","backward_date",control_backward_date},
    {"control","forward_skip",control_forward_skip},
    {"control","last_day",control_last_day},
    {"control","cut_after_complete_pair",control_cut_after_complete_pair},
    {"control","lastfire_untouched",control_lastfire_untouched},
    {"control","a01_same_minute",control_a01_same_minute},
    {"control","a01_next_minute",control_a01_next_minute},
    {"model","commit_error_old",model_commit_old},
    {"model","commit_error_new",model_commit_new},
    {"model","set_error_old",model_set_old},
    {"model","set_error_new",model_set_new},
    {"model","cut_before_commit",model_cut_before_commit},
    {"model","read_error",model_read_error},
    {"model","read_mismatch",model_read_mismatch},
    {"model","cut_after_commit",model_cut_after_commit}
};
#include "active_day_cases.h"
#include "active_day_review_cases.h"
int main(int argc,char** argv) {
    if (argc==3 && std::string(argv[1])=="--probe-allocation") {
        const std::string kind=argv[2];
        void* previous=std::malloc(8);
        host::forbidAllocation()=true;
        void* result=nullptr;
        if (kind=="malloc") result=std::malloc(32);
        if (kind=="calloc") result=std::calloc(2,32);
        if (kind=="realloc") result=std::realloc(previous,64);
        if (kind=="new") result=::operator new(32);
        host::forbidAllocation()=false;
        std::free(result);
        if (kind!="realloc") std::free(previous);
        return 3;  // reaching here means the fence did NOT stop the allocation
    }
    registerA021Tests(tests);
    registerReviewCases(tests);
    if (argc==2 && std::string(argv[1])=="--list") {
        for (const auto& t:tests) std::cout<<t.group<<"/"<<t.name<<"\n";
        return 0;
    }
    if (argc!=2) return 2;
    for (const auto& t:tests) {
        const std::string name=std::string(t.group)+"/"+t.name;
        if (name!=argv[1]) continue;
        try { fixture(); t.run(); std::cout<<"PASS "<<name<<"\n"; return 0; }
        catch (const std::exception& e) {
            host::forbidAllocation()=false;
            std::cout<<"FAIL "<<name<<": "<<e.what()<<"\n";
            return 1;
        }
    }
    return 2;
}
