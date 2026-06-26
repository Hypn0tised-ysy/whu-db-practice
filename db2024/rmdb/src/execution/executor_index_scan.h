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

class IndexScanExecutor : public AbstractExecutor {
   private:
    std::string tab_name_;
    TabMeta tab_;
    std::vector<Condition> conds_;
    RmFileHandle *fh_;
    std::vector<ColMeta> cols_;
    size_t len_;
    std::vector<Condition> fed_conds_;

    std::vector<std::string> index_col_names_;
    IndexMeta index_meta_;
    IxIndexHandle *ih_;

    Rid rid_;
    std::unique_ptr<RecScan> scan_;

    SmManager *sm_manager_;

    // For building key from conditions
    int col_tot_len_;

   public:
    IndexScanExecutor(SmManager *sm_manager, std::string tab_name, std::vector<Condition> conds, std::vector<std::string> index_col_names,
                    Context *context) {
        sm_manager_ = sm_manager;
        context_ = context;
        tab_name_ = std::move(tab_name);
        tab_ = sm_manager_->db_.get_table(tab_name_);
        conds_ = std::move(conds);
        index_col_names_ = index_col_names;
        index_meta_ = *(tab_.get_index_meta(index_col_names_));
        fh_ = sm_manager_->fhs_.at(tab_name_).get();
        cols_ = tab_.cols;
        len_ = cols_.back().offset + cols_.back().len;
        col_tot_len_ = index_meta_.col_tot_len;

        // Get index handle
        std::string index_name = sm_manager_->get_ix_manager()->get_index_name(tab_name_, index_meta_.cols);
        ih_ = sm_manager_->ihs_.at(index_name).get();

        // Reorder conditions to match index column order
        std::map<CompOp, CompOp> swap_op = {
            {OP_EQ, OP_EQ}, {OP_NE, OP_NE}, {OP_LT, OP_GT}, {OP_GT, OP_LT}, {OP_LE, OP_GE}, {OP_GE, OP_LE},
        };

        for (auto &cond : conds_) {
            if (cond.lhs_col.tab_name != tab_name_) {
                assert(!cond.is_rhs_val && cond.rhs_col.tab_name == tab_name_);
                std::swap(cond.lhs_col, cond.rhs_col);
                cond.op = swap_op.at(cond.op);
            }
        }
        fed_conds_ = conds_;
    }

    // Build a key buffer from conditions for the first N index columns
    void build_key(char *key, bool use_lower) {
        memset(key, 0, col_tot_len_);
        int offset = 0;

        for (size_t i = 0; i < index_meta_.cols.size(); i++) {
            auto &idx_col = index_meta_.cols[i];
            bool found = false;
            for (auto &cond : fed_conds_) {
                if (!cond.is_rhs_val) continue;
                if (cond.lhs_col.col_name == idx_col.name && cond.lhs_col.tab_name == tab_name_) {
                    // Use the already-initialized raw data from the value
                    if (cond.rhs_val.raw != nullptr) {
                        memcpy(key + offset, cond.rhs_val.raw->data, idx_col.len);
                    } else {
                        // Fallback: write directly from value
                        if (cond.rhs_val.type == TYPE_INT) {
                            *(int32_t *)(key + offset) = (int32_t)cond.rhs_val.bigint_val;
                        } else if (cond.rhs_val.type == TYPE_BIGINT || cond.rhs_val.type == TYPE_DATETIME) {
                            *(int64_t *)(key + offset) = cond.rhs_val.bigint_val;
                        } else if (cond.rhs_val.type == TYPE_FLOAT) {
                            *(float *)(key + offset) = cond.rhs_val.float_val;
                        } else if (cond.rhs_val.type == TYPE_STRING) {
                            memset(key + offset, 0, idx_col.len);
                            memcpy(key + offset, cond.rhs_val.str_val.c_str(), std::min((int)cond.rhs_val.str_val.size(), idx_col.len));
                        }
                    }
                    found = true;
                    break;
                }
            }
            if (!found && use_lower) {
                memset(key + offset, 0, idx_col.len);
            } else if (!found && !use_lower) {
                memset(key + offset, 0xFF, idx_col.len);
            }
            offset += idx_col.len;
        }
    }

    void beginTuple() override {
        std::vector<char> lower_key(col_tot_len_);
        std::vector<char> upper_key(col_tot_len_);

        build_key(lower_key.data(), true);   // lower bound
        build_key(upper_key.data(), false);  // upper bound

        Iid lower = ih_->lower_bound(lower_key.data());
        Iid upper = ih_->upper_bound(upper_key.data());

        // For single-point EQ query on full index, upper_bound = lower_bound+1
        // For range queries, lower_bound and upper_bound define the range
        scan_ = std::make_unique<IxScan>(ih_, lower, upper, sm_manager_->get_bpm());
        if (!scan_->is_end()) {
            rid_ = scan_->rid();
        }
    }

    void nextTuple() override {
        if (scan_->is_end()) return;
        scan_->next();
        if (!scan_->is_end()) {
            rid_ = scan_->rid();
        }
    }

    std::unique_ptr<RmRecord> Next() override {
        if (scan_->is_end()) {
            return nullptr;
        }

        auto rec = fh_->get_record(rid_, context_);

        // Check any remaining conditions not covered by the index
        if (eval_conds(fed_conds_, rec, cols_)) {
            return rec;
        } else {
            nextTuple();
            return Next();
        }
    }

    Rid &rid() override { return rid_; }

