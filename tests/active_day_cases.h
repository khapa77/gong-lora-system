#pragma once
// Included by the fixture after the production TU and common helpers.
static void seedActive(uint64_t raw, bool schema=true) {
    host::nvs().seed("active",host::Cell::integer(host::Type::U64,raw));
    if (schema) host::nvs().seed("active_schema",host::Cell::integer(host::Type::U32,1));
}
static void readyActive() { seedActive(record(3,20261116)); sched_setup(); }
static void uncertain(bool newDurable=false) {
    readyActive(); host::clock()=at(17);
    host::nvs().inject("commit","active",newDurable?host::Action::ErrorDurable:host::Action::ErrorBefore);
    require(!sched_activateDay(4),"uncertain write reported success");
    require(!host::nvs().fault.armed,"uncertain fixture missed commit");
}
static void assertUnknown() {
    require(sched_persistenceStatus()==active_day::Status::Unknown && !sched_isRunning(),
            "expected UNKNOWN with execution disabled");
}
static void addCase(std::vector<Test>& list,const char* group,const std::string& name,
                    std::function<void()> run) { list.push_back({group,name,std::move(run)}); }
static void registerA021Tests(std::vector<Test>& list) {
    for (unsigned day=0;day<DAY_COUNT;++day) addCase(list,"T01","roundtrip_day_"+std::to_string(day),[day]() {
        active_day::Record r,out; r.day=day; r.date=20261116; uint64_t raw=0;
        require(active_day::encode(r,DAY_COUNT,raw) && raw==record(day,20261116),"wrong wire layout");
        require(active_day::decode(raw,DAY_COUNT,out) && out.day==day && out.date==r.date,"roundtrip failed");
    });
    for (const std::string text : {"", "2024-01-01", "2024-02-29", "2026-11-16", "2400-02-29", "9999-12-31"})
        addCase(list,"T01","valid_date_"+(text.empty()?std::string("unbound"):text),[text]() {
            uint32_t date=99; char formatted[11];
            require(active_day::parseDate(text.c_str(),date),"valid date rejected");
            active_day::formatDate(date,formatted); require(text==formatted,"date format not canonical");
        });
    for (const std::string text : {"2023-12-31","2026-02-29","2100-02-29","2026-02-31","2026-00-16",
            "2026-13-16","2026-11-00","2026-11-31","2026-1-16","2026/11/16","2026-11-16x",
            " 2026-11-16","2026-11-16 ","0000-00-00","10000-01-01"})
        addCase(list,"T01","bad_date_"+text,[text]() {
            uint32_t date=99; require(!active_day::parseDate(text.c_str(),date) && date==99,"bad date accepted/changed output");
        });
    const std::vector<uint64_t> badRecords={record(DAY_COUNT,20261116),record(3,20260231),record(3,20261116,2),
        record(3,20261116,0),record(3,20261116)|(uint64_t(1)<<32)};
    for (size_t i=0;i<badRecords.size();++i) addCase(list,"T01","bad_record_"+std::to_string(i),[badRecords,i]() {
        active_day::Record r; require(!active_day::decode(badRecords[i],DAY_COUNT,r),"bad record decoded");
    });
    addCase(list,"T01","unbound_record",[]() {
        active_day::Record r; require(active_day::decode(record(0,0),DAY_COUNT,r) && r.date==0,"unbound lost");
        r.day=DAY_COUNT; uint64_t raw=7; require(!active_day::encode(r,DAY_COUNT,raw) && raw==7,"invalid day encoded");
        r.day=0; r.date=20260231; require(!active_day::encode(r,DAY_COUNT,raw),"bad calendar encoded");
    });

    for (bool schema : {false,true}) addCase(list,"T02",schema?"active_with_schema":"active_without_schema",[schema]() {
        legacy(8,"2026-11-15"); seedActive(record(3,20261116),schema); sched_setup();
        require(liveIs(3,"2026-11-16") && sched_isRunning(),"active not authoritative");
        require(host::nvs().writes==0 && host::nvs().commits==0,"valid boot rewrote storage");
        for (unsigned n=0;n<3;++n) { bootAgain(); require(sched_isRunning(),"reboot needs manual resume"); }
        require(host::nvs().writes==0 && host::nvs().commits==0,"repeated boot wrote storage");
    });
    addCase(list,"T02","schema_without_active",[]() {
        legacy(); host::nvs().seed("active_schema",host::Cell::integer(host::Type::U32,1)); sched_setup();
        assertUnknown(); require(count==0 && host::nvs().writes==0,"schema-only fell back to legacy");
    });
    for (unsigned schema : {0,2}) addCase(list,"T02","invalid_schema_"+std::to_string(schema),[schema]() {
        seedActive(record(3,20261116)); host::nvs().seed("active_schema",host::Cell::integer(host::Type::U32,schema));
        sched_setup(); require(sched_persistenceStatus()==active_day::Status::Invalid && count==0,"bad schema ignored");
    });
    addCase(list,"T02","wrong_schema_type",[]() {
        seedActive(record(3,20261116)); host::nvs().seed("active_schema",host::Cell::string("1")); sched_setup();
        require(sched_persistenceStatus()==active_day::Status::Invalid && host::nvs().writes==0,"wrong marker type accepted");
    });
    for (const std::string key : {"active_schema","active","aday","adate"}) addCase(list,"T02","read_error_"+key,[key]() {
        legacy(); host::nvs().inject("get",key,host::Action::ErrorBefore); sched_setup(); assertUnknown();
        require(!host::nvs().fault.armed && host::nvs().writes==0,"read failure became empty/migration");
    });
    addCase(list,"T02","missing_namespace",[]() {
        host::nvs()=host::NvsModel(); active_day::Store store(DAY_COUNT); active_day::Record r;
        require(store.load(true,r)==active_day::Source::Empty && store.status()==active_day::Status::Empty,
                "confirmed absent namespace not empty");
        require(host::nvs().writes==0 && !host::nvs().namespacePresent,"read-only probe created namespace");
    });
    addCase(list,"T02","unavailable_filesystem_not_empty",[]() {
        filesystemAvailable=false; sched_setup(); assertUnknown(); require(host::nvs().writes==0,"unmounted FS became empty");
    });
    for (const std::string mode : {"missing","json","root","open","oom","unmounted"}) addCase(list,"T02","blocked_file_"+mode,[mode]() {
        seedActive(record(3,20261116));
        if (mode=="missing") host::files().erase("/day03.conf");
        if (mode=="json") host::files()["/day03.conf"]="[bad";
        if (mode=="root") host::files()["/day03.conf"]="{}";
        if (mode=="open") { host::fs().failRead=true; host::fs().path="/day03.conf"; }
        if (mode=="oom") host::fs().afterReadOpen=[](){ host::failNewAfter()=0; };
        if (mode=="unmounted") filesystemAvailable=false;
        sched_setup(); host::failNewAfter()=-1;
        require(sched_persistenceStatus()==active_day::Status::Loaded && sched_hasError() && !sched_isRunning(),
                "file failure not BLOCKED or misclassified as invalid NVS");
        require(count==0 && host::nvs().writes==0 && !sched_resume(),"BLOCKED executed/wrote/resumed");
    });

    addCase(list,"T03","first_boot_marker_order",[]() {
        // Boot leaves a fresh device waiting; the operator activation is what
        // writes the legacy binding, so the marker order is checked there.
        sched_setup();
        require(sched_waitingForCourse() && !sched_isRunning(),"fresh boot did not wait");
        require(sched_activateDay(0),"explicit legacy activation failed");
        require(liveIs(0,"2026-11-16") && durable("active_schema"),"first activation failed");
        const auto& trace=host::nvs().trace;
        const auto marker=std::find(trace.begin(),trace.end(),"commit:active_schema");
        const auto active=std::find(trace.begin(),trace.end(),"set:active");
        require(marker!=trace.end() && active!=trace.end() && marker<active,"active written before marker commit");
        require(host::nvs().writes==2 && host::nvs().commits==2,"unexpected first-boot writes");
    });
    addCase(list,"T03","first_boot_seed",[]() {
        host::files()[SCHEDULE_FILE]=host::files()["/day03.conf"]; host::files().erase("/day00.conf");
        sched_setup();
        require(sched_waitingForCourse() && !host::files().count("/day00.conf"),
                "fresh boot seeded a day file by itself");
        require(sched_activateDay(0) && activeDay==0 && count==1 &&
                entries[0].description=="day3","explicit activation lost the seed");
    });
    addCase(list,"T03","prepare_failure_before_marker",[]() {
        host::files()["/day00.conf"]="bad"; sched_setup();
        require(sched_waitingForCourse() && host::nvs().writes==0 && count==0 && !sched_isRunning(),
                "fresh boot wrote/published");
        require(!sched_activateDay(0) && host::nvs().writes==0 && count==0,
                "failed activation wrote/published");
    });

    for (const std::string mode : {"missing_date","empty_date","file","file_missing_date"}) addCase(list,"T04",mode,[mode]() {
        if (mode=="file" || mode=="file_missing_date")
            host::files()["/activeday.conf"]=mode=="file"?"{\"day\":3,\"date\":\"2026-11-16\"}":"{\"day\":3}";
        else { legacy(3,""); if (mode=="missing_date") { host::nvs().visible.erase("gong/adate"); host::nvs().durable.erase("gong/adate"); } }
        const auto files=host::files(); host::clock()=0; sched_setup();
        const uint32_t date=mode=="file"?20261116:0;
        require(durable("active") && durable("active")->number==record(3,date),"legacy migration wrong");
        require(host::files()==files && host::fs().removes==0,"migration changed/deleted source files");
    });
    for (const std::string json : {"{}","{\"day\":13}","{\"day\":3,\"date\":null}","{\"day\":3,\"date\":123}",
            "{\"day\":3,\"date\":\"2026-02-31\"}","{bad"}) addCase(list,"T04","bad_file_"+json,[json]() {
        host::files()["/activeday.conf"]=json; sched_setup();
        require(sched_persistenceStatus()==active_day::Status::Invalid && count==0 && host::nvs().writes==0,
                "invalid legacy file activated/migrated");
        require(host::files()["/activeday.conf"]==json,"bad source deleted/changed");
    });
    addCase(list,"T04","legacy_file_open_error",[]() {
        host::files()["/activeday.conf"]="{\"day\":3}"; host::fs().failRead=true;
        sched_setup(); assertUnknown(); require(host::nvs().writes==0,"legacy read error migrated");
    });
    for (const std::string key : {"aday","adate"}) addCase(list,"T04","wrong_type_"+key,[key]() {
        legacy(); host::nvs().seed(key,host::Cell::integer(host::Type::U64,3)); sched_setup();
        require(sched_persistenceStatus()==active_day::Status::Invalid && host::nvs().writes==0,"wrong legacy type migrated");
    });

    struct FaultRow { const char* operation; host::Action action; const char* name; };
    const std::vector<FaultRow> faults={
        {"set",host::Action::ErrorBefore,"set_old"},{"set",host::Action::ErrorDurable,"set_new"},
        {"commit",host::Action::ErrorBefore,"commit_old"},{"commit",host::Action::ErrorDurable,"commit_new"},
        {"get",host::Action::ErrorBefore,"read_error"},{"get",host::Action::ReadMismatch,"read_mismatch"}};
    for (const std::string key : {"active_schema","active"}) for (const auto row : faults)
        addCase(list,"T05",key+"_"+row.name,[key,row]() {
            legacy(); const auto files=host::files();
            host::nvs().inject(row.operation,key,row.action,std::string(row.operation)=="get"?1:0);
            sched_setup(); assertUnknown();
            require(!host::nvs().fault.armed && count==0 && activeDay==-1,"migration fault missed/published");
            const unsigned writes=host::nvs().writes;
            for (int n=0;n<3;++n) { sched_check(); checkDateAdvance(); sched_catchup(); }
            require(host::nvs().writes==writes && host::files()==files,"migration failure retried/changed files");
            require(durable("aday")->number==3 && durable("adate")->text=="2026-11-16","legacy changed on fault");
        });
    for (const auto row : faults) addCase(list,"T05",std::string("activation_")+row.name,[row]() {
        readyActive(); host::clock()=at(17); host::nvs().inject(row.operation,"active",row.action);
        require(!sched_activateDay(4),"activation ignored fault"); assertUnknown();
        require(!host::nvs().fault.armed && liveIs(3,"2026-11-16"),"fault missed or candidate published");
    });
    addCase(list,"T05","write_open_failure",[]() {
        readyActive(); host::nvs().inject("open","gong",host::Action::ErrorBefore);
        require(!sched_activateDay(4),"open failure ignored"); assertUnknown();
    });

    for (const std::string key : {"active_schema","active"}) for (const std::string operation : {"set","commit"})
        for (const auto action : {host::Action::CutBefore,host::Action::CutAfter})
            addCase(list,"T06","migration_"+key+"_"+operation+(action==host::Action::CutBefore?"_before":"_after"),[key,operation,action]() {
                legacy(); host::nvs().inject(operation,key,action); bool cut=false;
                try { sched_setup(); } catch (const host::PowerCut&) { cut=true; }
                require(cut && !host::nvs().fault.armed,"migration cut missed");
                const bool hasActive=durable("active"),hasMarker=durable("active_schema");
                const unsigned writes=host::nvs().writes; bootAgain();
                if (hasActive) { require(liveIs(3,"2026-11-16") && sched_isRunning(),"durable migration not loaded");
                    require(host::nvs().writes==writes,"complete migration repeated"); }
                else if (hasMarker) { assertUnknown(); require(host::nvs().writes==writes,"marker-only boot retried"); }
                else require(liveIs(3,"2026-11-16"),"legacy lost before marker durability");
            });
    for (const std::string operation : {"set","commit","get"}) for (const auto action : {host::Action::CutBefore,host::Action::CutAfter})
        addCase(list,"T06","switch_"+operation+(action==host::Action::CutBefore?"_before":"_after"),[operation,action]() {
            readyActive(); host::clock()=at(17); host::nvs().inject(operation,"active",action); bool cut=false;
            try { sched_activateDay(4); } catch (const host::PowerCut&) { cut=true; }
            require(cut && !host::nvs().fault.armed,"switch cut missed");
            host::nvs().reboot(); resetRam(); loadActiveDay();
            require(liveIs(3,"2026-11-16") || liveIs(4,"2026-11-17"),"recovered mixed pair");
            checkDateAdvance(); require(liveIs(4,"2026-11-17"),"double advancement after cut");
        });

    addCase(list,"T07","transaction_phases",[]() {
        readyActive(); std::vector<active_day::Phase> phases;
        host::nvs().beforeOperation=[&](const std::string& op,const std::string& key) {
            if (key=="active" && (op=="set" || op=="commit" || op=="get")) {
                require(liveIs(3,"2026-11-16"),"published before readback"); phases.push_back(activeStore.phase());
            }
        };
        host::clock()=at(17); require(sched_activateDay(4),"phase activation failed");
        require(phases==std::vector<active_day::Phase>({active_day::Phase::Prepared,active_day::Phase::SetAcked,
                active_day::Phase::CommitAcked}),"wrong transaction phase order");
        require(activeStore.phase()==active_day::Phase::Idle && sched_isRunning(),"publish not READY");
    });
    for (const std::string mode : {"activation","restamp","recovery","direct_publish"}) addCase(list,"T07","no_allocation_"+mode,[mode]() {
        readyActive();
        host::files()["/day04.conf"]="[{\"hour\":4,\"min\":0,\"desc\":\""+std::string(120,'x')+"\"}]";
        if (mode=="recovery") activeStore.unknown();
        host::nvs().afterU64Read=[](const char* key) {
            if (std::strcmp(key,"active")==0 && activeStore.phase()==active_day::Phase::CommitAcked) {
                host::fenceTriggered()=true; host::forbidAllocation()=true;
            }
        };
        bool ok=false;
        if (mode=="activation") ok=sched_activateDay(4);
        if (mode=="restamp") ok=setActiveDay(3,String("2026-11-17"));
        if (mode=="recovery") ok=sched_recover(record(3,20261116));
        if (mode=="direct_publish") {
            PreparedDay candidate; active_day::Record r; r.day=4; r.date=20261116;
            require(prepareRecord(candidate,r) && prepareFromPath("/day04.conf",candidate),"publish fixture failed");
            host::fenceTriggered()=true; host::forbidAllocation()=true;
            publishPrepared(candidate,false,false); ok=true;
        }
        host::forbidAllocation()=false; host::nvs().afterU64Read=nullptr;
        require(ok && host::fenceTriggered() && lockDepth==0,"publication fence not reached/balanced");
    });
    for (const std::string mode : {"candidate_new","document_new","json_pool","date_string","description_string","restamp_copy"})
        addCase(list,"T07","prepare_oom_"+mode,[mode]() {
            readyActive(); const unsigned writes=host::nvs().writes;
            if (mode=="candidate_new") host::failNewAfter()=0;
            if (mode=="date_string") host::failStrings()=true;
            if (mode=="document_new") host::fs().afterReadOpen=[](){host::failNewAfter()=0;};
            if (mode=="json_pool") host::failJsonPool()=true;
            if (mode=="description_string") host::fs().afterReadOpen=[](){host::failStrings()=true;};
            bool ok=false;
            if (mode=="restamp_copy") {
                const String date("2026-11-17"); host::failStringCopies()=true; ok=setActiveDay(3,date);
            } else ok=sched_activateDay(4);
            host::failNewAfter()=-1; host::failStrings()=false;
            host::failStringCopies()=false; host::failJsonPool()=false;
            require(!ok && liveIs(3,"2026-11-16") && sched_isRunning(),"prepare failure replaced old state");
            require(host::nvs().writes==writes,"preparation failure touched NVS");
        });

    for (const std::string mode : {"unknown","invalid","paused","blocked"}) addCase(list,"T08","guards_"+mode,[mode]() {
        if (mode=="unknown") uncertain();
        if (mode=="invalid") { seedActive(record(3,20261116,2)); sched_setup(); }
        if (mode=="blocked") { seedActive(record(3,20261116)); host::files().erase("/day03.conf"); sched_setup(); }
        if (mode=="paused") { uncertain(); require(sched_recover(record(3,20261116)),"pause fixture failed"); }
        const auto status=sched_persistenceStatus(); const auto files=host::files();
        const unsigned writes=host::nvs().writes,fw=host::fs().writes,rm=host::fs().removes,rn=host::fs().renames;
        const unsigned audio=audioCalls; schedPendingSave=true; audioPlaying=true; host::clock()=at(18,4);
        DynamicJsonDocument doc(64); JsonArray in=doc.to<JsonArray>(); String err;
        require(!sched_add(5,0,"x",1,1,20) && !sched_edit(1,5,0,"x",1,1,true,20) && !sched_del(1),"live edit escaped guard");
        require(!sched_addToDay(5,5,0,"x",1,1,20) && !sched_editInDay(5,1,5,0,"x",1,1,true,20) &&
                !sched_delFromDay(5,1),"template edit escaped guard");
        if (mode!="blocked") require(!sched_activateDay(4),"ordinary activation bypassed quarantine/pause");
        require(!sched_stageDay(0,in,err) && !sched_commitCourse(err),"course write escaped guard");
        sched_load(); require(!sched_parseFromPath("/day03.conf"),"reload escaped guard");
        sched_save(); require(!sched_saveNow(),"save escaped guard");
        for (unsigned n=0;n<3;++n) { sched_check(); checkDateAdvance(); sched_catchup(); }
        audioPlaying=false; sched_check();
        require(host::nvs().writes==writes && host::files()==files && host::fs().writes==fw &&
                host::fs().removes==rm && host::fs().renames==rn,"guard allowed a write/delete/rename");
        require(audioCalls==audio && schedPendingSave && sched_persistenceStatus()==status && !sched_isRunning(),
                "quarantine cleared pending/latch or fired audio");
    });
    addCase(list,"T08","failure_inside_date_advance",[]() {
        readyActive(); host::clock()=at(17,4);
        host::nvs().inject("commit","active",host::Action::ErrorBefore); sched_check();
        assertUnknown(); require(audioCalls==0,"old schedule fired after advancement failure");
    });

    for (const std::string mode : {"success","open","short","serialize"}) addCase(list,"T09","pending_flush_"+mode,[mode]() {
        readyActive(); audioPlaying=true;
        require(sched_edit(1,4,0,"unsaved",1,1,true,30) && schedPendingSave,"edit not deferred");
        const unsigned writes=host::nvs().writes;
        if (mode=="open") { host::fs().failWrite=true; host::fs().path="/day03.conf"; }
        if (mode=="short") host::fs().shortWrite=true;
        if (mode=="serialize") host::failReserve()=true;
        const bool ok=sched_activateDay(4); host::failReserve()=false;
        if (mode=="success") require(ok && !schedPendingSave && host::files()["/day03.conf"].find("unsaved")!=std::string::npos,
                                    "switch did not flush old pending edits");
        else require(!ok && schedPendingSave && activeDay==3 && entries[0].description=="unsaved" &&
                     host::nvs().writes==writes,"failed flush lost pending/switched active");
    });
    addCase(list,"T09","pending_tick_flush",[]() {
        readyActive(); audioPlaying=true; require(sched_edit(1,4,0,"unsaved",1,1,true,30),"edit failed");
        sched_check(); require(schedPendingSave,"flushed while playing"); audioPlaying=false;
        host::fs().failWrite=true; sched_check(); require(schedPendingSave,"failed tick flush cleared pending");
        host::fs().failWrite=false; sched_check(); require(!schedPendingSave,"successful tick flush not cleared");
    });
    addCase(list,"T09","failed_flush_keeps_current_day_running",[]() {
        readyActive(); audioPlaying=true; require(sched_edit(1,4,0,"unsaved",1,1,true,30),"edit failed");
        audioPlaying=false; host::fs().failWrite=true; host::clock()=at(16,4);
        sched_check(); require(audioCalls==1 && schedPendingSave && sched_isRunning(),
                "file-save failure silently stopped a consistent current-day schedule");
    });
    for (const std::string mode : {"deny_discard","failed_recovery","accept_discard"}) addCase(list,"T09",mode,[mode]() {
        readyActive(); audioPlaying=true; require(sched_edit(1,4,0,"unsaved",1,1,true,30),"edit failed");
        host::nvs().inject("commit","active",host::Action::ErrorBefore);
        require(!setActiveDay(3,String("2026-11-17")),"restamp ignored fault"); assertUnknown();
        const unsigned writes=host::nvs().writes; const auto files=host::files(); audioPlaying=false;
        sched_check(); require(schedPendingSave && host::files()==files,"quarantine flushed pending");
        if (mode=="failed_recovery") host::nvs().inject("commit","active",host::Action::ErrorBefore);
        const bool ok=sched_recover(record(3,20261116),mode!="deny_discard");
        if (mode=="accept_discard") {
            require(ok && !schedPendingSave && liveIs(3,"2026-11-16") && !sched_isRunning(),"confirmed discard failed");
            require(sched_resume(),"resume failed"); sched_check(); require(host::files()==files,"old pending replayed");
        } else {
            require(!ok && schedPendingSave && entries[0].description=="unsaved","failed/denied recovery discarded edits");
            if (mode=="deny_discard") require(host::nvs().writes==writes,"denied recovery wrote NVS");
        }
    });

    for (const auto row : faults) addCase(list,"T10",std::string("recovery_")+row.name,[row]() {
        uncertain(); host::nvs().inject(row.operation,"active",row.action,std::string(row.operation)=="get"?1:0);
        require(!sched_recover(record(3,20261116)),"recovery ignored fault"); assertUnknown();
        require(!host::nvs().fault.armed && liveIs(3,"2026-11-16"),"recovery fault missed/published");
    });
    for (const std::string mode : {"expected_mismatch","missing","unknown_version","wrong_type","file_missing","read_error","open_error"})
        addCase(list,"T10",mode,[mode]() {
            uncertain();
            if (mode=="missing") { host::nvs().visible.erase("gong/active"); host::nvs().durable.erase("gong/active"); }
            if (mode=="unknown_version") seedActive(record(3,20261116,2));
            if (mode=="wrong_type") host::nvs().seed("active",host::Cell::string("bad"));
            if (mode=="file_missing") host::files().erase("/day03.conf");
            if (mode=="read_error") host::nvs().inject("get","active",host::Action::ErrorBefore);
            if (mode=="open_error") host::nvs().inject("open","gong",host::Action::ErrorBefore);
            const unsigned writes=host::nvs().writes;
            require(!sched_recover(mode=="expected_mismatch"?record(4,20261117):record(3,20261116)),"unsafe recovery accepted");
            require(!sched_isRunning() && !sched_resume() && host::nvs().writes==writes,"failed recovery resumed/wrote");
        });
    for (bool next : {false,true}) addCase(list,"T10",next?"recover_new_record":"recover_old_record",[next]() {
        uncertain(next); const uint64_t expected=next?record(4,20261117):record(3,20261116);
        const unsigned writes=host::nvs().writes,commits=host::nvs().commits;
        require(sched_recover(expected) && !sched_isRunning(),"successful recovery not paused");
        require(durable("active")->number==expected && host::nvs().writes==writes+1 && host::nvs().commits==commits+1,
                "recovery changed target or made multiple attempts");
        require(sched_recover(expected) && host::nvs().writes==writes+1,"repeated recovery wrote");
        require(!sched_recover(expected^1),"paused recovery accepted stale expected record");
        host::clock()=at(19,4); sched_check(); checkDateAdvance(); sched_catchup();
        require(durable("active")->number==expected && audioCalls==0,"paused scheduler advanced/fired");
    });

    addCase(list,"T11","resume_then_normal_advancement",[]() {
        uncertain(); require(sched_recover(record(3,20261116)),"recovery failed");
        const unsigned writes=host::nvs().writes; require(sched_resume() && sched_resume(),"resume not idempotent");
        require(host::nvs().writes==writes && liveIs(3,"2026-11-16"),"resume wrote/advanced");
        sched_check(); require(liveIs(4,"2026-11-17"),"normal advancement not restored");
        require(!sched_recover(record(4,20261117)),"running scheduler started recovery");
    });
    for (bool pause : {false,true}) addCase(list,"T11",pause?"reboot_paused":"reboot_unknown",[pause]() {
        uncertain(true); if (pause) require(sched_recover(record(4,20261117)),"recovery fixture failed");
        const unsigned writes=host::nvs().writes; bootAgain();
        require(sched_isRunning() && liveIs(4,"2026-11-17") && host::nvs().writes==writes,
                "valid reboot needs resume or rewrites state");
    });
    addCase(list,"T11","resume_rejects_boot_empty",[]() {
        require(!sched_resume(),"BOOT resumed"); active_day::Record r; activeStore.load(true,r);
        require(!sched_resume(),"EMPTY resumed");
    });

    addCase(list,"T12","a01_catchup_then_next_minute",[]() {
        seedActive(record(3,20261116)); host::clock()=at(16,4,0,20);
        host::files()["/day03.conf"]="[{\"hour\":4,\"min\":0,\"track\":1},{\"hour\":4,\"min\":1,\"track\":2}]";
        host::nvs().seed("lastfire",host::Cell::integer(host::Type::U32,at(16,3,59)));
        sched_setup(); sched_check(); host::clock()=at(16,4,1); sched_check();
        require(audioTracks==std::vector<int>({1,2}),"catchup suppressed next event or repeated first");
    });
    addCase(list,"T12","no_legacy_or_other_key_writes",[]() {
        legacy(); host::nvs().seed("unrelated",host::Cell::string("keep")); sched_setup();
        const uint32_t last=lastFireTs; host::clock()=at(17); require(sched_activateDay(4),"activation failed");
        activeStore.unknown(); require(sched_recover(record(4,20261117)) && sched_resume(),"recovery failed");
        require(durable("aday")->number==3 && durable("adate")->text=="2026-11-16" &&
                durable("unrelated")->text=="keep" && durable("lastfire")->number==last,"unrelated/legacy key changed");
        for (const auto& op : host::nvs().trace) if (op.find("set:")==0)
            require(op=="set:active" || op=="set:active_schema","A02 wrote outside its keys");
    });
}
