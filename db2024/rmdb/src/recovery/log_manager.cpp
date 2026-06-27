#include <cstring>
#include <iostream>
#include "log_manager.h"

lsn_t LogManager::add_log_to_buffer(LogRecord* log_record) {
    std::unique_lock<std::mutex> lock(latch_);
    log_record->lsn_ = global_lsn_.fetch_add(1);

    if (log_buffer_.is_full(log_record->log_tot_len_)) {
        flush_log_to_disk();
    }

    log_record->serialize(log_buffer_.buffer_ + log_buffer_.offset_);
    log_buffer_.offset_ += log_record->log_tot_len_;
    return log_record->lsn_;
}

void LogManager::flush_log_to_disk() {
    if (log_buffer_.offset_ == 0) return;
    int fd = disk_manager_->get_file_fd(LOG_FILE_NAME);
    disk_manager_->write_log(log_buffer_.buffer_, log_buffer_.offset_);
    persist_lsn_ = global_lsn_.load() - 1;
    log_buffer_.offset_ = 0;
    memset(log_buffer_.buffer_, 0, LOG_BUFFER_SIZE + 1);
}
