/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "transaction_manager.h"
#include "record/rm_file_handle.h"
#include "system/sm_manager.h"

std::unordered_map<txn_id_t, Transaction *> TransactionManager::txn_map = {};

/**
 * @description: 事务的开始方法
 * @return {Transaction*} 开始事务的指针
 * @param {Transaction*} txn 事务指针，空指针代表需要创建新事务，否则开始已有事务
 * @param {LogManager*} log_manager 日志管理器指针
 */
Transaction * TransactionManager::begin(Transaction* txn, LogManager* log_manager) {
    // Todo:
    // 1. 判断传入事务参数是否为空指针
    // 2. 如果为空指针，创建新事务
    // 3. 把开始事务加入到全局事务表中
    // 4. 返回当前事务指针
    if (txn == nullptr) {
        txn_id_t txn_id = next_txn_id_.fetch_add(1);
        txn = new Transaction(txn_id);
        txn->set_state(TransactionState::GROWING);
        std::unique_lock<std::mutex> lock(latch_);
        txn_map[txn_id] = txn;
        // 记录begin日志
        if (log_manager != nullptr) {
            auto log_rec = new BeginLogRecord(txn_id);
            log_rec->prev_lsn_ = txn->get_prev_lsn();
            txn->set_prev_lsn(log_manager->add_log_to_buffer(log_rec));
        }
    }
    return txn;
}

/**
 * @description: 事务的提交方法
 * @param {Transaction*} txn 需要提交的事务
 * @param {LogManager*} log_manager 日志管理器指针
 */
void TransactionManager::commit(Transaction* txn, LogManager* log_manager) {
    // Todo:
    // 1. 如果存在未提交的写操作，提交所有的写操作
    // 2. 释放所有锁
    // 3. 释放事务相关资源，eg.锁集
    // 4. 把事务日志刷入磁盘中
    // 5. 更新事务状态
    if (txn == nullptr || txn->get_state() == TransactionState::COMMITTED
        || txn->get_state() == TransactionState::ABORTED) {
        return;
    }
    // 释放所有锁
    auto lock_set = txn->get_lock_set();
    for (auto &lid : *lock_set) {
        lock_manager_->unlock(txn, lid);
    }
    lock_set->clear();
    // 记录commit日志并刷盘
    if (log_manager != nullptr) {
        auto log_rec = new CommitLogRecord(txn->get_transaction_id());
        log_rec->prev_lsn_ = txn->get_prev_lsn();
        txn->set_prev_lsn(log_manager->add_log_to_buffer(log_rec));
        log_manager->flush_log_to_disk();
    }
    // 刷所有表的数据页和文件头到磁盘
    for (auto &entry : sm_manager_->fhs_) {
        int fd = entry.second->GetFd();
        // 先写文件头（包含num_pages等元数据）
        auto fhdr = entry.second->get_file_hdr();
        sm_manager_->get_disk_manager()->write_page(fd, RM_FILE_HDR_PAGE, (char*)&fhdr, sizeof(RmFileHdr));
        // 再刷所有缓冲池页面
        sm_manager_->get_bpm()->flush_all_pages(fd);
    }
    // 刷所有索引页到磁盘
    for (auto &entry : sm_manager_->ihs_) {
        sm_manager_->get_ix_manager()->flush_index(entry.second.get());
    }
    txn->set_state(TransactionState::COMMITTED);
}

/**
 * @description: 事务的终止（回滚）方法
 * @param {Transaction *} txn 需要回滚的事务
 * @param {LogManager} *log_manager 日志管理器指针
 */
