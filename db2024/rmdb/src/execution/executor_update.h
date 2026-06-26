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

class UpdateExecutor : public AbstractExecutor {
   private:
    TabMeta tab_;
    std::vector<Condition> conds_;
    RmFileHandle *fh_;
    std::vector<Rid> rids_;
    std::string tab_name_;
    std::vector<SetClause> set_clauses_;
    SmManager *sm_manager_;

   public:
    UpdateExecutor(SmManager *sm_manager, const std::string &tab_name, std::vector<SetClause> set_clauses,
                   std::vector<Condition> conds, std::vector<Rid> rids, Context *context) {
        sm_manager_ = sm_manager;
        tab_name_ = tab_name;
        set_clauses_ = set_clauses;
        tab_ = sm_manager_->db_.get_table(tab_name);
        fh_ = sm_manager_->fhs_.at(tab_name).get();
        conds_ = conds;
        rids_ = rids;
        context_ = context;
    }
    std::unique_ptr<RmRecord> Next() override {
        // Initialize raw buffers for set clauses (only once)
        for (auto &set_clause : set_clauses_) {
            auto col = tab_.get_col(set_clause.lhs.col_name);
            if (set_clause.rhs.raw == nullptr) {
                set_clause.rhs.init_raw(col->len);
            }
        }

        for (auto &rid : rids_) {
            auto rec = fh_->get_record(rid, context_);

            // Build old index keys before modification
            std::vector<std::vector<char>> old_keys;
            for (size_t i = 0; i < tab_.indexes.size(); ++i) {
                auto &index = tab_.indexes[i];
                std::vector<char> key(index.col_tot_len);
                int key_offset = 0;
                for (int j = 0; j < index.col_num; ++j) {
                    memcpy(key.data() + key_offset, rec->data + index.cols[j].offset, index.cols[j].len);
                    key_offset += index.cols[j].len;
                }
                old_keys.push_back(std::move(key));
            }

            // 应用set子句
            for (auto &set_clause : set_clauses_) {
                auto col = tab_.get_col(set_clause.lhs.col_name);
                memcpy(rec->data + col->offset, set_clause.rhs.raw->data, col->len);
            }

            // 更新记录
            fh_->update_record(rid, rec->data, context_);

            // 更新索引: delete old entry, insert new entry
            for (size_t i = 0; i < tab_.indexes.size(); ++i) {
                auto &index = tab_.indexes[i];
                auto ih = sm_manager_->ihs_.at(sm_manager_->get_ix_manager()->get_index_name(tab_name_, index.cols)).get();

                // Build new key from modified record
                char *new_key = new char[index.col_tot_len];
                int key_offset = 0;
                for (int j = 0; j < index.col_num; ++j) {
                    memcpy(new_key + key_offset, rec->data + index.cols[j].offset, index.cols[j].len);
                    key_offset += index.cols[j].len;
                }

                // Delete old index entry using old key
                ih->delete_entry(old_keys[i].data(), context_->txn_);
                // Insert new index entry using new key
                ih->insert_entry(new_key, rid, context_->txn_);
                delete[] new_key;
            }
        }
        return nullptr;
    }

    Rid &rid() override { return _abstract_rid; }
};