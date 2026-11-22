#pragma once
#include <type_traits>
// Review regressions use the current production TU and the existing host seams.
template<class T> static bool zeroRepresentation(const T& value) {
    const unsigned char* bytes=reinterpret_cast<const unsigned char*>(&value);
    for (size_t i=0;i<sizeof(T);++i) if (bytes[i]) return false;
    return true;
}
static void requireEmptyEntry(const ScheduleEntry& entry) {
    // Inspect scalar object representations, never evaluate an indeterminate bool.
    require(zeroRepresentation(entry.id) && zeroRepresentation(entry.hour) && zeroRepresentation(entry.minute) &&
            zeroRepresentation(entry.track) && zeroRepresentation(entry.loop) && zeroRepresentation(entry.vol) &&
            zeroRepresentation(entry.enabled) && entry.description.length()==0,"uninitialized/stale entry outside count");
}
static void stageReplacementCourse() {
    for (int day=0;day<DAY_COUNT;++day) {
        char path[16]; stagePath(day,path);
        host::files()[path]="[{\"id\":1,\"hour\":5,\"min\":0,\"track\":2,\"desc\":\"replacement\"}]";
    }
}
static void courseReloadFailure() {
    readyActive(); stageReplacementCourse();
    host::fs().failRead=true; host::fs().path="/day03.conf";
    String error;
    (void)sched_commitCourse(error);
    host::fs().failRead=false;
}
static void registerReviewCases(std::vector<Test>& list) {
    const std::vector<std::string> nulDates={"2026-11-16\\u0000junk","\\u0000bad","2026-11-16\\u0000"};
    for (size_t i=0;i<nulDates.size();++i) addCase(list,"T04","legacy_nul_date_"+std::to_string(i),[nulDates,i]() {
        host::clock()=0;
        host::files()["/activeday.conf"]="{\"day\":3,\"date\":\""+nulDates[i]+"\"}";
        const auto files=host::files(); sched_setup();
        require(sched_persistenceStatus()==active_day::Status::Invalid && !sched_isRunning() && count==0,
                "embedded NUL accepted as truncated/unbound legacy date");
        require(host::nvs().writes==0 && host::files()==files,"invalid NUL date migrated/changed source");
    });
    for (int error : {EIO,ENOMEM,EACCES,EMFILE,ENODEV,EINVAL,ENOTDIR})
        for (bool present : {false,true})
            addCase(list,"T02","legacy_stat_error_"+std::to_string(error)+(present?"_present":"_absent"),[error,present]() {
                if (present) host::files()["/activeday.conf"]="{\"day\":3,\"date\":\"2026-11-16\"}";
                const auto files=host::files();
                host::fs().statError=error; host::fs().statErrorPath="/activeday.conf";
                sched_setup(); assertUnknown();
                require(host::nvs().writes==0 && host::fs().writes==0 && host::files()==files && activeDay==-1,
                        "ambiguous legacy lookup activated/wrote Day 00");
            });
    for (const std::string mode : {"root_missing","root_io","legacy_directory","day_directory","day_probe","seed_probe","day_unreadable","seed_unreadable","day_create_error"})
        addCase(list,"T02","boot_presence_"+mode,[mode]() {
            if (mode=="root_missing") host::fs().rootMissing=true;
            if (mode=="root_io") { host::fs().statError=EIO; host::fs().statErrorPath="/"; }
            if (mode=="legacy_directory") host::fs().directoryPath="/activeday.conf";
            if (mode=="day_directory") host::fs().directoryPath="/day00.conf";
            if (mode=="day_probe") { host::fs().statError=EIO; host::fs().statErrorPath="/day00.conf"; }
            if (mode=="seed_probe") {
                host::files().erase("/day00.conf"); host::files()[SCHEDULE_FILE]="[]";
                host::fs().statError=EIO; host::fs().statErrorPath=SCHEDULE_FILE;
            }
            if (mode=="day_unreadable") { host::fs().failRead=true; host::fs().path="/day00.conf"; }
            if (mode=="seed_unreadable") {
                host::files().erase("/day00.conf"); host::files()[SCHEDULE_FILE]="[]";
                host::fs().failRead=true; host::fs().path=SCHEDULE_FILE;
            }
            if (mode=="day_create_error") {
                host::files().erase("/day00.conf"); host::fs().failWrite=true; host::fs().path="/day00.conf";
            }
            const auto files=host::files(); sched_setup(); assertUnknown();
            require(host::nvs().writes==0 && host::fs().writes==0 && host::files()==files,
                    "unconfirmed absence created/replaced boot files");
        });
    for (bool readError : {false,true}) addCase(list,"T07",readError?"activation_present_but_unreadable":"activation_stat_error",[readError]() {
        readyActive(); const auto files=host::files(); const unsigned writes=host::nvs().writes;
        if (readError) { host::fs().failRead=true; host::fs().path="/day04.conf"; }
        else { host::fs().statError=EIO; host::fs().statErrorPath="/day04.conf"; }
        require(!sched_activateDay(4),"ambiguous presence permitted activation");
        require(host::files()==files && host::fs().writes==0 && host::nvs().writes==writes,
                "ambiguous exists truncated an existing day file");
        require(liveIs(3,"2026-11-16") && sched_isRunning(),"failed preparation replaced/quarantined consistent live day");
    });
    addCase(list,"T03","confirmed_absent_day_can_be_created",[]() {
        host::files().erase("/day00.conf"); sched_setup();
        require(sched_isRunning() && activeDay==0 && host::files()["/day00.conf"]=="[]",
                "confirmed absence no longer allows first boot");
    });
    addCase(list,"model","stat_distinguishes_absence_error_and_presence",[]() {
        struct stat info;
        errno=EIO; require(::stat("/littlefs/day03.conf",&info)==0 && S_ISREG(info.st_mode),"PRESENT stat failed");
        require(::stat("/littlefs/activeday.conf",&info)==-1 && errno==ENOENT,"absence not ENOENT");
        host::fs().statError=EIO; host::fs().statErrorPath="/activeday.conf";
        require(::stat("/littlefs/activeday.conf",&info)==-1 && errno==EIO,"I/O error confused with ENOENT");
    });
    for (const std::string failure : {"later","active_fallback","active_remove"})
        for (const std::string operation : {"state","tick","catchup","advance","save","resume"})
            addCase(list,"T08","partial_import_"+failure+"_"+operation,[failure,operation]() {
                readyActive(); stageReplacementCourse();
                host::fs().renameErrorPath=failure=="later"?"/day04.new":"/day03.new";
                if (failure=="active_remove") host::fs().removeErrorPath="/day03.conf";
                String error;
                require(!sched_commitCourse(error) && error.length(),"rename fault did not reject import");
                require(host::fs().renames>0,"rename fault was not reached");
                if (failure=="later") require(host::files()["/day03.conf"].find("replacement")!=std::string::npos,
                                             "active file was not replaced before later error");
                if (failure=="active_fallback") require(!host::files().count("/day03.conf"),
                                                       "fallback did not remove active file");
                const auto files=host::files();
                const unsigned writes=host::fs().writes,nvsWrites=host::nvs().writes;
                if (operation=="tick") { host::clock()=at(16,4); sched_check(); }
                if (operation=="catchup") { host::clock()=at(16,4,0,20); sched_catchup(); }
                if (operation=="advance") { host::clock()=at(17,4); checkDateAdvance(); }
                if (operation=="save") { sched_save(); require(!sched_saveNow(),"partial import saved stale RAM"); }
                if (operation=="resume") require(!sched_resume() && !sched_activateDay(4),"partial import bypassed BLOCKED");
                require(sched_hasError() && !sched_isRunning(),"partial import left old RAM executable");
                require(audioCalls==0 && activeDay==3 && activeDate=="2026-11-16","partial import fired/advanced");
                require(host::files()==files && host::fs().writes==writes,"partial import wrote stale RAM");
                require(sched_persistenceStatus()==active_day::Status::Ready && host::nvs().writes==nvsWrites,
                        "file BLOCKED changed NVS status/storage");
            });
    addCase(list,"T08","partial_import_before_active_keeps_consistent_day",[]() {
        readyActive(); stageReplacementCourse(); host::fs().renameErrorPath="/day02.new";
        String error; require(!sched_commitCourse(error),"rename fault missed");
        require(sched_isRunning() && !sched_hasError() && liveIs(3,"2026-11-16"),"untouched active day was blocked");
        host::clock()=at(16,4); sched_check(); require(audioCalls==1,"consistent active day stopped ringing");
    });
    for (const std::string mode : {"initialization","empty","short","long_to_short"})
        addCase(list,"T07","prepared_day_"+mode,[mode]() {
            readyActive();
            if (mode=="long_to_short") {
                std::string json="[";
                for (unsigned i=0;i<MAX_SCHEDULES;++i) {
                    if (i) json+=",";
                    json+="{\"id\":"+std::to_string(i+1)+",\"hour\":6,\"min\":"+std::to_string(i)+
                        ",\"desc\":\""+std::string(80,'x')+"\"}";
                }
                host::files()["/day03.conf"]=json+"]";
                require(sched_parseFromPath("/day03.conf") && count==MAX_SCHEDULES,"long schedule fixture not loaded");
            }
            std::aligned_storage<sizeof(PreparedDay),alignof(PreparedDay)>::type storage;
            std::memset(&storage,0xa5,sizeof(storage));
            PreparedDay* candidate=new (&storage) PreparedDay;
            struct Destroy { PreparedDay* p; ~Destroy() { p->~PreparedDay(); } } destroy{candidate};
            if (mode=="initialization") {
                for (const auto& entry:candidate->items) requireEmptyEntry(entry);
                return;
            }
            host::files()["/day04.conf"]=mode=="empty"?"[]":
                "[{\"id\":7,\"hour\":5,\"min\":0,\"desc\":\"short\"}]";
            active_day::Record r; r.day=4; r.date=20261116;
            require(prepareRecord(*candidate,r) && prepareFromPath("/day04.conf",*candidate),"short candidate preparation failed");
            host::forbidAllocation()=true;
            publishPrepared(*candidate,true,false);
            host::forbidAllocation()=false;
            require(count==(mode=="empty"?0U:1U) && activeDay==4 && sched_isRunning(),"incorrect publication metadata");
            if (count) require(entries[0].id==7 && entries[0].description=="short","short entry was lost");
            for (unsigned i=count;i<MAX_SCHEDULES;++i) requireEmptyEntry(entries[i]);
            host::clock()=at(16,6,20); sched_check();
            require(audioCalls==0,"old entry beyond count fired");
        });
    for (const std::string mode : {"open","json","root","oom"})
        addCase(list,"T08","course_reload_error_"+mode,[mode]() {
            readyActive(); stageReplacementCourse();
            if (mode=="open") { host::fs().failRead=true; host::fs().path="/day03.conf"; }
            if (mode=="json") host::files()["/day03.new"]="[bad";
            if (mode=="root") host::files()["/day03.new"]="{}";
            if (mode=="oom") host::fs().afterReadOpen=[](){host::failNewAfter()=0;};
            String error; const bool ok=sched_commitCourse(error);
            host::failNewAfter()=-1; host::fs().afterReadOpen=nullptr;
            require(!ok && error.length(),"course reload failure reported success");
            require(sched_persistenceStatus()==active_day::Status::Ready && host::nvs().writes==0,
                    "file failure reclassified/wrote valid NVS");
            require(sched_hasError() && !sched_isRunning(),"old RAM schedule remained executable");
            host::clock()=at(16,4); sched_check(); sched_catchup();
            require(audioCalls==0,"BLOCKED fired old RAM schedule");
        });
    for (const std::string operation : {"resume","activation_same","activation_other","advance","tick","mutations","recover"})
        addCase(list,"T08","course_blocked_guard_"+operation,[operation]() {
            courseReloadFailure();
            const auto files=host::files(); const auto nvs=host::nvs().durable;
            const unsigned writes=host::nvs().writes;
            if (operation=="resume") require(!sched_resume(),"Resume bypassed file BLOCKED");
            if (operation=="activation_same") require(!sched_activateDay(3),"same-day activation bypassed BLOCKED");
            if (operation=="activation_other") require(!sched_activateDay(4),"other-day activation bypassed BLOCKED");
            if (operation=="tick") {
                host::clock()=at(16,4); sched_check(); sched_catchup();
                require(audioCalls==0,"BLOCKED fired old RAM on a normal tick/catch-up");
            }
            if (operation=="advance") {
                host::clock()=at(17,4); sched_check(); checkDateAdvance(); sched_catchup();
                require(audioCalls==0,"BLOCKED fired during date advancement");
            }
            if (operation=="mutations") {
                DynamicJsonDocument doc(64); JsonArray in=doc.to<JsonArray>(); String err;
                require(!sched_add(6,0,"x",1,1,20) && !sched_edit(1,6,0,"x",1,1,true,20) && !sched_del(1),
                        "BLOCKED allowed live edits");
                require(!sched_addToDay(5,6,0,"x",1,1,20) && !sched_editInDay(5,1,6,0,"x",1,1,true,20) &&
                        !sched_delFromDay(5,1),"BLOCKED allowed template edits");
                require(!sched_stageDay(0,in,err) && !sched_commitCourse(err),"BLOCKED allowed course mutation");
                sched_save(); require(!sched_saveNow(),"BLOCKED allowed stale RAM save");
                require(!sched_parseFromPath("/day04.conf"),"arbitrary parser bypassed BLOCKED");
            }
            if (operation=="recover") require(!sched_recover(record(3,20261116)),"NVS Recovery bypassed file BLOCKED");
            require(sched_hasError() && !sched_isRunning() && host::files()==files && host::nvs().writes==writes,
                    "guard changed files/NVS or unblocked execution");
            require(durable("active")->number==nvs.at("gong/active").number,"guard changed active record");
        });
    for (const std::string mode : {"missing","open","json","root","oom","unmounted"})
        addCase(list,"T08","course_blocked_reload_failure_"+mode,[mode]() {
            courseReloadFailure();
            if (mode=="missing") host::files().erase("/day03.conf");
            if (mode=="open") { host::fs().failRead=true; host::fs().path="/day03.conf"; }
            if (mode=="json") host::files()["/day03.conf"]="[bad";
            if (mode=="root") host::files()["/day03.conf"]="{}";
            if (mode=="unmounted") filesystemAvailable=false;
            if (mode=="oom") host::fs().afterReadOpen=[](){host::failNewAfter()=0;};
            const bool loaded=sched_load(); host::failNewAfter()=-1; host::fs().afterReadOpen=nullptr;
            require(!loaded,"failed explicit file reload reported success");
            require(sched_hasError() && !sched_isRunning() && !sched_resume(),"failed reload cleared BLOCKED");
            require(sched_persistenceStatus()==active_day::Status::Ready && host::nvs().writes==0,
                    "failed file reload affected NVS health");
        });
    addCase(list,"T08","course_blocked_reload_current_file",[]() {
        courseReloadFailure(); host::clock()=at(17,4);
        host::files()[SCHEDULE_FILE]="[{\"hour\":4,\"min\":0,\"desc\":\"stale-seed\"}]";
        const unsigned writes=host::nvs().writes;
        require(sched_load(),"current active file could not reload");
        require(!sched_hasError() && !sched_isRunning() && count==1 && entries[0].description=="replacement",
                "explicit reload did not adopt current active file paused");
        require(activeDay==3 && activeDate=="2026-11-16" && host::nvs().writes==writes,
                "file reload advanced/restamped/rewrote NVS");
        sched_check(); sched_catchup(); require(audioCalls==0,"reloaded day ran before Resume");
        host::clock()=at(16,5); require(sched_resume(),"verified file could not Resume"); sched_check();
        require(audioTracks==std::vector<int>({2}),"Resume executed stale RAM schedule");
    });
    for (bool oldFormat : {false,true}) addCase(list,"T08",oldFormat?"boot_blocked_reload_legacy":"boot_blocked_reload_active",[oldFormat]() {
        if (oldFormat) legacy(); else seedActive(record(3,20261116));
        const std::string file=host::files()["/day03.conf"];
        host::files().erase("/day03.conf"); sched_setup();
        require(!sched_activateDay(4) && !sched_resume(),"boot BLOCKED bypassed by ordinary activation/Resume");
        host::files()["/day03.conf"]=file; require(sched_load(),"repaired boot file reload failed");
        require(liveIs(3,"2026-11-16") && !sched_hasError() && !sched_isRunning(),"boot file repair not published paused");
        require(sched_resume(),"repaired boot file cannot Resume");
        require(host::nvs().writes==(oldFormat?2U:0U),"file repair repeated/missed migration");
    });
    // First boot of a genuinely empty device publishes the reference course
    // (spec 12) instead of the legacy Day 00, but an installation that still
    // has any legacy schedule file keeps the legacy path untouched.
    addCase(list,"T03","fresh_device_publishes_reference_course",[]() {
        host::files().clear(); host::nvs()=host::NvsModel();
        sched_setup();
        require(courseMode,"fresh device did not enter course mode");
        require(courseGeneration>0 && courseLength==14,"fresh device did not publish the 14-day reference course");
        require(!sched_isRunning(),"published course rang before an explicit Activate");
        require(!host::files().count("/gong.conf"),"fresh device wrote the legacy seed file");
        require(!host::files().count("/day00.conf"),"fresh device wrote a legacy day file");
        require(host::files().count("/course/g1/meta.json")==1,"reference generation meta missing");
    });
    addCase(list,"T03","fresh_course_activates_and_fires",[]() {
        host::files().clear(); host::nvs()=host::NvsModel();
        sched_setup();
        require(courseMode && courseLength==14,"reference course not published");
        require(sched_activateDay(0),"seeded day 0 could not be activated");
        require(sched_isRunning() && activeDay==0,"activation did not bind day 0");
        host::clock()=at(16,19,45); sched_check();
        require(audioCalls==1,"seeded arrival gong did not fire at 19:45");
    });
    addCase(list,"T02","legacy_day_files_block_course_seed",[]() {
        sched_setup();
        require(!courseMode,"course was seeded over an existing legacy installation");
        require(liveIs(0,"2026-11-16"),"legacy first boot did not activate Day 00");
        require(!host::files().count("/course/g1/meta.json"),"legacy boot wrote a course generation");
    });
    addCase(list,"T02","legacy_gong_seed_blocks_course_seed",[]() {
        for (int day=0;day<DAY_COUNT;++day) { char path[16]; dayPath(day,path); host::files().erase(path); }
        host::files()[SCHEDULE_FILE]="[{\"hour\":4,\"min\":0,\"desc\":\"legacy\"}]";
        sched_setup();
        require(!courseMode,"course was seeded over the legacy gong.conf seed file");
        require(!host::files().count("/course/g1/meta.json"),"legacy seed boot wrote a course generation");
    });
    addCase(list,"T08","course_reload_success_stays_running",[]() {
        readyActive(); stageReplacementCourse(); String error;
        require(sched_commitCourse(error) && !sched_hasError() && sched_isRunning(),"successful course reload stopped scheduler");
        host::clock()=at(16,5); sched_check();
        require(audioTracks==std::vector<int>({2}) && host::nvs().writes==1,"new course did not fire normally");
    });
}
