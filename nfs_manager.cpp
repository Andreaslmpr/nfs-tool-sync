#include <iostream>
#include <vector>
#include <thread>
#include <atomic>
#include <mutex>
#include <cstring>
#include <cerrno>
#include <unistd.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/syscall.h>
#include <fcntl.h>
#include <csignal>
#include <sstream>
#include <iomanip>
#include <stdexcept>
#include <chrono>
#include "common.h"
#include "config_parser.h"
#include "logger.h"
#include "buffer.h"


//static methoods
static std::vector<SyncInfo> all_syncs;
static std::mutex all_syncs_mtx;
static BoundedBuffer* task_buffer = nullptr;
static std::atomic<bool> manager_running(false);
static std::vector<std::thread> worker_threads;
static std::thread command_thread;
static int cmd_listen_fd = -1;
static std::once_flag workers_joined_flag;
static std::mutex shutdown_mtx;
static bool shutdown_started = false;


// ---------------------------------------------------------------- helpers --

// "/dir/file@host:port" όπως το ζητάει η μορφή των logs
static std::string endpoint(const std::string &dir, const std::string &host,
                            int port, const std::string &file = "") {
    std::string p = dir;
    if (!file.empty()) p += "/" + file;
    return p + "@" + host + ":" + std::to_string(port);
}

static long thread_pid() {
    return (long)syscall(SYS_gettid);
}

//split "path@host:port" into its parts
static void split_hostport(const std::string &in,
                           std::string &path,
                           std::string &host,
                           int &port) {
    auto at_pos = in.find('@');
    if (at_pos == std::string::npos)
        throw std::runtime_error("Invalid format (missing @): " + in);
    path = in.substr(0, at_pos);
    std::string hp = in.substr(at_pos + 1);
    auto colon_pos = hp.find(':');
    if (colon_pos == std::string::npos)
        throw std::runtime_error("Invalid format (missing :): " + in);
    host = hp.substr(0, colon_pos);
    try {
        port = std::stoi(hp.substr(colon_pos + 1));
    } catch (const std::exception &) {
        throw std::runtime_error("Invalid port number: " + in);
    }
}

// Το cancel δέχεται είτε "/dir" είτε "/dir@host:port" (η εκφώνηση δίνει σκέτο dir)
static void split_hostport_optional(const std::string &in,
                                    std::string &path,
                                    std::string &host,
                                    int &port) {
    if (in.find('@') == std::string::npos) {
        path = in;
        host.clear();
        port = 0;
        return;
    }
    split_hostport(in, path, host, port);
}

static int connect_to(const std::string &host, int port) {
    struct addrinfo hints{}, *res, *p;
    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%d", port);
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host.c_str(), port_str, &hints, &res) != 0) return -1;
    for (p = res; p; p = p->ai_next) {
        int sockfd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
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

static bool send_all(int fd, const char *buf, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = send(fd, buf + sent, len - sent, 0);
        if (n <= 0) return false;
        sent += n;
    }
    return true;
}

static bool recv_all(int fd, char *buf, size_t len) {
    size_t recvd = 0;
    while (recvd < len) {
        ssize_t n = recv(fd, buf + recvd, len - recvd, 0);
        if (n <= 0) return false;
        recvd += n;
    }
    return true;
}

// Διαβάζει token μέχρι ' ' ή '\n', ένα byte τη φορά, ώστε να μην καταναλωθούν
// κατά λάθος τα binary δεδομένα που ακολουθούν το header.
static bool recv_token(int fd, std::string &out, char *delim = nullptr) {
    out.clear();
    char c;
    while (true) {
        ssize_t n = recv(fd, &c, 1, 0);
        if (n <= 0) return false;
        if (c == ' ' || c == '\n') {
            if (delim) *delim = c;
            return true;
        }
        if (c == '\r') continue;
        out.push_back(c);
    }
}

static bool recv_line(int fd, std::string &out) {
    out.clear();
    char c;
    while (true) {
        ssize_t n = recv(fd, &c, 1, 0);
        if (n <= 0) return !out.empty();
        if (c == '\n') return true;
        if (c == '\r') continue;
        out.push_back(c);
    }
}

