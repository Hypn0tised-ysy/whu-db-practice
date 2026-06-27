#pragma once
#include "execution_defs.h"
#include "execution_manager.h"
#include "executor_abstract.h"
#include "index/ix.h"
#include "system/sm.h"
#include <algorithm>
#include <cstring>
#include <vector>

class SortExecutor : public AbstractExecutor {
private:
    std::unique_ptr<AbstractExecutor> prev_;
    std::vector<ColMeta> sort_cols_;
    std::vector<bool> is_desc_;
    size_t len_ = 0;
    std::vector<std::vector<char>> sorted_data_;
    size_t current_idx_ = 0;
    bool is_sorted_ = false;
    std::vector<ColMeta> output_cols_;

    static int cmpcol(const char *a, const char *b, const ColMeta &c) {
        if (c.type == TYPE_INT) {
            int32_t va = *(int32_t*)(a + c.offset), vb = *(int32_t*)(b + c.offset);
            return va < vb ? -1 : va > vb ? 1 : 0;
        }
        if (c.type == TYPE_BIGINT || c.type == TYPE_DATETIME) {
            int64_t va = *(int64_t*)(a + c.offset), vb = *(int64_t*)(b + c.offset);
            return va < vb ? -1 : va > vb ? 1 : 0;
        }
        if (c.type == TYPE_FLOAT) {
            float va = *(float*)(a + c.offset), vb = *(float*)(b + c.offset);
            return va < vb ? -1 : va > vb ? 1 : 0;
        }
        if (c.type == TYPE_STRING) {
            std::string sa(a + c.offset, c.len), sb(b + c.offset, c.len);
            sa.resize(strlen(sa.c_str())); sb.resize(strlen(sb.c_str()));
            return sa < sb ? -1 : sa > sb ? 1 : 0;
        }
        return 0;
    }

public:
    SortExecutor(std::unique_ptr<AbstractExecutor> prev, std::vector<TabCol> sel_cols, std::vector<bool> is_desc)
        : prev_(std::move(prev)), is_desc_(std::move(is_desc)) {
        output_cols_ = prev_->cols();
        len_ = output_cols_.empty() ? 0 : output_cols_.back().offset + output_cols_.back().len;
        for (auto &sc : sel_cols)
            for (auto &c : output_cols_)
                if ((sc.tab_name.empty() || c.tab_name == sc.tab_name) && c.name == sc.col_name)
                    { sort_cols_.push_back(c); break; }
    }

    void beginTuple() override {
        if (is_sorted_) return;
        prev_->beginTuple();
        while (!prev_->is_end()) {
            auto rec = prev_->Next();
            if (!rec) { prev_->nextTuple(); continue; }
            size_t tl = prev_->tupleLen(); if (!tl) tl = len_;
            sorted_data_.emplace_back(tl);
            memcpy(sorted_data_.back().data(), rec->data, tl);
            prev_->nextTuple();
        }
        if (!sort_cols_.empty()) std::sort(sorted_data_.begin(), sorted_data_.end(),
            [this](auto &a, auto &b) {
                for (size_t i = 0; i < sort_cols_.size(); i++) {
                    int c = cmpcol(a.data(), b.data(), sort_cols_[i]);
                    if (c) return is_desc_[i] ? c > 0 : c < 0;
                }
                return false;
            });
        current_idx_ = 0; is_sorted_ = true;
    }

    void nextTuple() override { if (current_idx_ < sorted_data_.size()) current_idx_++; }
    bool is_end() const override { return current_idx_ >= sorted_data_.size(); }
    std::unique_ptr<RmRecord> Next() override {
        if (current_idx_ >= sorted_data_.size()) return nullptr;
        auto r = std::make_unique<RmRecord>(len_);
        memcpy(r->data, sorted_data_[current_idx_].data(), len_);
        return r;
    }
    Rid &rid() override { return _abstract_rid; }
    size_t tupleLen() const override { return len_; }
    const std::vector<ColMeta> &cols() const override { return output_cols_; }
};
