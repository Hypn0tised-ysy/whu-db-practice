#pragma once
#include "execution_defs.h"
#include "execution_manager.h"
#include "executor_abstract.h"
#include "index/ix.h"
#include "system/sm.h"

// Block Nested-Loop Join:
//   1. Load a block of left rows (up to JOIN_BUFFER_ROWS).
//   2. Scan the entire right table ONCE per block.
//   3. Check each right row against ALL left rows in the block.
//   4. Store matches in per-left-row buffers.
//   5. Emit in LEFT-PRIMARY order (L1,R1),(L1,R2),...,(L2,R1),...
//
// Complexity: ceil(N / B) right scans (B = JOIN_BUFFER_ROWS).
// Memory: bounded — at most B * K right-row copies where K is the
//         number of matches per left row within ONE block.

static const size_t JOIN_BUFFER_ROWS = 500;

class NestedLoopJoinExecutor : public AbstractExecutor {
private:
    std::unique_ptr<AbstractExecutor> left_;
    std::unique_ptr<AbstractExecutor> right_;
    size_t len_;
    std::vector<ColMeta> cols_;
    std::vector<Condition> fed_conds_;

    // --- Block state ---
    std::vector<std::vector<char>> left_block_;

    // Per-left-row match buffers for the current block.
    // per_left_matches_[i] = list of right-row data matching left_block_[i].
    std::vector<std::vector<std::vector<char>>> per_left_matches_;

    // Cursor into per_left_matches_
    size_t cur_left_idx_ = 0;
    size_t cur_match_idx_ = 0;
    std::vector<char> cur_right_data_;   // current right row to output

    bool left_exhausted_ = false;
    bool isend_ = false;

    // --- Block helpers ---