// [TIMESTAMP] [SOURCE_DIR] [TARGET_DIR] [THREAD_PID] [OPERATION] [RESULT] [DETAILS]
static void log_operation(const SyncTask &t, const std::string &operation,
                          const std::string &result, const std::string &details) {
    log_manager("[" + endpoint(t.source_dir, t.source_host, t.source_port, t.filename) + "] "
                "[" + endpoint(t.target_dir, t.target_host, t.target_port, t.filename) + "] "
                "[" + std::to_string(thread_pid()) + "] "
                "[" + operation + "] [" + result + "] [" + details + "]");
}

// Μήνυμα που γράφεται στο manager log και (αν υπάρχει console) στέλνεται και εκεί
static void notify(int console_fd, const std::string &msg) {
    log_manager(msg);
    if (console_fd >= 0) {
        std::string line = "[" + current_timestamp() + "] " + msg + "\n";
        send_all(console_fd, line.c_str(), line.size());
    }
}

static void record_error(const std::string &src_host, int src_port, const std::string &src_dir) {
    std::lock_guard<std::mutex> lk(all_syncs_mtx);
    for (auto &si : all_syncs) {
        if (si.source_host == src_host && si.source_port == src_port && si.source_dir == src_dir) {
            si.error_count++;
            break;
        }
    }
}

static void record_success(const SyncTask &t) {
    std::lock_guard<std::mutex> lk(all_syncs_mtx);
    for (auto &si : all_syncs) {
        if (si.source_host == t.source_host && si.source_port == t.source_port
         && si.source_dir  == t.source_dir  && si.target_host == t.target_host
         && si.target_port == t.target_port && si.target_dir  == t.target_dir) {
            si.last_sync_time = std::time(nullptr);
            break;
        }
    }
}


// ---------------------------------------------------------------- workers --

// PULL από τον source client. Επιστρέφει false σε σφάλμα (το log έχει ήδη γραφτεί).
static bool do_pull(const SyncTask &t, std::vector<char> &data) {
    int src = connect_to(t.source_host, t.source_port);
    if (src < 0) {
        log_operation(t, "PULL", "ERROR", "File: " + t.filename
                      + " Cannot connect to " + t.source_host + ":" + std::to_string(t.source_port));
        record_error(t.source_host, t.source_port, t.source_dir);
        return false;
    }
    std::string pull = "PULL " + t.source_dir + "/" + t.filename + "\n";
    if (!send_all(src, pull.c_str(), pull.size())) {
        close(src);
        log_operation(t, "PULL", "ERROR", "File: " + t.filename + " Error sending PULL command");
        record_error(t.source_host, t.source_port, t.source_dir);
        return false;
    }

    // Απάντηση: <filesize><space><data>  ή  "-1 <error message>\n"
    std::string size_tok;
    if (!recv_token(src, size_tok)) {
        close(src);
        log_operation(t, "PULL", "ERROR", "File: " + t.filename + " No response from source");
        record_error(t.source_host, t.source_port, t.source_dir);
        return false;
    }
    long fsize = -1;
    try {
        fsize = std::stol(size_tok);
    } catch (const std::exception &) {
        fsize = -1;
    }
    if (fsize < 0) {
        std::string errmsg;
        recv_line(src, errmsg);
        close(src);
        if (errmsg.empty()) errmsg = "Cannot read file";
        log_operation(t, "PULL", "ERROR", "File: " + t.filename + " " + errmsg);
        record_error(t.source_host, t.source_port, t.source_dir);
        return false;
    }

    data.assign((size_t)fsize, 0);
    if (fsize > 0 && !recv_all(src, data.data(), (size_t)fsize)) {
        close(src);
        log_operation(t, "PULL", "ERROR", "File: " + t.filename + " Error receiving data");
        record_error(t.source_host, t.source_port, t.source_dir);
        return false;
    }
    close(src);
    log_operation(t, "PULL", "SUCCESS", std::to_string(fsize) + " bytes pulled");
    return true;
}

