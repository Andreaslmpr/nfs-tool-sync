#ifndef CONFIG_PARSER_H
#define CONFIG_PARSER_H

#include <string>
#include <vector>
#include <ctime>    

//(source - target)
struct SyncInfo {
    std::string source_host;
    int         source_port;
    std::string source_dir;
    std::string target_host;
    int         target_port;
    std::string target_dir;
    bool        active;           // true,canceled
    time_t      last_sync_time;    
    int         error_count;       
};


// Format κάθε γραμμής: 
//   /source_dir@source_host:source_port /target_dir@target_host:target_port
std::vector<SyncInfo> parse_config_file(const std::string &config_path);

#endif
