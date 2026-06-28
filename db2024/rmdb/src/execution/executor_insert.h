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

class InsertExecutor : public AbstractExecutor {
   private:
    TabMeta tab_;                   // 表的元数据
    std::vector<Value> values_;     // 需要插入的数据
    RmFileHandle *fh_;              // 表的数据文件句柄
    std::string tab_name_;          // 表名称
    Rid rid_;                       // 插入的位置，由于系统默认插入时不指定位置，因此当前rid_在插入后才赋值
    SmManager *sm_manager_;

   public:
    InsertExecutor(SmManager *sm_manager, const std::string &tab_name, std::vector<Value> values, Context *context) {
        sm_manager_ = sm_manager;
        tab_ = sm_manager_->db_.get_table(tab_name);
        values_ = values;
        tab_name_ = tab_name;
        if (values.size() != tab_.cols.size()) {
            throw InvalidValueCountError();
        }
        fh_ = sm_manager_->fhs_.at(tab_name).get();
        context_ = context;
    };

    std::unique_ptr<RmRecord> Next() override {
        // Make record buffer
        RmRecord rec(fh_->get_file_hdr().record_size);
        for (size_t i = 0; i < values_.size(); i++) {
            auto &col = tab_.cols[i];
            auto &val = values_[i];
            // Allow INT->BIGINT and STRING->DATETIME implicit conversion
            if (col.type != val.type) {
                if (col.type == TYPE_DATETIME && val.type == TYPE_STRING) {
                    val.set_datetime(parse_datetime(val.str_val));
                } else if (col.type == TYPE_BIGINT && val.type == TYPE_INT) {
                    val.type = TYPE_BIGINT;
                } else if (col.type == TYPE_INT && val.type == TYPE_BIGINT) {
                    if (val.bigint_val > INT32_MAX || val.bigint_val < INT32_MIN) {
                        throw RMDBError("BIGINT value out of INT range");
                    }
                    val.type = TYPE_INT;
                } else {
                    throw IncompatibleTypeError(coltype2str(col.type), coltype2str(val.type));
                }
            }
            val.init_raw(col.len);
            memcpy(rec.data + col.offset, val.raw->data, col.len);
        }

        // Check uniqueness constraints BEFORE inserting
        // For composite indexes, check the FULL composite key, not individual columns
        for(size_t idx = 0; idx < tab_.indexes.size(); ++idx) {
            auto& index = tab_.indexes[idx];

            // Build the full composite key from the new record
            std::vector<char> new_key(index.col_tot_len);
            int key_offset = 0;
            for(int j = 0; j < index.col_num; ++j) {
                memcpy(new_key.data() + key_offset, rec.data + index.cols[j].offset, index.cols[j].len);
                key_offset += index.cols[j].len;
            }

            // Scan all existing table records and compare full composite key
            RmScan table_scan(fh_);
            while (!table_scan.is_end()) {
                auto existing_rec = fh_->get_record(table_scan.rid(), context_);
                std::vector<char> existing_key(index.col_tot_len);
                key_offset = 0;
                for(int j = 0; j < index.col_num; ++j) {
                    memcpy(existing_key.data() + key_offset, existing_rec->data + index.cols[j].offset, index.cols[j].len);
                    key_offset += index.cols[j].len;
                }
                if (memcmp(new_key.data(), existing_key.data(), index.col_tot_len) == 0) {
                    throw RMDBError("Duplicate entry for unique index");
                }
                table_scan.next();
            }
        }

        // 加表级IX锁（意向排他锁）
        if (context_ != nullptr && context_->txn_ != nullptr && context_->lock_mgr_ != nullptr) {
            context_->lock_mgr_->lock_IX_on_table(context_->txn_, fh_->GetFd());
        }

        // WAL: 先确定 RID → 写日志 → 再写数据。
        // 1. 确定 RID（不修改任何文件元数据，避免 crash 后元数据与日志不一致）
        bool need_new_page = false;
        if (fh_->get_file_hdr().first_free_page_no == RM_NO_PAGE) {
            // 无空闲页：预测新页号为当前 num_pages，slot 0（新页第一个 slot）
            rid_ = Rid{fh_->get_file_hdr().num_pages, 0};
            need_new_page = true;
        } else {
            // 有空闲页：只读获取空闲 slot 位置
            auto page_handle = fh_->fetch_page_handle(fh_->get_file_hdr().first_free_page_no);
            int slot_no = Bitmap::first_bit(false, page_handle.bitmap, fh_->get_file_hdr().num_records_per_page);
            assert(slot_no < fh_->get_file_hdr().num_records_per_page);
            rid_ = Rid{page_handle.page->get_page_id().page_no, slot_no};
            sm_manager_->get_bpm()->unpin_page(page_handle.page->get_page_id(), false);
        }

        // 2. 写 WAL 日志到缓冲区（必须在数据修改之前）
        if (context_ != nullptr && context_->log_mgr_ != nullptr && context_->txn_ != nullptr) {
            auto log_rec = new InsertLogRecord(context_->txn_->get_transaction_id(), rec, rid_, tab_name_);
            log_rec->prev_lsn_ = context_->txn_->get_prev_lsn();
            context_->txn_->set_prev_lsn(context_->log_mgr_->add_log_to_buffer(log_rec));
        }

        // 3. 写入记录数据到预定位置
        if (need_new_page) {
            // 新建页并插入。此时 WAL 已写入，修改 file_hdr 是安全的。
            // redo 阶段的 page 补齐逻辑保证恢复时缺失页能被重新创建。
            rid_ = fh_->insert_record(rec.data, context_);
        } else {
            fh_->insert_record(rid_, rec.data);
        }

        // 加记录级X锁（写锁）
        if (context_ != nullptr && context_->txn_ != nullptr && context_->lock_mgr_ != nullptr) {
            context_->lock_mgr_->lock_exclusive_on_record(context_->txn_, rid_, fh_->GetFd());
        }

        // 记录写操作，用于事务回滚（保存 record 数据，避免 abort 时回读已删除记录）
        if (context_->txn_ != nullptr) {
            auto wr = new WriteRecord(WType::INSERT_TUPLE, tab_name_, rid_, rec);
            context_->txn_->append_write_record(wr);
        }

        // Insert into indexes
        for(size_t idx = 0; idx < tab_.indexes.size(); ++idx) {
            auto& index = tab_.indexes[idx];
            auto ih = sm_manager_->ihs_.at(sm_manager_->get_ix_manager()->get_index_name(tab_name_, index.cols)).get();
            char* key = new char[index.col_tot_len];
            int key_offset = 0;
            for(int j = 0; j < index.col_num; ++j) {
                memcpy(key + key_offset, rec.data + index.cols[j].offset, index.cols[j].len);
                key_offset += index.cols[j].len;
            }
            ih->insert_entry(key, rid_, context_->txn_);
            delete[] key;
        }
        return nullptr;
    }
    Rid &rid() override { return rid_; }
};