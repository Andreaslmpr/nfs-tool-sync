#ifndef BUFFER_H
#define BUFFER_H

#include <queue>
#include <mutex>
#include <condition_variable>
#include <string>

// Δομή που περιγράφει ένα task συγχρονισμού 1 αρχείου
struct SyncTask {
    std::string source_host;
    int         source_port;
    std::string source_dir;
    std::string target_host;
    int         target_port;
    std::string target_dir;
    std::string filename;
};

class BoundedBuffer {
public:
    BoundedBuffer(size_t capacity);
    ~BoundedBuffer();

    // Μπλοκάρει όσο ο buffer είναι γεμάτος. Επιστρέφει false αν ο buffer κλείνει.
    bool push(const SyncTask &task);
    // Μπλοκάρει όσο ο buffer είναι άδειος. Θέτει shutdown_flag=true όταν
    // δεν πρόκειται να έρθουν άλλα tasks (drain_and_stop/shutdown).
    SyncTask pop(bool &shutdown_flag);

    //CLEANUP tasks and terminates
    void remove_tasks_with_source(const std::string &source_host, int source_port, const std::string &source_dir);

    // Graceful: δεν δέχεται νέα tasks, τα ήδη υπάρχοντα εκτελούνται και μετά
    // οι workers τερματίζουν (χρησιμοποιείται από την εντολή shutdown).
    void drain_and_stop();
    // Immediate: πετάει ό,τι έχει μείνει στην ουρά και ξυπνάει όλους.
    void shutdown();

    size_t size();

private:
    std::queue<SyncTask> queue_;
    std::mutex            mtx_;
    std::condition_variable cv_full_;
    std::condition_variable cv_empty_;
    size_t                capacity_;
    bool                  shutting_down_;
    bool                  draining_;
};

#endif // BUFFER_H