    // Fill left_block_ with up to JOIN_BUFFER_ROWS left rows.
    // Returns false if no more left rows exist.
    // IMPORTANT: never calls nextTuple() after Next() returns nullptr —
    //   that would trigger RmScan::next()'s assert(!is_end()).
    bool fill_left_block() {
        if (left_exhausted_) return false;
        left_block_.clear();
        size_t left_len = left_->tupleLen();
        while (left_block_.size() < JOIN_BUFFER_ROWS && !left_->is_end()) {
            auto rec = left_->Next();
            if (!rec) break;   // scan exhausted (all remaining rows filtered out)
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

    // Scan right table ONCE for the current block.
    // For each right row, check against ALL left rows in the block.
    // Stores matching right-row data in per_left_matches_.
    void build_block_matches() {
        per_left_matches_.clear();
        per_left_matches_.resize(left_block_.size());

        right_->beginTuple();
        size_t right_len = right_->tupleLen();
        while (!right_->is_end()) {
            auto right_rec = right_->Next();
            if (!right_rec) break;   // scan exhausted

            const char *right_ptr = right_rec->data;
            for (size_t i = 0; i < left_block_.size(); i++) {
                if (fed_conds_.empty() ||
                    check_left_row_with_right_data(i, right_rec)) {
                    per_left_matches_[i].emplace_back(right_ptr,
                                                      right_ptr + right_len);
                }
            }
            right_->nextTuple();
        }
    }

    // Advance the cursor to the next match.
    // If the current block is exhausted, loads the next block.
    // Sets isend_ = true when no more output exists.
    void advance_to_next_output() {
        while (true) {
            // Search within the current block's matches (left-primary order)
            while (cur_left_idx_ < per_left_matches_.size()) {
                if (cur_match_idx_ < per_left_matches_[cur_left_idx_].size()) {
                    cur_right_data_ = std::move(
                        per_left_matches_[cur_left_idx_][cur_match_idx_]);
                    cur_match_idx_++;
                    return;
                }
                cur_left_idx_++;
                cur_match_idx_ = 0;
            }

            // Current block exhausted — load next
            if (!fill_left_block()) {
                isend_ = true;
                cur_right_data_.clear();
                return;
            }
            build_block_matches();
            cur_left_idx_ = 0;
            cur_match_idx_ = 0;
            // Loop to search in the new block
        }
    }

public:
    NestedLoopJoinExecutor(std::unique_ptr<AbstractExecutor> left,
                           std::unique_ptr<AbstractExecutor> right,
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

    void beginTuple() override {
        isend_ = false;
        left_exhausted_ = false;
        cur_left_idx_ = 0;
        cur_match_idx_ = 0;
        cur_right_data_.clear();
        per_left_matches_.clear();
        left_block_.clear();

        left_->beginTuple();
        if (!fill_left_block()) { isend_ = true; return; }
        build_block_matches();
        advance_to_next_output();
    }

    void nextTuple() override {
        if (isend_) return;
        advance_to_next_output();
    }

    std::unique_ptr<RmRecord> Next() override {
        if (isend_ || cur_right_data_.empty()) return nullptr;
        auto rec = std::make_unique<RmRecord>(len_);
        memcpy(rec->data, left_block_[cur_left_idx_].data(),
               left_->tupleLen());
        memcpy(rec->data + left_->tupleLen(), cur_right_data_.data(),
               right_->tupleLen());
        return rec;
    }

    Rid &rid() override { return _abstract_rid; }
    size_t tupleLen() const override { return len_; }
    const std::vector<ColMeta> &cols() const override { return cols_; }
    bool is_end() const override { return isend_; }

private:
    // --- Column lookup and type-safe comparison ---

    static const ColMeta* find_col(const std::vector<ColMeta> &cols,
                                   const TabCol &target) {
        for (auto &c : cols) {
            if (c.tab_name == target.tab_name && c.name == target.col_name) {
                return &c;
            }
        }
        return nullptr;
    }

    // Check left_block_[left_idx] against a right RmRecord.
    bool check_left_row_with_right_data(
            size_t left_idx, const std::unique_ptr<RmRecord> &right_rec) {
        auto &left_cols = left_->cols();
        auto &right_cols = right_->cols();
        const char *left_ptr = left_block_[left_idx].data();
        const char *right_ptr = right_rec->data;

        for (auto &cond : fed_conds_) {
            const ColMeta *lhs_col = find_col(left_cols, cond.lhs_col);
            const char *lhs_val = nullptr;
            if (lhs_col) {
                lhs_val = left_ptr + lhs_col->offset;
            } else {
                lhs_col = find_col(right_cols, cond.lhs_col);
                if (lhs_col) lhs_val = right_ptr + lhs_col->offset;
            }
            if (!lhs_col || !lhs_val) return false;

            if (cond.is_rhs_val) {
                if (!eval_rhs_val(lhs_val, lhs_col->type, lhs_col->len,
                                  cond.op, cond.rhs_val))
                    return false;
            } else {
                const ColMeta *rhs_col = find_col(left_cols, cond.rhs_col);
                const char *rhs_val = nullptr;
                if (rhs_col) {
                    rhs_val = left_ptr + rhs_col->offset;
                } else {
                    rhs_col = find_col(right_cols, cond.rhs_col);
                    if (rhs_col) rhs_val = right_ptr + rhs_col->offset;
                }
                if (!rhs_col || !rhs_val) return false;
                if (!compare_typed(lhs_val, *lhs_col, rhs_val, *rhs_col,
                                   cond.op))
                    return false;
            }
        }
        return true;
    }

    // --- Type conversion helpers ---

    static int64_t col_to_int64(const char *data, ColType type) {
        if (type == TYPE_INT) return static_cast<int64_t>(*(int32_t*)data);
        return *(int64_t*)data;
    }

    static float col_to_float(const char *data, ColType type) {
        if (type == TYPE_INT) return static_cast<float>(*(int32_t*)data);
        if (type == TYPE_BIGINT || type == TYPE_DATETIME)
            return static_cast<float>(*(int64_t*)data);
        return *(float*)data;
    }

    bool compare_typed(const char *a, const ColMeta &ca,
                       const char *b, const ColMeta &cb, CompOp op) {
        if (ca.type == TYPE_STRING || cb.type == TYPE_STRING) {
            std::string sa(a, ca.len), sb(b, cb.len);
            sa.resize(strlen(sa.c_str()));
            sb.resize(strlen(sb.c_str()));
            return cmp_s(sa, sb, op);
        }
        if (ca.type == TYPE_FLOAT || cb.type == TYPE_FLOAT) {
            return cmp_f(col_to_float(a, ca.type), col_to_float(b, cb.type),
                         op);
        }
        return cmp_i(col_to_int64(a, ca.type), col_to_int64(b, cb.type), op);
    }

    // --- Scalar comparison helpers ---

    bool eval_rhs_val(const char *lhs, ColType type, int len,
                      CompOp op, const Value &rhs) {
        if (type == TYPE_INT) {
            int32_t v = *(int32_t*)lhs;
            return cmp_i(v, rhs.bigint_val, op);
        }
        if (type == TYPE_BIGINT || type == TYPE_DATETIME) {
            int64_t v = *(int64_t*)lhs;
            return cmp_i(v, rhs.bigint_val, op);
        }
        if (type == TYPE_FLOAT) {
            float v = *(float*)lhs;
            return cmp_f(v, rhs.float_val, op);
        }
        if (type == TYPE_STRING) {
            std::string s(lhs, len);
            s.resize(strlen(s.c_str()));
            std::string r = rhs.str_val;
            return cmp_s(s, r, op);
        }
        return false;
    }

    bool cmp_i(int64_t a, int64_t b, CompOp op) {
        switch (op) {
            case OP_EQ: return a == b;
            case OP_NE: return a != b;
            case OP_LT: return a < b;
            case OP_GT: return a > b;
            case OP_LE: return a <= b;
            case OP_GE: return a >= b;
        }
        return false;
    }
    bool cmp_f(float a, float b, CompOp op) {
        switch (op) {
            case OP_EQ: return a == b;
            case OP_NE: return a != b;
            case OP_LT: return a < b;
            case OP_GT: return a > b;
            case OP_LE: return a <= b;
            case OP_GE: return a >= b;
        }
        return false;
    }
    bool cmp_s(const std::string &a, const std::string &b, CompOp op) {
        switch (op) {
            case OP_EQ: return a == b;
            case OP_NE: return a != b;
            case OP_LT: return a < b;
            case OP_GT: return a > b;
            case OP_LE: return a <= b;
            case OP_GE: return a >= b;
        }
        return false;
    }
};
