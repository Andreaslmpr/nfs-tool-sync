#include <iostream>
#include <string>
#include <sstream>
#include <vector>
#include <mutex>
#include <unistd.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <cstdio>
#include <cstring>

#include "common.h"
#include "logger.h"

// Connect to manager_host:manager_port, returns sockfd or -1
static int connect_to(const std::string &host, int port) {
    struct addrinfo hints{}, *res, *p;
    char port_str[16];
    std::snprintf(port_str, sizeof(port_str), "%d", port);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host.c_str(), port_str, &hints, &res) != 0) {
        return -1;
    }
    int sockfd = -1;
    for (p = res; p != nullptr; p = p->ai_next) {
        sockfd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (sockfd < 0) continue;
        if (connect(sockfd, p->ai_addr, p->ai_addrlen) == 0) {
            freeaddrinfo(res);
            return sockfd;
        }
        close(sockfd);
    }
    freeaddrinfo(res);
    return -1;
}

// Send all bytes
static bool send_all(int sockfd, const char *buf, size_t len) {
    size_t total = 0;
    while (total < len) {
        ssize_t sent = send(sockfd, buf + total, len - total, 0);
        if (sent <= 0) return false;
        total += sent;
    }
    return true;
}

// Receive one line. Returns false on EOF without data.
static bool recv_line(int sockfd, std::string &out) {
    out.clear();
    char c;
    while (true) {
        ssize_t r = recv(sockfd, &c, 1, 0);
        if (r <= 0) return !out.empty();
        if (c == '\n') return true;
        if (c == '\r') continue;
        out.push_back(c);
    }
}

// [TIMESTAMP] Command add /dir1@1.2.3.4:8080 -> /dir2@4.5.6.7:8090
static std::string format_command_log(const std::string &line) {
    std::istringstream iss(line);
    std::string cmd, src, tgt, extra;
    iss >> cmd;
    if (cmd == "add" && (iss >> src) && (iss >> tgt) && !(iss >> extra)) {
        return "Command add " + src + " -> " + tgt;
    }
    return "Command " + line;
}

int main(int argc, char *argv[]) {
    std::string manager_host;
    int manager_port = 0;
    std::string console_log_path;
    int opt;

    // Parse flags: -l <console_log> -h <manager_host> -p <manager_port>
    while ((opt = getopt(argc, argv, "l:h:p:")) != -1) {
        switch (opt) {
            case 'l': console_log_path = optarg; break;
            case 'h': manager_host     = optarg; break;
            case 'p':
                try {
                    manager_port = std::stoi(optarg);
                } catch (const std::exception &) {
                    std::cerr << "Error: invalid port number: " << optarg << std::endl;
                    return 1;
                }
                break;
            default:
                std::cerr << "Usage: " << argv[0]
                          << " -l <console-logfile> -h <host_IP> -p <host_port>" << std::endl;
                return 1;
        }
    }
    if (console_log_path.empty() || manager_host.empty() || manager_port <= 0) {
        std::cerr << "Usage: " << argv[0]
                  << " -l <console-logfile> -h <host_IP> -p <host_port>" << std::endl;
        return 1;
    }

    init_console_log(console_log_path);

    std::string line;
    while (true) {
        // Prompt for input
        std::cout << "> " << std::flush;
        if (!std::getline(std::cin, line)) break;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t'))
            line.pop_back();
        if (line.empty()) continue;

        log_console(format_command_log(line));

        int sockfd = connect_to(manager_host, manager_port);
        if (sockfd < 0) {
            std::cerr << "Error: cannot connect to manager at "
                      << manager_host << ":" << manager_port << std::endl;
            log_console("Error: cannot connect to manager");
            continue;
        }

        std::string tosend = line + "\n";
        if (!send_all(sockfd, tosend.c_str(), tosend.size())) {
            std::cerr << "Error: send failed" << std::endl;
            close(sockfd);
            continue;
        }

        // Ο manager μπορεί να απαντήσει με πολλές γραμμές (π.χ. shutdown,
        // ή μία γραμμή "Added file:" ανά αρχείο) και κλείνει τη σύνδεση στο τέλος.
        std::string response;
        while (recv_line(sockfd, response)) {
            std::cout << response << std::endl;
        }
        close(sockfd);

        std::istringstream iss(line);
        std::string cmd;
        iss >> cmd;
        if (cmd == "shutdown") break;
    }
    return 0;
}
