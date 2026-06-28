#include "log_recovery.h"
#include "record/rm_file_handle.h"
#include <set>
#include <cstring>

/**
 * @brief 从 table_name_ (非 null-terminated) 构建 std::string
 */
static std::string make_table_name(const char* name, size_t size) {
    return std::string(name, size);
}

void RecoveryManager::analyze() {
    // 循环读取整个日志文件到动态 buffer 中
    int file_size = disk_manager_->get_file_size(LOG_FILE_NAME);
    if (file_size <= 0) return;

    log_data_.resize(file_size);
    int total_read = 0;
    int offset = 0;

    while (total_read < file_size) {
        int chunk = std::min(file_size - total_read, LOG_BUFFER_SIZE);
        disk_manager_->read_log(log_data_.data() + total_read, chunk, offset + total_read);
        total_read += chunk;
    }
    log_data_size_ = total_read;
}

bool RecoveryManager::validate_log_boundary(int offset) {
    if (offset < 0 || offset + LOG_HEADER_SIZE > log_data_size_) return false;
    uint32_t log_len = *reinterpret_cast<uint32_t*>(log_data_.data() + offset + OFFSET_LOG_TOT_LEN);
    // 日志长度必须至少包含头部，且不能越界
    if (log_len < LOG_HEADER_SIZE) return false;
    if (offset + (int)log_len > log_data_size_) return false;
    return true;
}

bool RecoveryManager::is_valid_log_type(LogType type) {
    return type == LogType::INSERT || type == LogType::DELETE ||
           type == LogType::UPDATE || type == LogType::begin ||
           type == LogType::commit || type == LogType::ABORT;
}

void RecoveryManager::redo() {
    if (log_data_size_ == 0) return;

    // 重放所有 DML 日志（不跳过 ABORT 事务）。
    // 原因：abort() 只 flush 了 ABORT 日志记录，回滚操作修改的数据页/索引页
    // 可能未落盘。redo 必须重建崩溃瞬间的物理状态，再由 undo 逆序回滚
    // 未完成事务（包括 ABORT 事务，其回滚可能丢失）。
    int offset = 0;
    while (offset < log_data_size_) {
        if (offset + LOG_HEADER_SIZE > log_data_size_) break;

        LogType type = *reinterpret_cast<LogType*>(log_data_.data() + offset);
        uint32_t log_len = *reinterpret_cast<uint32_t*>(log_data_.data() + offset + OFFSET_LOG_TOT_LEN);

        if (!validate_log_boundary(offset) || !is_valid_log_type(type)) {
            break;
        }

        if (type == LogType::INSERT) {
            InsertLogRecord rec; rec.deserialize(log_data_.data() + offset);
            std::string table_name = make_table_name(rec.table_name_, rec.table_name_size_);
            auto fh = sm_manager_->fhs_.at(table_name).get();
            // 确保目标页存在：如果 insert 分配的页号超出当前文件头记录，
            // 补齐文件头（可能是在日志刷盘前崩溃，文件头未持久化）
            int needed_pages = rec.rid_.page_no + 1;
            while (fh->get_file_hdr().num_pages < needed_pages) {
                auto ph = fh->create_new_page_handle();
                sm_manager_->get_bpm()->unpin_page(ph.page->get_page_id(), true);
            }
            fh->insert_record(rec.rid_, rec.insert_value_.data);
        } else if (type == LogType::DELETE) {
            DeleteLogRecord rec; rec.deserialize(log_data_.data() + offset);
            std::string table_name = make_table_name(rec.table_name_, rec.table_name_size_);
            auto fh = sm_manager_->fhs_.at(table_name).get();
            fh->delete_record(rec.rid_, nullptr);
        } else if (type == LogType::UPDATE) {
            UpdateLogRecord rec; rec.deserialize(log_data_.data() + offset);
            std::string table_name = make_table_name(rec.table_name_, rec.table_name_size_);
            auto fh = sm_manager_->fhs_.at(table_name).get();
            // 确保目标页存在
            int needed_pages = rec.rid_.page_no + 1;
            while (fh->get_file_hdr().num_pages < needed_pages) {
                auto ph = fh->create_new_page_handle();
                sm_manager_->get_bpm()->unpin_page(ph.page->get_page_id(), true);
            }
            fh->update_record(rec.rid_, rec.new_value_->data, nullptr);
        }
        // begin/commit/ABORT 不需要 redo
        offset += log_len;
    }
}

