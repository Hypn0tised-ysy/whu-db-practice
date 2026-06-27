#pragma once
#include "execution_defs.h"
#include "execution_manager.h"
#include "executor_abstract.h"
#include "index/ix.h"
#include "system/sm.h"

// Block Nested-Loop Join: buffers a block of left rows, scans right once per block
// Join buffer size: maximum number of left rows to buffer at once
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
    size_t block_idx_ = 0;         // current index within left_block_
    bool left_exhausted_ = false;  // all left rows have been buffered
    bool isend_;

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

    // Read up to JOIN_BUFFER_ROWS left rows into the block buffer
    bool fill_left_block() {
        left_block_.clear();
        size_t left_len = left_->tupleLen();
        while (left_block_.size() < JOIN_BUFFER_ROWS && !left_->is_end()) {
            auto rec = left_->Next();
            if (!rec) { left_->nextTuple(); continue; }
            left_block_.emplace_back(left_len);
            memcpy(left_block_.back().data(), rec->data, left_len);
            left_->nextTuple();
        }
        return !left_block_.empty();
    }

    void beginTuple() override {
        isend_ = false; left_exhausted_ = false; block_idx_ = 0;
        left_->beginTuple();
        if (!fill_left_block()) { isend_ = true; return; }
        right_->beginTuple();

        // Position at first matching pair in the block
        if (!fed_conds_.empty()) {
            while (true) {
                if (right_->is_end()) {
                    // Move to next block of left rows
                    if (!fill_left_block()) { isend_ = true; return; }
                    block_idx_ = 0;
                    right_->beginTuple();
                    if (right_->is_end()) { isend_ = true; return; }
                }
                if (check_current_pair()) break;
                right_->nextTuple();
            }
        }
    }

    void nextTuple() override {
        if (isend_) return;
        right_->nextTuple();

        while (true) {
            if (right_->is_end()) {
                block_idx_++;
                if (block_idx_ >= left_block_.size()) {
                    if (!fill_left_block()) { isend_ = true; return; }
                    block_idx_ = 0;
                }
                right_->beginTuple();
                if (right_->is_end()) { isend_ = true; return; }
            }
            if (fed_conds_.empty() || check_current_pair()) break;
            right_->nextTuple();
        }
    }

    std::unique_ptr<RmRecord> Next() override {
        if (isend_) return nullptr;
        auto right_rec = right_->Next();
        if (!right_rec) return nullptr;
        auto rec = std::make_unique<RmRecord>(len_);
        memcpy(rec->data, left_block_[block_idx_].data(), left_->tupleLen());
        memcpy(rec->data + left_->tupleLen(), right_rec->data, right_->tupleLen());
        return rec;
    }

    Rid &rid() override { return _abstract_rid; }
    size_t tupleLen() const override { return len_; }
    const std::vector<ColMeta> &cols() const override { return cols_; }
    bool is_end() const override { return isend_; }

private:
    bool check_current_pair() {
        auto right_rec = right_->Next();
        if (!right_rec) return false;
        auto &left_cols = left_->cols();
        auto &right_cols = right_->cols();
        std::vector<char> right_data(right_rec->data, right_rec->data + right_->tupleLen());
        const char *left_data = left_block_[block_idx_].data();

        for (auto &cond : fed_conds_) {
            const char *lhs_val = nullptr, *rhs_val = nullptr;
            ColType lhs_type; int lhs_len;

            if (cond.lhs_col.tab_name == left_cols[0].tab_name) {
                for (auto &c : left_cols) { if (c.name == cond.lhs_col.col_name) { lhs_val = left_data + c.offset; lhs_type = c.type; lhs_len = c.len; break; } }
            } else {
                for (auto &c : right_cols) { if (c.name == cond.lhs_col.col_name) { lhs_val = right_data.data() + c.offset; lhs_type = c.type; lhs_len = c.len; break; } }
            }

            if (cond.is_rhs_val) {
                if (!eval_rhs_val(lhs_val, lhs_type, lhs_len, cond.op, cond.rhs_val)) return false;
            } else {
                ColType rhs_type; int rhs_len;
                if (cond.rhs_col.tab_name == left_cols[0].tab_name) {
                    for (auto &c : left_cols) { if (c.name == cond.rhs_col.col_name) { rhs_val = left_data + c.offset; rhs_type = c.type; rhs_len = c.len; break; } }
                } else {
                    for (auto &c : right_cols) { if (c.name == cond.rhs_col.col_name) { rhs_val = right_data.data() + c.offset; rhs_type = c.type; rhs_len = c.len; break; } }
                }
                if (!compare(lhs_val, rhs_val, lhs_type, cond.op)) return false;
            }
        }
        return true;
    }

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

    bool compare(const char *a, const char *b, ColType t, CompOp op) {
        if (t == TYPE_INT) return cmp_i(*(int32_t*)a, *(int32_t*)b, op);
        if (t == TYPE_BIGINT || t == TYPE_DATETIME) return cmp_i(*(int64_t*)a, *(int64_t*)b, op);
        if (t == TYPE_FLOAT) return cmp_f(*(float*)a, *(float*)b, op);
        if (t == TYPE_STRING) {
            std::string sa(a, 256), sb(b, 256);
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
