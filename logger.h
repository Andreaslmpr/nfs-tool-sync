#ifndef LOGGER_H
#define LOGGER_H

#include <string>
#include <fstream>
#include <mutex>

// mutex seperates multiple threads
extern std::mutex log_mutex;

//log Message Append,Initialize to the string path url
void init_manager_log(const std::string &path);
void init_client_log(const std::string &path);
void init_console_log(const std::string &path);

//Message Prompter and Printer
void log_manager(const std::string &msg);
void log_client(const std::string &msg);
void log_console(const std::string &msg);

#endif 
