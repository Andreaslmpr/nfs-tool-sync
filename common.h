#ifndef COMMON_H
#define COMMON_H

#include <string>
#include <ctime>
#include <sstream>
#include <iomanip>

//format "YYYY-MM-DD HH:MM:SS"
inline std::string current_timestamp() {
    std::time_t t = std::time(nullptr);
    std::tm tm;
    localtime_r(&t, &tm);
    std::ostringstream oss;
    oss << std::setw(4) << std::setfill('0') << (tm.tm_year + 1900) << "-"
        << std::setw(2) << std::setfill('0') << (tm.tm_mon + 1) << "-"
        << std::setw(2) << std::setfill('0') << tm.tm_mday << " "
        << std::setw(2) << std::setfill('0') << tm.tm_hour << ":"
        << std::setw(2) << std::setfill('0') << tm.tm_min << ":"
        << std::setw(2) << std::setfill('0') << tm.tm_sec;
    return oss.str();
}

#endif // COMMON_H
