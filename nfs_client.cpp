#include <iostream>
#include <sstream>
#include <vector>
#include <string>
#include <thread>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <cerrno>
#include <unistd.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <dirent.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <csignal>
#include <filesystem>
#include <system_error>

#include "common.h"
#include "logger.h"

// List regular files in a flat directory
static std::vector<std::string> list_files_in_dir(const std::string &dirpath) {
    std::vector<std::string> result;
    DIR *dp = opendir(dirpath.c_str());
    if (!dp) return result;
    struct dirent *entry;
    while ((entry = readdir(dp)) != nullptr) {
        std::string name = entry->d_name;
        if (name == "." || name == "..") continue;
        std::string full = dirpath + "/" + name;
        struct stat st;
        if (stat(full.c_str(), &st) == 0 && S_ISREG(st.st_mode)) {
            result.push_back(name);
        }
    }
    closedir(dp);
    return result;
}

// Create a listening socket on the given port (IPv4)
static int create_listen_socket(int port) {
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        perror("socket");
        return -1;
    }
    int opt = 1;
    setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
#ifdef SO_REUSEPORT
    setsockopt(sockfd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));
#endif
    struct sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);
    if (bind(sockfd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(sockfd);
        return -1;
    }
    if (listen(sockfd, 10) < 0) {
        perror("listen");
        close(sockfd);
        return -1;
    }
    return sockfd;
}

// Send exactly len bytes over sockfd
static bool send_all(int sockfd, const char *buf, size_t len) {
    size_t total = 0;
    while (total < len) {
        ssize_t sent = send(sockfd, buf + total, len - total, 0);
        if (sent <= 0) return false;
        total += sent;
    }
    return true;
}

// Receive exactly len bytes into buf
static bool recv_all(int sockfd, char *buf, size_t len) {
    size_t total = 0;
    while (total < len) {
        ssize_t rec = recv(sockfd, buf + total, len - total, 0);
        if (rec <= 0) return false;
        total += rec;
    }
    return true;
}

// Read a token (until space or newline) and consume the delimiter.
// Διαβάζει byte-byte ώστε να μην "καταπιεί" ποτέ τα binary δεδομένα που ακολουθούν.
static bool read_token(int sockfd, std::string &out, char *delim = nullptr) {
    out.clear();
    char c;
    while (true) {
        ssize_t n = recv(sockfd, &c, 1, 0);
        if (n <= 0) return false;
        if (c == ' ' || c == '\n') {
            if (delim) *delim = c;
            break;
        }
        if (c == '\r') continue;
        out.push_back(c);
    }
    return true;
}

