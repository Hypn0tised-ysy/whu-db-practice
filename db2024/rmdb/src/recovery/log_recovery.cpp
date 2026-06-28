#include "log_recovery.h"
#include "record/rm_file_handle.h"
#include <map>
#include <set>
#include <cstring>
#include <vector>

/**
 * @brief 从 table_name_ (非 null-terminated) 构建 std::string
 */
static std::string make_table_name(const char* name, size_t size) {
    return std::string(name, size);
}

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
            std::string table_name = make_table_name(rec.table_name_, rec.table_name_size_);
            auto fh = sm_manager_->fhs_.at(table_name).get();
            fh->insert_record(rec.rid_, rec.insert_value_.data);
        } else if (type == LogType::DELETE) {
            DeleteLogRecord rec; rec.deserialize(buffer_.buffer_ + offset);
            std::string table_name = make_table_name(rec.table_name_, rec.table_name_size_);
            auto fh = sm_manager_->fhs_.at(table_name).get();
            fh->delete_record(rec.rid_, nullptr);
        } else if (type == LogType::UPDATE) {
            UpdateLogRecord rec; rec.deserialize(buffer_.buffer_ + offset);
            std::string table_name = make_table_name(rec.table_name_, rec.table_name_size_);
            auto fh = sm_manager_->fhs_.at(table_name).get();
            fh->update_record(rec.rid_, rec.new_value_->data, nullptr);
        }
        offset += log_len;
    }
}

void RecoveryManager::undo() {
    if (buffer_.offset_ == 0) return;

    // 第一遍扫描：收集已提交的事务ID
    std::set<txn_id_t> committed;
    int offset = 0;
    while (offset < buffer_.offset_) {
        LogType type = *reinterpret_cast<LogType*>(buffer_.buffer_ + offset);
        uint32_t log_len = *reinterpret_cast<uint32_t*>(buffer_.buffer_ + offset + OFFSET_LOG_TOT_LEN);
        txn_id_t tid = *reinterpret_cast<txn_id_t*>(buffer_.buffer_ + offset + OFFSET_LOG_TID);
        if (type == LogType::commit) committed.insert(tid);
        offset += log_len;
    }

    // 建立日志记录偏移量列表，用于逆序遍历
    std::vector<std::pair<int, uint32_t>> log_offsets;
    offset = 0;
    while (offset < buffer_.offset_) {
        uint32_t log_len = *reinterpret_cast<uint32_t*>(buffer_.buffer_ + offset + OFFSET_LOG_TOT_LEN);
        log_offsets.push_back({offset, log_len});
        offset += log_len;
    }

    // 逆向撤销未提交事务的操作（逆序=正确的回滚顺序）
    for (auto it = log_offsets.rbegin(); it != log_offsets.rend(); ++it) {
        offset = it->first;
        uint32_t log_len = it->second;
        LogType type = *reinterpret_cast<LogType*>(buffer_.buffer_ + offset);
        txn_id_t tid = *reinterpret_cast<txn_id_t*>(buffer_.buffer_ + offset + OFFSET_LOG_TID);

        // 跳过已提交事务和非事务记录
        if (committed.find(tid) != committed.end() || tid == INVALID_TXN_ID) continue;

        if (type == LogType::INSERT) {
            InsertLogRecord rec; rec.deserialize(buffer_.buffer_ + offset);
            std::string table_name = make_table_name(rec.table_name_, rec.table_name_size_);
            auto fh = sm_manager_->fhs_.at(table_name).get();
            fh->delete_record(rec.rid_, nullptr);
        } else if (type == LogType::DELETE) {
            DeleteLogRecord rec; rec.deserialize(buffer_.buffer_ + offset);
            std::string table_name = make_table_name(rec.table_name_, rec.table_name_size_);
            auto fh = sm_manager_->fhs_.at(table_name).get();
            fh->insert_record(rec.rid_, rec.deleted_value_->data);
        } else if (type == LogType::UPDATE) {
            UpdateLogRecord rec; rec.deserialize(buffer_.buffer_ + offset);
            std::string table_name = make_table_name(rec.table_name_, rec.table_name_size_);
            auto fh = sm_manager_->fhs_.at(table_name).get();
            fh->update_record(rec.rid_, rec.old_value_->data, nullptr);
        }
    }
}
