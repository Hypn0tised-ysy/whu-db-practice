/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#pragma once
#include "execution_defs.h"
#include "execution_manager.h"
#include "executor_abstract.h"
#include "index/ix.h"
#include "system/sm.h"

class DeleteExecutor : public AbstractExecutor {
   private:
    TabMeta tab_;                   // 表的元数据
    std::vector<Condition> conds_;  // delete的条件
    RmFileHandle *fh_;              // 表的数据文件句柄
    std::vector<Rid> rids_;         // 需要删除的记录的位置
    std::string tab_name_;          // 表名称
    SmManager *sm_manager_;

   public:
    DeleteExecutor(SmManager *sm_manager, const std::string &tab_name, std::vector<Condition> conds,
                   std::vector<Rid> rids, Context *context) {
        sm_manager_ = sm_manager;
        tab_name_ = tab_name;
        tab_ = sm_manager_->db_.get_table(tab_name);
        fh_ = sm_manager_->fhs_.at(tab_name).get();
        conds_ = conds;
        rids_ = rids;
        context_ = context;
    }

    std::unique_ptr<RmRecord> Next() override {
        // 加表级IX锁（意向排他锁）
        if (context_ != nullptr && context_->txn_ != nullptr && context_->lock_mgr_ != nullptr) {
            context_->lock_mgr_->lock_IX_on_table(context_->txn_, fh_->GetFd());
        }
        for (auto &rid : rids_) {
            // 加记录级X锁
            if (context_ != nullptr && context_->txn_ != nullptr && context_->lock_mgr_ != nullptr) {
                context_->lock_mgr_->lock_exclusive_on_record(context_->txn_, rid, fh_->GetFd());
            }

            auto rec = fh_->get_record(rid, context_);

            // 记录日志（WAL），用于故障恢复
            if (context_ != nullptr && context_->log_mgr_ != nullptr && context_->txn_ != nullptr) {
                auto log_rec = new DeleteLogRecord(context_->txn_->get_transaction_id(), *rec, rid, tab_name_);
                log_rec->prev_lsn_ = context_->txn_->get_prev_lsn();
                context_->txn_->set_prev_lsn(context_->log_mgr_->add_log_to_buffer(log_rec));
            }

            // 记录写操作（保存旧值，用于事务回滚）
            if (context_->txn_ != nullptr) {
                auto wr = new WriteRecord(WType::DELETE_TUPLE, tab_name_, rid, *rec);
                context_->txn_->append_write_record(wr);
            }

            // 删除索引
            for (size_t i = 0; i < tab_.indexes.size(); ++i) {
                auto &index = tab_.indexes[i];
                auto ih = sm_manager_->ihs_.at(sm_manager_->get_ix_manager()->get_index_name(tab_name_, index.cols)).get();
                char *key = new char[index.col_tot_len];
                int offset = 0;
                for (size_t j = 0; j < index.col_num; ++j) {
                    memcpy(key + offset, rec->data + index.cols[j].offset, index.cols[j].len);
                    offset += index.cols[j].len;
                }
                ih->delete_entry(key, context_->txn_);
                delete[] key;
            }

            // 删除记录
            fh_->delete_record(rid, context_);
        }
        return nullptr;
    }

    Rid &rid() override { return _abstract_rid; }
};