    size_t tupleLen() const override { return len_; }

    const std::vector<ColMeta> &cols() const override { return cols_; }

    bool is_end() const override { return scan_->is_end(); }

   private:
    bool eval_conds(const std::vector<Condition> &conds, const std::unique_ptr<RmRecord> &rec,
                    const std::vector<ColMeta> &rec_cols) {
        if (conds.empty()) return true;
        for (auto &cond : conds) {
            // Find lhs column
            for (auto &col : rec_cols) {
                if (col.tab_name == cond.lhs_col.tab_name && col.name == cond.lhs_col.col_name) {
                    char *lhs_val = rec->data + col.offset;
                    if (cond.is_rhs_val) {
                        if (!eval_condition(lhs_val, col, cond.op, cond.rhs_val)) {
                            return false;
                        }
                    } else {
                        // Find rhs column
                        for (auto &r_col : rec_cols) {
                            if (r_col.tab_name == cond.rhs_col.tab_name && r_col.name == cond.rhs_col.col_name) {
                                char *rhs_val = rec->data + r_col.offset;
                                if (!eval_binary(lhs_val, rhs_val, cond.op, col.type, col.len)) {
                                    return false;
                                }
                                break;
                            }
                        }
                    }
                    break;
                }
            }
        }
        return true;
    }

    bool eval_condition(char *lhs_val, const ColMeta &col, CompOp op, const Value &rhs_val) {
        if (col.type == TYPE_INT) {
            int32_t lhs = *(int32_t *)lhs_val;
            int64_t rhs = rhs_val.bigint_val;
            switch (op) {
                case OP_EQ: return lhs == rhs;
                case OP_NE: return lhs != rhs;
                case OP_LT: return lhs < rhs;
                case OP_GT: return lhs > rhs;
                case OP_LE: return lhs <= rhs;
                case OP_GE: return lhs >= rhs;
            }
        } else if (col.type == TYPE_BIGINT || col.type == TYPE_DATETIME) {
            int64_t lhs = *(int64_t *)lhs_val;
            int64_t rhs = rhs_val.bigint_val;
            switch (op) {
                case OP_EQ: return lhs == rhs;
                case OP_NE: return lhs != rhs;
                case OP_LT: return lhs < rhs;
                case OP_GT: return lhs > rhs;
                case OP_LE: return lhs <= rhs;
                case OP_GE: return lhs >= rhs;
            }
        } else if (col.type == TYPE_FLOAT) {
            float lhs = *(float *)lhs_val;
            float rhs = rhs_val.float_val;
            switch (op) {
                case OP_EQ: return lhs == rhs;
                case OP_NE: return lhs != rhs;
                case OP_LT: return lhs < rhs;
                case OP_GT: return lhs > rhs;
                case OP_LE: return lhs <= rhs;
                case OP_GE: return lhs >= rhs;
            }
        } else if (col.type == TYPE_STRING) {
            std::string lhs(lhs_val, col.len);
            lhs.resize(strlen(lhs.c_str()));
            std::string rhs = rhs_val.str_val;
            switch (op) {
                case OP_EQ: return lhs == rhs;
                case OP_NE: return lhs != rhs;
                case OP_LT: return lhs < rhs;
                case OP_GT: return lhs > rhs;
                case OP_LE: return lhs <= rhs;
                case OP_GE: return lhs >= rhs;
            }
        }
        return false;
    }

    bool eval_binary(char *lhs_val, char *rhs_val, CompOp op, ColType type, int len) {
        if (type == TYPE_INT) {
            int32_t lhs = *(int32_t *)lhs_val;
            int32_t rhs = *(int32_t *)rhs_val;
            switch (op) {
                case OP_EQ: return lhs == rhs;
                case OP_NE: return lhs != rhs;
                case OP_LT: return lhs < rhs;
                case OP_GT: return lhs > rhs;
                case OP_LE: return lhs <= rhs;
                case OP_GE: return lhs >= rhs;
            }
        } else if (type == TYPE_BIGINT || type == TYPE_DATETIME) {
            int64_t lhs = *(int64_t *)lhs_val;
            int64_t rhs = *(int64_t *)rhs_val;
            switch (op) {
                case OP_EQ: return lhs == rhs;
                case OP_NE: return lhs != rhs;
                case OP_LT: return lhs < rhs;
                case OP_GT: return lhs > rhs;
                case OP_LE: return lhs <= rhs;
                case OP_GE: return lhs >= rhs;
            }
        } else if (type == TYPE_FLOAT) {
            float lhs = *(float *)lhs_val;
            float rhs = *(float *)rhs_val;
            switch (op) {
                case OP_EQ: return lhs == rhs;
                case OP_NE: return lhs != rhs;
                case OP_LT: return lhs < rhs;
                case OP_GT: return lhs > rhs;
                case OP_LE: return lhs <= rhs;
                case OP_GE: return lhs >= rhs;
            }
        } else if (type == TYPE_STRING) {
            std::string lhs(lhs_val, len);
            lhs.resize(strlen(lhs.c_str()));
            std::string rhs(rhs_val, len);
            rhs.resize(strlen(rhs.c_str()));
            switch (op) {
                case OP_EQ: return lhs == rhs;
                case OP_NE: return lhs != rhs;
                case OP_LT: return lhs < rhs;
                case OP_GT: return lhs > rhs;
                case OP_LE: return lhs <= rhs;
                case OP_GE: return lhs >= rhs;
            }
        }
        return false;
    }
};