void RecoveryManager::undo() {
    if (log_data_size_ == 0) return;

    // 第一遍扫描：只将 COMMIT 视为已完成。
    // ABORT 事务的回滚操作（数据页/索引页修改）可能未落盘，
    // 恢复时必须重新 undo 其 DML 来完成回滚。
    std::set<txn_id_t> active_txns;
    std::set<txn_id_t> completed;  // 只有 COMMIT
    int offset = 0;
    while (offset < log_data_size_) {
        if (!validate_log_boundary(offset)) break;

        LogType type = *reinterpret_cast<LogType*>(log_data_.data() + offset);
        uint32_t log_len = *reinterpret_cast<uint32_t*>(log_data_.data() + offset + OFFSET_LOG_TOT_LEN);
        txn_id_t tid = *reinterpret_cast<txn_id_t*>(log_data_.data() + offset + OFFSET_LOG_TID);

        if (type == LogType::commit) {
            completed.insert(tid);
        } else if (type == LogType::INSERT || type == LogType::DELETE || type == LogType::UPDATE) {
            if (completed.find(tid) == completed.end()) {
                active_txns.insert(tid);
            }
        }
        offset += log_len;
    }

    // 建立日志记录偏移量列表，用于逆序遍历
    std::vector<std::pair<int, uint32_t>> log_offsets;
    offset = 0;
    while (offset < log_data_size_) {
        if (!validate_log_boundary(offset)) break;

        uint32_t log_len = *reinterpret_cast<uint32_t*>(log_data_.data() + offset + OFFSET_LOG_TOT_LEN);
        LogType type = *reinterpret_cast<LogType*>(log_data_.data() + offset);
        txn_id_t tid = *reinterpret_cast<txn_id_t*>(log_data_.data() + offset + OFFSET_LOG_TID);

        // 只记录活跃事务（未提交）的 DML 日志。
        // ABORT 事务也在此列——其回滚操作可能未落盘，需 undo 来补完。
        if (active_txns.find(tid) != active_txns.end() &&
            (type == LogType::INSERT || type == LogType::DELETE || type == LogType::UPDATE)) {
            log_offsets.push_back({offset, log_len});
        }
        offset += log_len;
    }

    // 逆向撤销活跃事务的操作
    for (auto it = log_offsets.rbegin(); it != log_offsets.rend(); ++it) {
        offset = it->first;
        LogType type = *reinterpret_cast<LogType*>(log_data_.data() + offset);
        txn_id_t tid = *reinterpret_cast<txn_id_t*>(log_data_.data() + offset + OFFSET_LOG_TID);

        // 安全起见再检查一次：已完成的事务不 undo
        if (completed.find(tid) != completed.end()) continue;

        if (type == LogType::INSERT) {
            InsertLogRecord rec; rec.deserialize(log_data_.data() + offset);
            std::string table_name = make_table_name(rec.table_name_, rec.table_name_size_);
            auto fh = sm_manager_->fhs_.at(table_name).get();
            fh->delete_record(rec.rid_, nullptr);
        } else if (type == LogType::DELETE) {
            DeleteLogRecord rec; rec.deserialize(log_data_.data() + offset);
            std::string table_name = make_table_name(rec.table_name_, rec.table_name_size_);
            auto fh = sm_manager_->fhs_.at(table_name).get();
            fh->insert_record(rec.rid_, rec.deleted_value_->data);
        } else if (type == LogType::UPDATE) {
            UpdateLogRecord rec; rec.deserialize(log_data_.data() + offset);
            std::string table_name = make_table_name(rec.table_name_, rec.table_name_size_);
            auto fh = sm_manager_->fhs_.at(table_name).get();
            fh->update_record(rec.rid_, rec.old_value_->data, nullptr);
        }
    }
}

void RecoveryManager::truncate_log() {
    // 关闭并删除旧日志文件，创建新的空日志文件
    if (disk_manager_->GetLogFd() != -1) {
        disk_manager_->close_file(disk_manager_->GetLogFd());
    }
    if (disk_manager_->is_file(LOG_FILE_NAME)) {
        disk_manager_->destroy_file(LOG_FILE_NAME);
    }
    disk_manager_->create_file(LOG_FILE_NAME);
    disk_manager_->SetLogFd(-1);  // 下次 write_log 时会重新打开
}
