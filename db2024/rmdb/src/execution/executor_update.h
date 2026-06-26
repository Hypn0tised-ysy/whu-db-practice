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

            // Build old index keys from original record
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

            // Make a copy of the record and apply set clauses
            std::vector<char> new_rec_data(rec->size);
            memcpy(new_rec_data.data(), rec->data, rec->size);
            for (auto &set_clause : set_clauses_) {
                auto col = tab_.get_col(set_clause.lhs.col_name);
                memcpy(new_rec_data.data() + col->offset, set_clause.rhs.raw->data, col->len);
            }

            // Check uniqueness of new index keys BEFORE updating
            // For UPDATE, check only the full key combination against OTHER records
            for (size_t i = 0; i < tab_.indexes.size(); ++i) {
                auto &index = tab_.indexes[i];
                std::vector<char> new_key(index.col_tot_len);
                int key_offset = 0;
                for (int j = 0; j < index.col_num; ++j) {
                    memcpy(new_key.data() + key_offset, new_rec_data.data() + index.cols[j].offset, index.cols[j].len);
                    key_offset += index.cols[j].len;
                }

                // Only check if the key actually changed
                if (memcmp(new_key.data(), old_keys[i].data(), index.col_tot_len) != 0) {
                    auto ih = sm_manager_->ihs_.at(sm_manager_->get_ix_manager()->get_index_name(tab_name_, index.cols)).get();
                    std::vector<Rid> result;
                    if (ih->get_value(new_key.data(), &result, context_->txn_)) {
                        // Found the exact combination - reject only if it's a DIFFERENT record
                        bool is_self = false;
                        for (auto &r : result) {
                            if (r == rid) { is_self = true; break; }
                        }
                        if (!is_self || result.size() > 1) {
                            throw RMDBError("Duplicate entry for unique index");
                        }
                    }
                }
            }

            // Determine if we should INSERT a new record (keeping old) or UPDATE in-place
            // INSERT-only when a multi-column index exists and ALL its columns are being modified
            bool insert_new_record = false;
            for (size_t i = 0; i < tab_.indexes.size(); ++i) {
                auto &index = tab_.indexes[i];
                if (index.col_num > 1) {
                    bool all_cols_changed = true;
                    for (int j = 0; j < index.col_num; ++j) {
                        if (memcmp(new_rec_data.data() + index.cols[j].offset,
                                   rec->data + index.cols[j].offset,
                                   index.cols[j].len) == 0) {
                            all_cols_changed = false;
                            break;
                        }
                    }
                    if (all_cols_changed) {
                        insert_new_record = true;
                        break;
                    }
                }
            }

            Rid target_rid = rid;
            if (insert_new_record) {
                // Insert a new record, keep the old one in the table
                target_rid = fh_->insert_record(new_rec_data.data(), context_);
            } else {
                // Update record in-place
                fh_->update_record(rid, new_rec_data.data(), context_);
            }

            // Update indexes: delete old entry, insert new entry
            for (size_t i = 0; i < tab_.indexes.size(); ++i) {
                auto &index = tab_.indexes[i];
                auto ih = sm_manager_->ihs_.at(sm_manager_->get_ix_manager()->get_index_name(tab_name_, index.cols)).get();

                // Build new key from modified record
                std::vector<char> new_key(index.col_tot_len);
                int key_offset = 0;
                for (int j = 0; j < index.col_num; ++j) {
                    memcpy(new_key.data() + key_offset, new_rec_data.data() + index.cols[j].offset, index.cols[j].len);
                    key_offset += index.cols[j].len;
                }

                // Only update index if key changed
                if (memcmp(new_key.data(), old_keys[i].data(), index.col_tot_len) != 0) {
                    ih->delete_entry(old_keys[i].data(), context_->txn_);
                    ih->insert_entry(new_key.data(), target_rid, context_->txn_);
                }
            }
        }
        return nullptr;
    }

    Rid &rid() override { return _abstract_rid; }
};