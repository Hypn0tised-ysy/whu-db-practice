#pragma once
#include "execution_defs.h"
#include "execution_manager.h"
#include "executor_abstract.h"
#include "index/ix.h"
#include "system/sm.h"

// Block Nested-Loop Join: buffers a block of left rows, scans right once per block.
// For each right row, check against ALL left rows in the current block.
// Join buffer size: maximum number of left rows to buffer at once.
static const size_t JOIN_BUFFER_ROWS = 500;

class NestedLoopJoinExecutor : public AbstractExecutor {
private:
    std::unique_ptr<AbstractExecutor> left_;
    std::unique_ptr<AbstractExecutor> right_;
    size_t len_;
    std::vector<ColMeta> cols_;
    std::vector<Condition> fed_conds_;

    // Block buffer: stores left row data for current block
    std::vector<std::vector<char>> left_block_;
    size_t block_idx_ = 0;         // current left row index within left_block_
    bool left_exhausted_ = false;  // all left rows have been consumed
    bool isend_;

    // Cached right record — avoids double-consuming right_->Next()
    // (old code: check_current_pair consumed one record, Next() consumed another)
    std::vector<char> cur_right_data_;
    bool has_cur_right_ = false;

public:
    NestedLoopJoinExecutor(std::unique_ptr<AbstractExecutor> left, std::unique_ptr<AbstractExecutor> right,
                            std::vector<Condition> conds) {
        left_ = std::move(left);
        right_ = std::move(right);
        len_ = left_->tupleLen() + right_->tupleLen();
        cols_ = left_->cols();
        auto right_cols = right_->cols();
        for (auto &col : right_cols) col.offset += left_->tupleLen();
        cols_.insert(cols_.end(), right_cols.begin(), right_cols.end());
        isend_ = false;
        fed_conds_ = std::move(conds);
    }

    // Read up to JOIN_BUFFER_ROWS left rows into the block buffer.
    // Returns true if at least one row was loaded.
    bool fill_left_block() {
        if (left_exhausted_) return false;
        left_block_.clear();
        size_t left_len = left_->tupleLen();
        while (left_block_.size() < JOIN_BUFFER_ROWS && !left_->is_end()) {
            auto rec = left_->Next();
            if (!rec) { left_->nextTuple(); continue; }
            left_block_.emplace_back(left_len);
            memcpy(left_block_.back().data(), rec->data, left_len);
            left_->nextTuple();
        }
        if (left_block_.empty()) {
            left_exhausted_ = true;
            return false;
        }
        return true;
    }

    void beginTuple() override {
        isend_ = false;
        left_exhausted_ = false;
        block_idx_ = 0;
        has_cur_right_ = false;
        cur_right_data_.clear();

        left_->beginTuple();
        if (!fill_left_block()) { isend_ = true; return; }
        right_->beginTuple();

        // Position at first matching pair
        advance_to_next_match();
    }

    void nextTuple() override {
        if (isend_) return;
        advance_to_next_match();
    }

    std::unique_ptr<RmRecord> Next() override {
        if (isend_ || !has_cur_right_) return nullptr;
        auto rec = std::make_unique<RmRecord>(len_);
        memcpy(rec->data, left_block_[block_idx_].data(), left_->tupleLen());
        memcpy(rec->data + left_->tupleLen(), cur_right_data_.data(), right_->tupleLen());
        return rec;
    }

    Rid &rid() override { return _abstract_rid; }
    size_t tupleLen() const override { return len_; }
    const std::vector<ColMeta> &cols() const override { return cols_; }
    bool is_end() const override { return isend_; }

private:
    // Find a column in a column vector by TabCol (matches both table name and column name)
    static const ColMeta* find_col(const std::vector<ColMeta> &cols, const TabCol &target) {
        for (auto &c : cols) {
            if (c.tab_name == target.tab_name && c.name == target.col_name) {
                return &c;
            }
        }
        return nullptr;
    }

    // Check whether left_block_[left_idx] and the cached right record satisfy all join conditions.
    bool check_left_block_row_with_right(size_t left_idx) {
        auto &left_cols = left_->cols();
        auto &right_cols = right_->cols();
        const char *left_data = left_block_[left_idx].data();
        const char *right_data = cur_right_data_.data();

        for (auto &cond : fed_conds_) {
            // Resolve LHS column: search BOTH left and right column sets
            const char *lhs_val = nullptr;
            ColType lhs_type = TYPE_INT;
            int lhs_len = 0;

            if (auto *fc = find_col(left_cols, cond.lhs_col)) {
                lhs_val = left_data + fc->offset;
                lhs_type = fc->type;
                lhs_len = fc->len;
            } else if (auto *fc = find_col(right_cols, cond.lhs_col)) {
                lhs_val = right_data + fc->offset;
                lhs_type = fc->type;
                lhs_len = fc->len;
            }

            // Defensive: column not found in either input
            if (lhs_val == nullptr) return false;

            if (cond.is_rhs_val) {
                if (!eval_rhs_val(lhs_val, lhs_type, lhs_len, cond.op, cond.rhs_val)) return false;
            } else {
                // Resolve RHS column: search BOTH left and right column sets
                const char *rhs_val = nullptr;
                if (auto *fc = find_col(left_cols, cond.rhs_col)) {
                    rhs_val = left_data + fc->offset;
                } else if (auto *fc = find_col(right_cols, cond.rhs_col)) {
                    rhs_val = right_data + fc->offset;
                }
                if (rhs_val == nullptr) return false;
                if (!compare(lhs_val, rhs_val, lhs_type, cond.op, lhs_len)) return false;
            }
        }
        return true;
    }

