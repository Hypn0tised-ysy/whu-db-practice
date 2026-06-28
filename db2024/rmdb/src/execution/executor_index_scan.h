/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2. */

#pragma once

#include <cfloat>
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
    int col_tot_len_;

   public:
    IndexScanExecutor(SmManager *sm_manager, std::string tab_name, std::vector<Condition> conds,
                    std::vector<std::string> index_col_names, Context *context) {
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
        std::string index_name = sm_manager_->get_ix_manager()->get_index_name(tab_name_, index_meta_.cols);
        ih_ = sm_manager_->ihs_.at(index_name).get();
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

    // Write type-correct minimum value for a column
    static void write_col_min(char *dest, ColType type, int len) {
        switch (type) {
            case TYPE_INT: { int32_t v = INT32_MIN; memcpy(dest, &v, len); break; }
            case TYPE_BIGINT: case TYPE_DATETIME: { int64_t v = INT64_MIN; memcpy(dest, &v, len); break; }
            case TYPE_FLOAT: { float v = -FLT_MAX; memcpy(dest, &v, len); break; }
            case TYPE_STRING: memset(dest, 0, len); break;
            default: memset(dest, 0, len); break;
        }
    }

    // Write type-correct maximum value for a column
    static void write_col_max(char *dest, ColType type, int len) {
        switch (type) {
            case TYPE_INT: { int32_t v = INT32_MAX; memcpy(dest, &v, len); break; }
            case TYPE_BIGINT: case TYPE_DATETIME: { int64_t v = INT64_MAX; memcpy(dest, &v, len); break; }
            case TYPE_FLOAT: { float v = FLT_MAX; memcpy(dest, &v, len); break; }
            case TYPE_STRING: memset(dest, 0xFF, len); break;
            default: memset(dest, 0xFF, len); break;
        }
    }

    // Build a key from conditions matching index columns using leftmost prefix rule.
    // - use_lower=true:  build lower bound key (inclusive start of scan range)
    // - use_lower=false: build upper bound key (exclusive end of scan range)
    //
    // Leftmost prefix rule: once we hit a range condition (>, >=, <, <=) on column i,
    // subsequent columns i+1, i+2, ... are filled with type-appropriate MIN/MAX values
    // and conditions on those columns are evaluated by eval_conds as a filter.
    void build_bound_key(std::vector<char> &key_buf, bool use_lower) {
        int offset = 0;
        bool stopped = false;

        for (size_t i = 0; i < index_meta_.cols.size(); i++) {
            auto &idx_col = index_meta_.cols[i];

            if (stopped) {
                // Previous column used a range — fill this and remaining with min/max
                if (use_lower)
                    write_col_min(key_buf.data() + offset, idx_col.type, idx_col.len);
                else
                    write_col_max(key_buf.data() + offset, idx_col.type, idx_col.len);
                offset += idx_col.len;
                continue;
            }

            // Find condition matching this index column
            bool found_eq = false, found_range = false;
            const Value *range_val = nullptr;

            for (auto &cond : fed_conds_) {
                if (!cond.is_rhs_val) continue;
                if (cond.lhs_col.col_name != idx_col.name || cond.lhs_col.tab_name != tab_name_) continue;

                if (cond.op == OP_EQ) {
                    if (cond.rhs_val.raw != nullptr) {
                        memcpy(key_buf.data() + offset, cond.rhs_val.raw->data, idx_col.len);
                        found_eq = true;
                        break;  // EQ takes priority
                    }
                } else if (!found_eq) {
                    // Track range conditions
                    bool applies = false;
                    if (use_lower && (cond.op == OP_GT || cond.op == OP_GE)) applies = true;
                    if (!use_lower && (cond.op == OP_LT || cond.op == OP_LE)) applies = true;
                    if (applies && cond.rhs_val.raw != nullptr) {
                        range_val = &cond.rhs_val;
                        found_range = true;
                    }
                }
            }

            if (found_eq) {
                // EQ on this column: exact value, continue to next column
                offset += idx_col.len;
                // No "stopped" — we continue to next column
            } else if (found_range) {
                // Range condition: use the value, then STOP (remaining cols get min/max)
                memcpy(key_buf.data() + offset, range_val->raw->data, idx_col.len);
                offset += idx_col.len;
                stopped = true;
            } else {
                // No condition on this column: fill with min/max and STOP
                if (use_lower)
                    write_col_min(key_buf.data() + offset, idx_col.type, idx_col.len);
                else
                    write_col_max(key_buf.data() + offset, idx_col.type, idx_col.len);
                offset += idx_col.len;
                stopped = true;
            }
        }
    }

    void beginTuple() override {
        // 加表级S锁，防止幻读（阻止并发插入）
        if (context_ != nullptr && context_->txn_ != nullptr && context_->lock_mgr_ != nullptr) {
            context_->lock_mgr_->lock_shared_on_table(context_->txn_, fh_->GetFd());
        }
        // If no conditions, scan entire index (for ordering)
        if (fed_conds_.empty()) {
            Iid lower = ih_->leaf_begin();
            Iid upper = ih_->leaf_end();
            scan_ = std::make_unique<IxScan>(ih_, lower, upper, sm_manager_->get_bpm());
        } else {
            std::vector<char> lower_key(col_tot_len_);
            std::vector<char> upper_key(col_tot_len_);
            build_bound_key(lower_key, true);
            build_bound_key(upper_key, false);

            Iid lower = ih_->lower_bound(lower_key.data());
            Iid upper = ih_->upper_bound(upper_key.data());
            scan_ = std::make_unique<IxScan>(ih_, lower, upper, sm_manager_->get_bpm());
        }
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
        if (scan_->is_end()) return nullptr;
        // 加记录级S锁
        if (context_ != nullptr && context_->txn_ != nullptr && context_->lock_mgr_ != nullptr) {
            context_->lock_mgr_->lock_shared_on_record(context_->txn_, rid_, fh_->GetFd());
        }
        auto rec = fh_->get_record(rid_, context_);
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
            for (auto &col : rec_cols) {
                if (col.tab_name == cond.lhs_col.tab_name && col.name == cond.lhs_col.col_name) {
                    char *lhs_val = rec->data + col.offset;
                    if (cond.is_rhs_val) {
                        if (!eval_condition(lhs_val, col, cond.op, cond.rhs_val)) return false;
                    } else {
                        for (auto &r_col : rec_cols) {
                            if (r_col.tab_name == cond.rhs_col.tab_name && r_col.name == cond.rhs_col.col_name) {
                                char *rhs_val = rec->data + r_col.offset;
                                if (!eval_binary(lhs_val, rhs_val, cond.op, col.type, col.len)) return false;
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
            int32_t lhs = *(int32_t *)lhs_val; int64_t rhs = rhs_val.bigint_val;
            switch (op) { case OP_EQ: return lhs == rhs; case OP_NE: return lhs != rhs; case OP_LT: return lhs < rhs; case OP_GT: return lhs > rhs; case OP_LE: return lhs <= rhs; case OP_GE: return lhs >= rhs; }
        } else if (col.type == TYPE_BIGINT || col.type == TYPE_DATETIME) {
            int64_t lhs = *(int64_t *)lhs_val; int64_t rhs = rhs_val.bigint_val;
            switch (op) { case OP_EQ: return lhs == rhs; case OP_NE: return lhs != rhs; case OP_LT: return lhs < rhs; case OP_GT: return lhs > rhs; case OP_LE: return lhs <= rhs; case OP_GE: return lhs >= rhs; }
        } else if (col.type == TYPE_FLOAT) {
            float lhs = *(float *)lhs_val; float rhs = rhs_val.float_val;
            switch (op) { case OP_EQ: return lhs == rhs; case OP_NE: return lhs != rhs; case OP_LT: return lhs < rhs; case OP_GT: return lhs > rhs; case OP_LE: return lhs <= rhs; case OP_GE: return lhs >= rhs; }
        } else if (col.type == TYPE_STRING) {
            std::string lhs(lhs_val, col.len); lhs.resize(strlen(lhs.c_str()));
            std::string rhs = rhs_val.str_val;
            switch (op) { case OP_EQ: return lhs == rhs; case OP_NE: return lhs != rhs; case OP_LT: return lhs < rhs; case OP_GT: return lhs > rhs; case OP_LE: return lhs <= rhs; case OP_GE: return lhs >= rhs; }
        }
        return false;
    }

    bool eval_binary(char *lhs_val, char *rhs_val, CompOp op, ColType type, int len) {
        if (type == TYPE_INT) {
            int32_t lhs = *(int32_t *)lhs_val; int32_t rhs = *(int32_t *)rhs_val;
            switch (op) { case OP_EQ: return lhs == rhs; case OP_NE: return lhs != rhs; case OP_LT: return lhs < rhs; case OP_GT: return lhs > rhs; case OP_LE: return lhs <= rhs; case OP_GE: return lhs >= rhs; }
        } else if (type == TYPE_BIGINT || type == TYPE_DATETIME) {
            int64_t lhs = *(int64_t *)lhs_val; int64_t rhs = *(int64_t *)rhs_val;
            switch (op) { case OP_EQ: return lhs == rhs; case OP_NE: return lhs != rhs; case OP_LT: return lhs < rhs; case OP_GT: return lhs > rhs; case OP_LE: return lhs <= rhs; case OP_GE: return lhs >= rhs; }
        } else if (type == TYPE_FLOAT) {
            float lhs = *(float *)lhs_val; float rhs = *(float *)rhs_val;
            switch (op) { case OP_EQ: return lhs == rhs; case OP_NE: return lhs != rhs; case OP_LT: return lhs < rhs; case OP_GT: return lhs > rhs; case OP_LE: return lhs <= rhs; case OP_GE: return lhs >= rhs; }
        } else if (type == TYPE_STRING) {
            std::string lhs(lhs_val, len); lhs.resize(strlen(lhs.c_str()));
            std::string rhs(rhs_val, len); rhs.resize(strlen(rhs.c_str()));
            switch (op) { case OP_EQ: return lhs == rhs; case OP_NE: return lhs != rhs; case OP_LT: return lhs < rhs; case OP_GT: return lhs > rhs; case OP_LE: return lhs <= rhs; case OP_GE: return lhs >= rhs; }
        }
        return false;
    }
};
