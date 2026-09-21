#include "logger.h"
#include "common.h"
#include <iostream>

std::mutex log_mutex;

static std::ofstream manager_log_fs;
static std::ofstream client_log_fs;
static std::ofstream console_log_fs;

void init_manager_log(const std::string &path) {
    std::lock_guard<std::mutex> lk(log_mutex);
    if (manager_log_fs.is_open()) manager_log_fs.close();
    manager_log_fs.open(path, std::ios::out | std::ios::trunc);
    if (!manager_log_fs.is_open()) {
        std::cerr << "Error: cannot open manager log file: " << path << std::endl;
        std::exit(1);
    }
}

void init_client_log(const std::string &path) {
    std::lock_guard<std::mutex> lk(log_mutex);
    if (client_log_fs.is_open()) client_log_fs.close();
    client_log_fs.open(path, std::ios::out | std::ios::trunc);
    if (!client_log_fs.is_open()) {
        std::cerr << "Error: cannot open client log file: " << path << std::endl;
        std::exit(1);
    }
}

void init_console_log(const std::string &path) {
    std::lock_guard<std::mutex> lk(log_mutex);
    if (console_log_fs.is_open()) console_log_fs.close();
    console_log_fs.open(path, std::ios::out | std::ios::trunc);
    if (!console_log_fs.is_open()) {
        std::cerr << "Error: cannot open console log file: " << path << std::endl;
        std::exit(1);
    }
}

void log_manager(const std::string &msg) {
    std::lock_guard<std::mutex> lk(log_mutex);
    manager_log_fs << "[" << current_timestamp() << "] " << msg << "\n";
    manager_log_fs.flush();
}

void log_client(const std::string &msg) {
    std::lock_guard<std::mutex> lk(log_mutex);
    client_log_fs << "[" << current_timestamp() << "] " << msg << "\n";
    client_log_fs.flush();
}

void log_console(const std::string &msg) {
    std::lock_guard<std::mutex> lk(log_mutex);
    console_log_fs << "[" << current_timestamp() << "] " << msg << "\n";
    console_log_fs.flush();
}
