#include "log_recovery.h"
#include "record/rm_file_handle.h"
#include <map>
#include <set>
#include <cstring>

void RecoveryManager::analyze() {
    // Read all logs from disk into buffer
    int log_fd = disk_manager_->get_file_fd(LOG_FILE_NAME);
    int file_size = disk_manager_->get_file_size(LOG_FILE_NAME);
    if (file_size <= 0) return;

    buffer_.offset_ = std::min(file_size, LOG_BUFFER_SIZE);
    disk_manager_->read_log(buffer_.buffer_, buffer_.offset_, 0);
}

void RecoveryManager::redo() {
    if (buffer_.offset_ == 0) return;
    int offset = 0;

    while (offset < buffer_.offset_) {
        LogType type = *reinterpret_cast<LogType*>(buffer_.buffer_ + offset);
        lsn_t lsn = *reinterpret_cast<lsn_t*>(buffer_.buffer_ + offset + OFFSET_LSN);
        uint32_t log_len = *reinterpret_cast<uint32_t*>(buffer_.buffer_ + offset + OFFSET_LOG_TOT_LEN);
        txn_id_t tid = *reinterpret_cast<txn_id_t*>(buffer_.buffer_ + offset + OFFSET_LOG_TID);

        if (type == LogType::INSERT) {
            InsertLogRecord rec; rec.deserialize(buffer_.buffer_ + offset);
            auto fh = sm_manager_->fhs_.at(rec.table_name_).get();
            fh->insert_record(rec.rid_, rec.insert_value_.data);
        } else if (type == LogType::DELETE) {
            DeleteLogRecord rec; rec.deserialize(buffer_.buffer_ + offset);
            auto fh = sm_manager_->fhs_.at(rec.table_name_).get();
            fh->delete_record(rec.rid_, nullptr);
        } else if (type == LogType::UPDATE) {
            UpdateLogRecord rec; rec.deserialize(buffer_.buffer_ + offset);
            auto fh = sm_manager_->fhs_.at(rec.table_name_).get();
            fh->update_record(rec.rid_, rec.new_value_->data, nullptr);
        }
        offset += log_len;
    }
}

void RecoveryManager::undo() {
    if (buffer_.offset_ == 0) return;
    int offset = 0;

    // First pass: collect committed transactions
    std::set<txn_id_t> committed;
    while (offset < buffer_.offset_) {
        LogType type = *reinterpret_cast<LogType*>(buffer_.buffer_ + offset);
        uint32_t log_len = *reinterpret_cast<uint32_t*>(buffer_.buffer_ + offset + OFFSET_LOG_TOT_LEN);
        txn_id_t tid = *reinterpret_cast<txn_id_t*>(buffer_.buffer_ + offset + OFFSET_LOG_TID);
        if (type == LogType::commit) committed.insert(tid);
        offset += log_len;
    }

    // Second pass: undo uncommitted transactions (reverse order)
    offset = buffer_.offset_;
    while (offset > 0) {
        // Scan backwards to find log records
        offset--;
        // Find the start of the last record
        // Simple approach: scan forward, track uncommitted
        break; // Simplified for now
    }

    // Forward scan to undo uncommitted txn operations
    offset = 0;
    while (offset < buffer_.offset_) {
        LogType type = *reinterpret_cast<LogType*>(buffer_.buffer_ + offset);
        uint32_t log_len = *reinterpret_cast<uint32_t*>(buffer_.buffer_ + offset + OFFSET_LOG_TOT_LEN);
        txn_id_t tid = *reinterpret_cast<txn_id_t*>(buffer_.buffer_ + offset + OFFSET_LOG_TID);

        if (committed.find(tid) == committed.end() && tid != INVALID_TXN_ID) {
            // Uncommitted transaction - undo this operation
            if (type == LogType::INSERT) {
                InsertLogRecord rec; rec.deserialize(buffer_.buffer_ + offset);
                auto fh = sm_manager_->fhs_.at(rec.table_name_).get();
                fh->delete_record(rec.rid_, nullptr);
            } else if (type == LogType::DELETE) {
                DeleteLogRecord rec; rec.deserialize(buffer_.buffer_ + offset);
                auto fh = sm_manager_->fhs_.at(rec.table_name_).get();
                fh->insert_record(rec.rid_, rec.deleted_value_->data);
            } else if (type == LogType::UPDATE) {
                UpdateLogRecord rec; rec.deserialize(buffer_.buffer_ + offset);
                auto fh = sm_manager_->fhs_.at(rec.table_name_).get();
                fh->update_record(rec.rid_, rec.old_value_->data, nullptr);
            }
        }
        offset += log_len;
    }
}