// PUSH προς τον target client.
static bool do_push(const SyncTask &t, const std::vector<char> &data) {
    int tgt = connect_to(t.target_host, t.target_port);
    if (tgt < 0) {
        log_operation(t, "PUSH", "ERROR", "File: " + t.filename
                      + " Cannot connect to " + t.target_host + ":" + std::to_string(t.target_port));
        record_error(t.source_host, t.source_port, t.source_dir);
        return false;
    }
    const std::string path = t.target_dir + "/" + t.filename;
    bool ok = true;

    // 1) chunk_size == -1 : δημιουργία / άδειασμα του αρχείου
    std::string init = "PUSH " + path + " -1\n";
    ok = send_all(tgt, init.c_str(), init.size());

    // 2) τα δεδομένα σε chunks
    const size_t CHUNK = 4096;
    size_t off = 0;
    while (ok && off < data.size()) {
        size_t csz = std::min(CHUNK, data.size() - off);
        std::string hdr = "PUSH " + path + " " + std::to_string(csz) + " ";
        ok = send_all(tgt, hdr.c_str(), hdr.size())
          && send_all(tgt, data.data() + off, csz);
        off += csz;
    }

    // 3) chunk_size == 0 : τέλος αρχείου
    if (ok) {
        std::string eof = "PUSH " + path + " 0\n";
        ok = send_all(tgt, eof.c_str(), eof.size());
    }
    close(tgt);

    if (!ok) {
        log_operation(t, "PUSH", "ERROR", "File: " + t.filename + " Error sending data to target");
        record_error(t.source_host, t.source_port, t.source_dir);
        return false;
    }
    log_operation(t, "PUSH", "SUCCESS", std::to_string(data.size()) + " bytes pushed");
    record_success(t);
    return true;
}

static void worker_thread_function(int id) {
    (void)id;
    while (true) {
        bool shutdown_flag = false;
        SyncTask task = task_buffer->pop(shutdown_flag);
        if (shutdown_flag) break;
        if (task.filename.empty()) continue;

        std::vector<char> data;
        if (!do_pull(task, data)) continue;
        do_push(task, data);
    }
}


// ------------------------------------------------------- LIST & enqueueing --

// Ρωτάει τον source client για τα αρχεία του φακέλου και βάζει ένα task ανά αρχείο.
static void list_and_enqueue(const SyncInfo &si, int console_fd) {
    int sock = connect_to(si.source_host, si.source_port);
    if (sock < 0) {
        notify(console_fd, "Error connecting to source "
               + endpoint(si.source_dir, si.source_host, si.source_port)
               + ": " + strerror(errno));
        record_error(si.source_host, si.source_port, si.source_dir);
        return;
    }
    std::string cmd = "LIST " + si.source_dir + "\n";
    if (!send_all(sock, cmd.c_str(), cmd.size())) {
        close(sock);
        notify(console_fd, "Error sending LIST to "
               + endpoint(si.source_dir, si.source_host, si.source_port));
        record_error(si.source_host, si.source_port, si.source_dir);
        return;
    }

    std::string fn;
    while (recv_line(sock, fn)) {
        if (fn == ".") break;        // τέλος λίστας
        if (fn.empty()) continue;
        SyncTask t{si.source_host, si.source_port, si.source_dir,
                   si.target_host, si.target_port, si.target_dir, fn};
        if (!task_buffer->push(t)) break;   // ο manager τερματίζει
        notify(console_fd, "Added file: "
               + endpoint(si.source_dir, si.source_host, si.source_port, fn) + " -> "
               + endpoint(si.target_dir, si.target_host, si.target_port, fn));
    }
    close(sock);
}

static void initial_load_config_to_buffer() {
    std::vector<SyncInfo> snapshot;
    {
        std::lock_guard<std::mutex> lk(all_syncs_mtx);
        snapshot = all_syncs;
    }
    if (snapshot.empty()) {
        log_manager("No syncs configured");
        return;
    }
    for (const auto &si : snapshot) {
        if (!manager_running.load()) break;
        if (!si.active) {
            log_manager("Skipping inactive sync: "
                        + endpoint(si.source_dir, si.source_host, si.source_port));
            continue;
        }
        list_and_enqueue(si, -1);
    }
}


// ------------------------------------------------------- console commands --

static void join_workers_once() {
    std::call_once(workers_joined_flag, []() {
        for (auto &w : worker_threads) if (w.joinable()) w.join();
    });
}