    // Advance to the next matching (left, right) pair.
    // For each right row (one scan per block), checks ALL left rows in the block.
    // This is true BNLJ: right is scanned once per block, not once per left row.
    bool advance_to_next_match() {
        while (true) {
            // First, try remaining left rows against the current cached right record
            if (has_cur_right_) {
                for (size_t i = block_idx_ + 1; i < left_block_.size(); i++) {
                    if (fed_conds_.empty() || check_left_block_row_with_right(i)) {
                        block_idx_ = i;
                        return true;
                    }
                }
                // No more matches for this right record
                has_cur_right_ = false;
                right_->nextTuple();
            }

            // Need next right record
            if (right_->is_end()) {
                // Right exhausted for current block, load next block of left rows
                if (!fill_left_block()) { isend_ = true; return false; }
                block_idx_ = 0;
                right_->beginTuple();
                if (right_->is_end()) { isend_ = true; return false; }
            }

            // Get next right record and cache it
            auto right_rec = right_->Next();
            if (!right_rec) {
                right_->nextTuple();
                continue;
            }
            cur_right_data_.assign(right_rec->data, right_rec->data + right_->tupleLen());
            has_cur_right_ = true;

            // Check ALL left rows in block against this right record
            for (size_t i = 0; i < left_block_.size(); i++) {
                if (fed_conds_.empty() || check_left_block_row_with_right(i)) {
                    block_idx_ = i;
                    return true;
                }
            }

            // No match for this right record, continue to next
            has_cur_right_ = false;
            right_->nextTuple();
        }
    }

    // --- Comparison helpers (unchanged from original) ---

    bool eval_rhs_val(const char *lhs, ColType type, int len, CompOp op, const Value &rhs) {
        if (type == TYPE_INT) { int32_t v = *(int32_t*)lhs; return cmp_i(v, rhs.bigint_val, op); }
        if (type == TYPE_BIGINT || type == TYPE_DATETIME) { int64_t v = *(int64_t*)lhs; return cmp_i(v, rhs.bigint_val, op); }
        if (type == TYPE_FLOAT) { float v = *(float*)lhs; return cmp_f(v, rhs.float_val, op); }
        if (type == TYPE_STRING) {
            std::string s(lhs, len); s.resize(strlen(s.c_str())); std::string r = rhs.str_val;
            return cmp_s(s, r, op);
        }
        return false;
    }

    bool compare(const char *a, const char *b, ColType t, CompOp op, int len) {
        if (t == TYPE_INT) return cmp_i(*(int32_t*)a, *(int32_t*)b, op);
        if (t == TYPE_BIGINT || t == TYPE_DATETIME) return cmp_i(*(int64_t*)a, *(int64_t*)b, op);
        if (t == TYPE_FLOAT) return cmp_f(*(float*)a, *(float*)b, op);
        if (t == TYPE_STRING) {
            std::string sa(a, len), sb(b, len);
            sa.resize(strlen(sa.c_str())); sb.resize(strlen(sb.c_str()));
            return cmp_s(sa, sb, op);
        }
        return false;
    }

    bool cmp_i(int64_t a, int64_t b, CompOp op) {
        switch (op) { case OP_EQ: return a==b; case OP_NE: return a!=b; case OP_LT: return a<b; case OP_GT: return a>b; case OP_LE: return a<=b; case OP_GE: return a>=b; }
        return false;
    }
    bool cmp_f(float a, float b, CompOp op) {
        switch (op) { case OP_EQ: return a==b; case OP_NE: return a!=b; case OP_LT: return a<b; case OP_GT: return a>b; case OP_LE: return a<=b; case OP_GE: return a>=b; }
        return false;
    }
    bool cmp_s(const std::string &a, const std::string &b, CompOp op) {
        switch (op) { case OP_EQ: return a==b; case OP_NE: return a!=b; case OP_LT: return a<b; case OP_GT: return a>b; case OP_LE: return a<=b; case OP_GE: return a>=b; }
        return false;
    }
};