// Handle commands from nfs_manager
static void handle_manager_connection(int client_fd) {
    signal(SIGPIPE, SIG_IGN);
    int write_fd = -1;

    while (true) {
        std::string cmd;
        if (!read_token(client_fd, cmd)) break;
        if (cmd.empty()) continue;

        if (cmd == "LIST") {
            // LIST <source_dir>\n  ->  ένα όνομα ανά γραμμή και "." στο τέλος
            std::string dir;
            if (!read_token(client_fd, dir)) break;  // newline consumed
            auto files = list_files_in_dir(dir);
            for (auto &f : files) {
                std::string out = f + "\n";
                send_all(client_fd, out.c_str(), out.size());
            }
            send_all(client_fd, ".\n", 2);
            log_client("Received LIST " + dir + " -> sent " + std::to_string(files.size()) + " filenames");
        }
        else if (cmd == "PULL") {
            // PULL <path>\n  ->  <filesize><space><data>
            std::string filepath;
            if (!read_token(client_fd, filepath)) break;
            int fd = open(filepath.c_str(), O_RDONLY);
            if (fd < 0) {
                std::string err = "-1 " + std::string(strerror(errno)) + "\n";
                send_all(client_fd, err.c_str(), err.size());
                log_client("Received PULL " + filepath + " -> open error: " + strerror(errno));
                continue;
            }
            struct stat st;
            if (fstat(fd, &st) < 0) {
                close(fd);
                std::string err = "-1 " + std::string(strerror(errno)) + "\n";
                send_all(client_fd, err.c_str(), err.size());
                log_client("Received PULL " + filepath + " -> fstat error: " + strerror(errno));
                continue;
            }
            long remaining = st.st_size;
            // <filesize><space>
            std::string hdr = std::to_string(remaining) + " ";
            send_all(client_fd, hdr.c_str(), hdr.size());
            log_client("Received PULL " + filepath + " -> sending " + std::to_string(remaining) + " bytes");
            // <data> : σκέτα bytes, σε chunks, χωρίς επιπλέον headers
            const size_t MAX_CHUNK = 4096;
            char buf[MAX_CHUNK];
            bool ok = true;
            while (remaining > 0) {
                size_t toread = std::min((long)MAX_CHUNK, remaining);
                ssize_t r = read(fd, buf, toread);
                if (r <= 0) { ok = false; break; }
                if (!send_all(client_fd, buf, r)) { ok = false; break; }
                remaining -= r;
            }
            close(fd);
            if (!ok) {
                log_client("Error while sending " + filepath);
                break;   // η ροή δεν είναι πλέον συγχρονισμένη
            }
        }
        else if (cmd == "PUSH") {
            // PUSH <path> <chunk_size> <data>
            //   chunk_size == -1 : δημιουργία/άδειασμα αρχείου
            //   chunk_size  >  0 : ακολουθούν chunk_size bytes δεδομένων
            //   chunk_size ==  0 : τέλος αρχείου
            std::string filepath, size_str;
            if (!read_token(client_fd, filepath)) break;   // filepath delim
            if (!read_token(client_fd, size_str)) break;   // size delim
            long chunk_size = 0;
            try {
                chunk_size = std::stol(size_str);
            } catch (const std::exception &) {
                log_client("Invalid chunk size for PUSH " + filepath + ": '" + size_str + "'");
                break;
            }

            if (chunk_size == -1) {
                // init: create directories and open
                std::filesystem::path p(filepath);
                std::error_code ec;
                if (!p.parent_path().empty()) {
                    std::filesystem::create_directories(p.parent_path(), ec);
                    if (ec) {
                        log_client("mkdir failed for " + p.parent_path().string() + ": " + ec.message());
                        continue;
                    }
                }
                if (write_fd >= 0) close(write_fd);
                write_fd = open(filepath.c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0644);
                if (write_fd < 0) {
                    log_client("Error opening for PUSH init: " + filepath + " -> " + strerror(errno));
                    continue;
                }
                log_client("Received PUSH init " + filepath);
            }
            else if (chunk_size > 0) {
                // receive raw data
                std::vector<char> data(chunk_size);
                if (!recv_all(client_fd, data.data(), chunk_size)) break;
                if (write_fd >= 0) {
                    ssize_t w = write(write_fd, data.data(), chunk_size);
                    if (w < 0) {
                        log_client("Error writing chunk for PUSH: " + filepath + " -> " + strerror(errno));
                    }
                }
                log_client("Received PUSH chunk " + filepath + " (" + std::to_string(chunk_size) + " bytes)");
            }
            else {
                // chunk_size == 0: EOF
                if (write_fd >= 0) {
                    close(write_fd);
                    write_fd = -1;
                }
                log_client("Received PUSH EOF " + filepath);
            }
        }
        else {
            // unknown: skip to end of line
            std::string skip;
            read_token(client_fd, skip);
            log_client("Unknown command: " + cmd);
        }
    }
    if (write_fd >= 0) close(write_fd);
    close(client_fd);
}

int main(int argc, char *argv[]) {
    int port = 0;
    std::string log_path;
    int opt;

    // Σύμφωνα με την εκφώνηση: ./nfs_client -p <port_number>
    // (το -l <logfile> είναι προαιρετικό, default: nfs_client_<port>.log)
    while ((opt = getopt(argc, argv, "p:l:")) != -1) {
        switch (opt) {
            case 'p':
                try {
                    port = std::stoi(optarg);
                } catch (const std::exception &) {
                    std::cerr << "Error: invalid port number: " << optarg << std::endl;
                    return 1;
                }
                break;
            case 'l': log_path = optarg; break;
            default:
                std::cerr << "Usage: " << argv[0] << " -p <port_number> [-l <logfile>]" << std::endl;
                return 1;
        }
    }
    if (port <= 0 || port > 65535) {
        std::cerr << "Usage: " << argv[0] << " -p <port_number> [-l <logfile>]" << std::endl;
        return 1;
    }
    if (log_path.empty()) log_path = "nfs_client_" + std::to_string(port) + ".log";

    init_client_log(log_path);
    signal(SIGPIPE, SIG_IGN);
    int listen_fd = create_listen_socket(port);
    if (listen_fd < 0) {
        std::cerr << "Error: cannot create listen socket on port "
                  << port << ": " << strerror(errno) << "\n";
        return 1;
    }
    std::cout << "nfs_client listening on port " << port << std::endl;
    log_client("Client started, listening on port " + std::to_string(port));
    while (true) {
        struct sockaddr_in cli;
        socklen_t len = sizeof(cli);
        int fd = accept(listen_fd, (struct sockaddr*)&cli, &len);
        if (fd < 0) continue;
        std::thread(handle_manager_connection, fd).detach();
    }
    close(listen_fd);
    return 0;
}
