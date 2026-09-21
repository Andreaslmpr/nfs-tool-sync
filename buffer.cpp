#include "buffer.h"
#include <algorithm>

BoundedBuffer::BoundedBuffer(size_t capacity)
    :capacity_(capacity > 0 ? capacity : 1), shutting_down_(false), draining_(false) {}

BoundedBuffer::~BoundedBuffer() {
    //Default Constructor
}

bool BoundedBuffer::push(const SyncTask &task) {
    std::unique_lock<std::mutex> lk(mtx_);
    cv_full_.wait(lk, [this]() { return queue_.size() < capacity_ || shutting_down_ || draining_; });
    if (shutting_down_ || draining_) return false;
    queue_.push(task);
    cv_empty_.notify_one();
    return true;
}

SyncTask BoundedBuffer::pop(bool &shutdown_flag) {
    std::unique_lock<std::mutex> lk(mtx_);
    cv_empty_.wait(lk, [this]() { return !queue_.empty() || shutting_down_ || draining_; });
    // Στο graceful drain οι workers φεύγουν μόνο όταν αδειάσει η ουρά.
    if (queue_.empty() && (shutting_down_ || draining_)) {
        shutdown_flag = true;
        return SyncTask();
    }//close the file
    SyncTask task = queue_.front();
    queue_.pop();
    cv_full_.notify_one();
    shutdown_flag = false;
    return task;
}

//queue using
void BoundedBuffer::remove_tasks_with_source(const std::string &source_host, int source_port, const std::string &source_dir) {
    std::lock_guard<std::mutex> lk(mtx_);
    std::queue<SyncTask> tmp;
    while(!queue_.empty()){
        SyncTask t = queue_.front();
        queue_.pop();
        if (t.source_host == source_host && t.source_port == source_port && t.source_dir == source_dir) {
        } else {
            tmp.push(t);
        }
    }
    std::swap(queue_, tmp);
    cv_full_.notify_all();
}

void BoundedBuffer::drain_and_stop() {
    std::lock_guard<std::mutex> lk(mtx_);
    draining_ = true;
    // ξυπνάμε όσους περιμένουν ώστε να δουν τη νέα κατάσταση
    cv_full_.notify_all();
    cv_empty_.notify_all();
}

void BoundedBuffer::shutdown() {
    std::lock_guard<std::mutex> lk(mtx_);
    shutting_down_ = true;
    std::queue<SyncTask> tmp;
    std::swap(queue_, tmp);
    // wake up any waiting pushers/poppers so they can finish
    cv_full_.notify_all();
    cv_empty_.notify_all();
}

size_t BoundedBuffer::size() {
    std::lock_guard<std::mutex> lk(mtx_);
    return queue_.size();
}