// shutdown: τερματίζει τα ήδη ενεργά tasks, αδειάζει την ουρά και σταματάει
static void perform_shutdown(int console_fd) {
    {
        std::lock_guard<std::mutex> lk(shutdown_mtx);
        if (shutdown_started) {
            notify(console_fd, "Manager is already shutting down");
            return;
        }
        shutdown_started = true;
    }
    notify(console_fd, "Shutting down manager...");
    notify(console_fd, "Waiting for all active workers to finish.");
    notify(console_fd, "Processing remaining queued tasks.");

    task_buffer->drain_and_stop();   // τα queued tasks ολοκληρώνονται κανονικά
    join_workers_once();

    notify(console_fd, "Manager shutdown complete.");

    manager_running.store(false);
    // ξυπνάει το accept() του command_listener ώστε να τερματίσει το thread
    if (cmd_listen_fd >= 0) ::shutdown(cmd_listen_fd, SHUT_RDWR);
}

static void handle_add(std::istringstream &iss, int fd) {
    std::string src_token, tgt_token;
    if (!(iss >> src_token >> tgt_token)) {
        notify(fd, "Usage: add <source_dir@host:port> <target_dir@host:port>");
        return;
    }
    SyncInfo si;
    si.active = true;
    si.last_sync_time = 0;
    si.error_count    = 0;
    try {
        split_hostport(src_token, si.source_dir, si.source_host, si.source_port);
        split_hostport(tgt_token, si.target_dir, si.target_host, si.target_port);
    } catch (const std::exception &e) {
        notify(fd, std::string("Invalid add syntax: ") + e.what());
        return;
    }

    bool already_active = false;
    {
        std::lock_guard<std::mutex> lk(all_syncs_mtx);
        bool found = false;
        for (auto &s : all_syncs) {
            if (s.source_host == si.source_host && s.source_port == si.source_port
             && s.source_dir  == si.source_dir) {
                found = true;
                if (s.active) already_active = true;
                else {                       // είχε γίνει cancel -> ξανα-ενεργοποιείται
                    s.active      = true;
                    s.target_dir  = si.target_dir;
                    s.target_host = si.target_host;
                    s.target_port = si.target_port;
                }
                break;
            }
        }
        if (!found) all_syncs.push_back(si);
    }

    if (already_active) {
        notify(fd, "Already in queue: "
               + endpoint(si.source_dir, si.source_host, si.source_port));
        return;
    }

    // LIST + enqueue των αρχείων του νέου φακέλου
    list_and_enqueue(si, fd);
}

static void handle_cancel(std::istringstream &iss, int fd) {
    std::string src_token;
    if (!(iss >> src_token)) {
        notify(fd, "Usage: cancel <source_dir>");
        return;
    }
    std::string src_dir, host;
    int port = 0;
    try {
        split_hostport_optional(src_token, src_dir, host, port);
    } catch (const std::exception &e) {
        notify(fd, std::string("Invalid cancel syntax: ") + e.what());
        return;
    }

    bool found = false;
    std::string full;
    std::string match_host;
    int match_port = 0;
    {
        std::lock_guard<std::mutex> lk(all_syncs_mtx);
        for (auto &si : all_syncs) {
            if (si.source_dir != src_dir) continue;
            if (!host.empty() && (si.source_host != host || si.source_port != port)) continue;
            if (!si.active) continue;          // έχει ήδη σταματήσει
            si.active  = false;
            found      = true;
            full       = endpoint(si.source_dir, si.source_host, si.source_port);
            match_host = si.source_host;
            match_port = si.source_port;
            break;
        }
    }

    if (!found) {
        notify(fd, "Directory not being synchronized: " + src_token);
        return;
    }
    // τα tasks που περιμένουν στην ουρά για αυτόν τον φάκελο ακυρώνονται
    task_buffer->remove_tasks_with_source(match_host, match_port, src_dir);
    notify(fd, "Synchronization stopped for " + full);
}

