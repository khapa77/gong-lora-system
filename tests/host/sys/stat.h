#pragma once
#include_next <sys/stat.h>
#include "../LittleFS.h"

namespace host {
// Mirror the installed VFS stat/errno seam, independently of exists()/open().
inline int statFile(const char* full, struct stat* out) {
    const char prefix[]="/littlefs";
    if (std::strncmp(full,prefix,sizeof(prefix)-1)!=0) { errno=EINVAL; return -1; }
    const char* path=full+sizeof(prefix)-1;
    if (!path[0]) path="/";
    const auto& fault=fs();
    if (fault.statError && (fault.statErrorPath.empty() || fault.statErrorPath==path)) {
        errno=fault.statError; return -1;
    }
    *out={};
    if (std::strcmp(path,"/")==0) {
        if (fault.rootMissing) { errno=ENOENT; return -1; }
        out->st_mode=S_IFDIR; return 0;
    }
    if (fault.directoryPath==path) { out->st_mode=S_IFDIR; return 0; }
    if (!files().count(path)) { errno=ENOENT; return -1; }
    out->st_mode=S_IFREG; return 0;
}
}
#define stat(path, out) host::statFile(path, out)
