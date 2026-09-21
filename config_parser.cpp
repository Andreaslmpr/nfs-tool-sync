#include "config_parser.h"
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <algorithm>
#include <cstring>
#include <sys/stat.h>

// Βοηθητική: split string σε δύο μέρη με delimiter '@'
static void split_hostport(const std::string &in, std::string &path, std::string &host, int &port) {
    // in = "/some/dir@hostname:port"
    auto at_pos = in.find('@');
    if (at_pos == std::string::npos) {
        throw std::runtime_error("Invalid format (missing @): " + in);
    }
    path = in.substr(0, at_pos);
    std::string hostport = in.substr(at_pos + 1);
    auto colon_pos = hostport.find(':');
    if (colon_pos == std::string::npos) {
        throw std::runtime_error("Invalid format (missing :): " + in);
    }
    host = hostport.substr(0, colon_pos);
    std::string port_str = hostport.substr(colon_pos + 1);
    try {
        port = std::stoi(port_str);
    } catch (const std::exception &) {
        throw std::runtime_error("Invalid port number: " + in);
    }
    if (port <= 0 || port > 65535) {
        throw std::runtime_error("Invalid port number: " + in);
    }
}

std::vector<SyncInfo> parse_config_file(const std::string &config_path) {
    std::vector<SyncInfo> result;
    std::ifstream ifs(config_path);
    if (!ifs.is_open()) {
        throw std::runtime_error("Cannot open config file: " + config_path);
    }

    std::string line;
    while (std::getline(ifs, line)) {
        // Αφαίρεση τυχόν '\r' (config γραμμένο σε Windows) και κενών γραμμών/σχολίων
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n' || line.back() == ' ' || line.back() == '\t'))
            line.pop_back();
        if (line.empty() || line[0] == '#') continue;

        std::istringstream iss(line);
        std::string src_token, tgt_token;
        if (!(iss >> src_token >> tgt_token)) {
            throw std::runtime_error("Invalid config line: " + line);
        }
        SyncInfo si;
        si.active = true;
        si.last_sync_time = 0;
        si.error_count = 0;

        //src_token: "/source_dir@host:port"
        split_hostport(src_token, si.source_dir, si.source_host, si.source_port);
        //tgt_token: "/target_dir@host:port"
        split_hostport(tgt_token, si.target_dir, si.target_host, si.target_port);

        result.push_back(si);
    }
    return result;
}