void TransactionManager::abort(Transaction * txn, LogManager *log_manager) {
    // Todo:
    // 1. 回滚所有写操作
    // 2. 释放所有锁
    // 3. 清空事务相关资源，eg.锁集
    // 4. 把事务日志刷入磁盘中
    // 5. 更新事务状态
    if (txn == nullptr || txn->get_state() == TransactionState::ABORTED) {
        return;
    }
    // 回滚所有写操作（逆序回滚）
    auto &write_set = *txn->get_write_set();
    while (!write_set.empty()) {
        WriteRecord *wr = write_set.back();
        write_set.pop_back();
        std::string &tab_name = wr->GetTableName();
        RmFileHandle *fh = sm_manager_->fhs_.at(tab_name).get();
        if (wr->GetWriteType() == WType::INSERT_TUPLE) {
            // INSERT → 删除插入的记录
            fh->delete_record(wr->GetRid(), nullptr);
            // 也删除索引条目
            TabMeta &tab = sm_manager_->db_.get_table(tab_name);
            for (auto &index : tab.indexes) {
                auto ih = sm_manager_->ihs_.at(sm_manager_->get_ix_manager()->get_index_name(tab_name, index.cols)).get();
                auto rec = fh->get_record(wr->GetRid(), nullptr);
                char *key = new char[index.col_tot_len];
                int off = 0;
                for (auto &col : index.cols) { memcpy(key+off, rec->data+col.offset, col.len); off+=col.len; }
                ih->delete_entry(key, txn);
                delete[] key;
            }
        } else if (wr->GetWriteType() == WType::DELETE_TUPLE) {
            // DELETE → 重新插入旧记录
            fh->insert_record(wr->GetRid(), wr->GetRecord().data);
            TabMeta &tab = sm_manager_->db_.get_table(tab_name);
            for (auto &index : tab.indexes) {
                auto ih = sm_manager_->ihs_.at(sm_manager_->get_ix_manager()->get_index_name(tab_name, index.cols)).get();
                char *key = new char[index.col_tot_len];
                int off = 0;
                for (auto &col : index.cols) { memcpy(key+off, wr->GetRecord().data+col.offset, col.len); off+=col.len; }
                ih->insert_entry(key, wr->GetRid(), txn);
                delete[] key;
            }
        } else if (wr->GetWriteType() == WType::UPDATE_TUPLE) {
            // UPDATE → 恢复旧记录
            // 先读取当前记录（新值），用于删除新索引条目
            auto cur_rec = fh->get_record(wr->GetRid(), nullptr);
            TabMeta &tab = sm_manager_->db_.get_table(tab_name);

            // 构建并删除新索引条目（从当前记录中获取 key）
            for (auto &index : tab.indexes) {
                auto ih = sm_manager_->ihs_.at(sm_manager_->get_ix_manager()->get_index_name(tab_name, index.cols)).get();
                char *new_key = new char[index.col_tot_len];
                int off = 0;
                for (auto &col : index.cols) { memcpy(new_key+off, cur_rec->data+col.offset, col.len); off+=col.len; }
                ih->delete_entry(new_key, txn);
                delete[] new_key;
            }

            // 恢复旧记录
            fh->update_record(wr->GetRid(), wr->GetRecord().data, nullptr);

            // 插入旧索引条目（从 WriteRecord 中获取 key）
            for (auto &index : tab.indexes) {
                auto ih = sm_manager_->ihs_.at(sm_manager_->get_ix_manager()->get_index_name(tab_name, index.cols)).get();
                char *old_key = new char[index.col_tot_len];
                int off = 0;
                for (auto &col : index.cols) { memcpy(old_key+off, wr->GetRecord().data+col.offset, col.len); off+=col.len; }
                ih->insert_entry(old_key, wr->GetRid(), txn);
                delete[] old_key;
            }
        }
        delete wr;
    }
    // 释放所有锁
    auto lock_set = txn->get_lock_set();
    for (auto &lid : *lock_set) {
        lock_manager_->unlock(txn, lid);
    }
    lock_set->clear();
    // 记录abort日志
    if (log_manager != nullptr) {
        auto log_rec = new AbortLogRecord(txn->get_transaction_id());
        log_rec->prev_lsn_ = txn->get_prev_lsn();
        txn->set_prev_lsn(log_manager->add_log_to_buffer(log_rec));
        log_manager->flush_log_to_disk();
    }
    txn->set_state(TransactionState::ABORTED);
}