// Μία εντολή ανά σύνδεση: διαβάζεται, εκτελείται, απαντάμε και κλείνουμε
// (ο nfs_console ανοίγει νέα σύνδεση για κάθε εντολή).
static void handle_console_commands(int fd) {
    std::string line;
    if (!recv_line(fd, line)) {
        close(fd);
        return;
    }
    while (!line.empty() && (line.back() == ' ' || line.back() == '\t')) line.pop_back();
    if (line.empty()) {
        close(fd);
        return;
    }

    std::istringstream iss(line);
    std::string cmd;
    iss >> cmd;

    try {
        if (cmd == "add") {
            handle_add(iss, fd);
        } else if (cmd == "cancel") {
            handle_cancel(iss, fd);
        } else if (cmd == "shutdown") {
            perform_shutdown(fd);
        } else {
            notify(fd, "Unknown command: " + cmd);
        }
    } catch (const std::exception &e) {
        // καμία εντολή δεν πρέπει να ρίξει τον manager
        notify(fd, std::string("Error while executing command: ") + e.what());
    }
    close(fd);
}

static void command_listener(int port) {
    cmd_listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (cmd_listen_fd < 0) {
        std::cerr << "Error: cannot create command socket: " << strerror(errno) << std::endl;
        manager_running.store(false);
        return;
    }
    int opt = 1;
    setsockopt(cmd_listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);
    if (bind(cmd_listen_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        std::cerr << "Error: cannot bind port " << port << ": " << strerror(errno) << std::endl;
        log_manager("Error: cannot bind command port " + std::to_string(port));
        close(cmd_listen_fd);
        cmd_listen_fd = -1;
        manager_running.store(false);
        return;
    }
    listen(cmd_listen_fd, 10);
    log_manager("Manager listening for console commands on port " + std::to_string(port));

    while (manager_running.load()) {
        int nf = accept(cmd_listen_fd, nullptr, nullptr);
        if (nf < 0) {
            if (!manager_running.load()) break;   // shutdown() στο listening socket
            if (errno == EINTR) continue;
            break;
        }
        std::thread t(handle_console_commands, nf);
        t.detach();
    }
    close(cmd_listen_fd);
    cmd_listen_fd = -1;
}


// ------------------------------------------------------------------- main --

static void usage(const char *prog) {
    std::cerr << "Usage: " << prog
              << " -l <manager_logfile> -c <config_file> -n <worker_limit>"
              << " -p <port_number> -b <bufferSize>" << std::endl;
}

int main(int argc, char *argv[]) {
    signal(SIGPIPE, SIG_IGN);

    std::string logfile, configfile;
    int workers = 0, port = 0, bufsize = 0;
    int opt;

    while ((opt = getopt(argc, argv, "l:c:n:p:b:")) != -1) {
        try {
            switch (opt) {
                case 'l': logfile    = optarg; break;
                case 'c': configfile = optarg; break;
                case 'n': workers    = std::stoi(optarg); break;
                case 'p': port       = std::stoi(optarg); break;
                case 'b': bufsize    = std::stoi(optarg); break;
                default:  usage(argv[0]); return 1;
            }
        } catch (const std::exception &) {
            std::cerr << "Error: invalid numeric argument for -" << (char)opt << std::endl;
            return 1;
        }
    }
    if (logfile.empty() || configfile.empty() || workers <= 0 || port <= 0 || bufsize <= 0) {
        usage(argv[0]);
        return 1;
    }

    init_manager_log(logfile);
    try {
        all_syncs = parse_config_file(configfile);
    } catch (const std::exception &e) {
        std::cerr << "Error: " << e.what() << std::endl;
        log_manager(std::string("Error reading config file: ") + e.what());
        return 1;
    }

    task_buffer = new BoundedBuffer(bufsize);
    manager_running.store(true);
    for (int i = 0; i < workers; ++i)
        worker_threads.emplace_back(worker_thread_function, i);

    // Ο listener ξεκινάει πρώτος ώστε το console να μπορεί να συνδεθεί
    // ακόμη κι όσο τρέχει ο αρχικός συγχρονισμός.
    command_thread = std::thread(command_listener, port);
    initial_load_config_to_buffer();

    while (manager_running.load())
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

    task_buffer->drain_and_stop();
    join_workers_once();
    if (command_thread.joinable()) command_thread.join();
    delete task_buffer;
    return 0;
}
