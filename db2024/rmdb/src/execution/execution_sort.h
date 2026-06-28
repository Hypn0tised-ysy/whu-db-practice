#pragma once
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <unistd.h>
#include <vector>

#include "execution_defs.h"
#include "execution_manager.h"
#include "executor_abstract.h"
#include "index/ix.h"
#include "system/sm.h"

class SortExecutor : public AbstractExecutor {
private:
    std::unique_ptr<AbstractExecutor> prev_;
    std::vector<ColMeta> sort_cols_;
    std::vector<bool> is_desc_;
    size_t len_ = 0;
    std::vector<ColMeta> output_cols_;

    // In-memory sort state (used when data fits in memory)
    std::vector<std::vector<char>> sorted_data_;
    size_t current_idx_ = 0;
    bool is_sorted_ = false;

    // External sort state (used when data exceeds SORT_CHUNK_ROWS)
    bool is_external_ = false;
    std::vector<std::string> run_files_;        // paths to sorted run files
    std::vector<std::ifstream> run_streams_;     // open streams for merge (indexed by run_idx)
    std::vector<std::vector<char>> run_heads_;   // current head row from each run
    std::vector<size_t> heap_;                   // min-heap of run indices
    std::vector<char> cur_output_row_;           // current output row during streaming merge
    int instance_id_;                            // unique per-instance ID for temp file naming

    inline static std::atomic<int> next_instance_id_{0};   // global counter for instance IDs
    static const size_t SORT_CHUNK_ROWS = 50000;

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

    // Compare two rows by sort_cols_. Returns true if row a < row b.
    bool row_less(const std::vector<char> &a, const std::vector<char> &b) const {
        for (size_t i = 0; i < sort_cols_.size(); i++) {
            int c = cmpcol(a.data(), b.data(), sort_cols_[i]);
            if (c) return is_desc_[i] ? c > 0 : c < 0;
        }
        return false;
    }

    // Write current sorted_data_ chunk to a temporary run file.
    // Use pid + instance_id + run_id to avoid collisions across queries/clients/crashes.
    std::string write_sorted_run(int run_id) {
        std::string path = "sort_" + std::to_string(getpid()) + "_"
                         + std::to_string(instance_id_) + "_"
                         + std::to_string(run_id) + ".tmp";
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        uint64_t nrows = sorted_data_.size();
        out.write(reinterpret_cast<const char*>(&nrows), sizeof(nrows));
        for (auto &row : sorted_data_) {
            out.write(row.data(), len_);
        }
        out.close();
        return path;
    }

    // Open a run file and read its first row into run_heads_[run_idx].
    bool open_run(size_t run_idx, const std::string &path) {
        run_streams_[run_idx].open(path, std::ios::binary);
        if (!run_streams_[run_idx].is_open()) return false;
        // Skip the nrows header (uint64_t)
        run_streams_[run_idx].seekg(sizeof(uint64_t));
        run_heads_[run_idx].resize(len_);
        run_streams_[run_idx].read(run_heads_[run_idx].data(), len_);
        return run_streams_[run_idx].gcount() == (std::streamsize)len_;
    }

    // Read next row from run run_idx into run_heads_[run_idx]. Returns false if exhausted.
    bool advance_run(size_t run_idx) {
        run_heads_[run_idx].resize(len_);
        run_streams_[run_idx].read(run_heads_[run_idx].data(), len_);
        return run_streams_[run_idx].gcount() == (std::streamsize)len_;
    }

    // --- Manual min-heap (smallest row at heap_[0]) ---
    // Compare heap elements: true if run a's head > run b's head

    bool heap_gt(size_t a, size_t b) const {
        return row_less(run_heads_[b], run_heads_[a]);
    }

    void heap_sift_up(size_t idx) {
        while (idx > 0) {
            size_t parent = (idx - 1) / 2;
            if (!heap_gt(heap_[parent], heap_[idx])) break;
            std::swap(heap_[parent], heap_[idx]);
            idx = parent;
        }
    }

    void heap_sift_down(size_t idx) {
        size_t n = heap_.size();
        while (true) {
            size_t smallest = idx;
            size_t left = 2 * idx + 1;
            size_t right = 2 * idx + 2;
            if (left < n && heap_gt(heap_[smallest], heap_[left])) smallest = left;
            if (right < n && heap_gt(heap_[smallest], heap_[right])) smallest = right;
            if (smallest == idx) break;
            std::swap(heap_[idx], heap_[smallest]);
            idx = smallest;
        }
    }

    void heap_push(size_t run_idx) {
        heap_.push_back(run_idx);
        heap_sift_up(heap_.size() - 1);
    }

    void heap_pop() {
        heap_[0] = heap_.back();
        heap_.pop_back();
        if (!heap_.empty()) heap_sift_down(0);
    }

public:
    SortExecutor(std::unique_ptr<AbstractExecutor> prev, std::vector<TabCol> sel_cols, std::vector<bool> is_desc)
        : prev_(std::move(prev)), is_desc_(std::move(is_desc)),
          instance_id_(next_instance_id_.fetch_add(1)) {
        output_cols_ = prev_->cols();
        len_ = output_cols_.empty() ? 0 : output_cols_.back().offset + output_cols_.back().len;
        for (auto &sc : sel_cols)
            for (auto &c : output_cols_)
                if ((sc.tab_name.empty() || c.tab_name == sc.tab_name) && c.name == sc.col_name)
                    { sort_cols_.push_back(c); break; }
    }

    ~SortExecutor() {
        for (auto &s : run_streams_) { if (s.is_open()) s.close(); }
        for (auto &path : run_files_) { std::remove(path.c_str()); }
    }

    void beginTuple() override {
        if (is_sorted_) return;

        prev_->beginTuple();

        // Phase 1: Read all rows from child, writing sorted chunks to temp files
        int run_id = 0;
        sorted_data_.clear();

        while (!prev_->is_end()) {
            auto rec = prev_->Next();
            if (!rec) { prev_->nextTuple(); continue; }
            size_t tl = prev_->tupleLen(); if (!tl) tl = len_;
            sorted_data_.emplace_back(tl);
            memcpy(sorted_data_.back().data(), rec->data, tl);
            prev_->nextTuple();

            if (sorted_data_.size() >= SORT_CHUNK_ROWS) {
                if (!sort_cols_.empty()) {
                    std::sort(sorted_data_.begin(), sorted_data_.end(),
                        [this](auto &a, auto &b) { return row_less(a, b); });
                }
                run_files_.push_back(write_sorted_run(run_id++));
                sorted_data_.clear();
            }
        }

        // Handle remaining rows in last chunk
        if (!sorted_data_.empty()) {
            if (!sort_cols_.empty()) {
                std::sort(sorted_data_.begin(), sorted_data_.end(),
                    [this](auto &a, auto &b) { return row_less(a, b); });
            }
            if (run_files_.empty()) {
                // All data fits in memory
                current_idx_ = 0;
                is_sorted_ = true;
                is_external_ = false;
                return;
            }
            run_files_.push_back(write_sorted_run(run_id++));
            sorted_data_.clear();
        } else if (run_files_.empty()) {
            // No data at all
            current_idx_ = 0;
            is_sorted_ = true;
            is_external_ = false;
            return;
        }

        // Phase 2: Streaming k-way merge
        is_external_ = true;
        is_sorted_ = true;
        size_t num_runs = run_files_.size();
        run_streams_.resize(num_runs);
        run_heads_.resize(num_runs);
        heap_.clear();

        for (size_t i = 0; i < num_runs; i++) {
            if (open_run(i, run_files_[i])) {
                heap_push(i);
            }
        }

        // Extract first output row
        if (!heap_.empty()) {
            size_t top = heap_[0];
            cur_output_row_ = run_heads_[top];
        }
    }

    void nextTuple() override {
        if (!is_external_) {
            if (current_idx_ < sorted_data_.size()) current_idx_++;
            return;
        }
        // Streaming merge: advance to next smallest row
        if (heap_.empty()) { cur_output_row_.clear(); return; }

        // Advance the run that produced the current output row
        size_t top = heap_[0];
        heap_pop();
        if (advance_run(top)) {
            heap_push(top);
        }

        // Next smallest is at heap top
        if (!heap_.empty()) {
            cur_output_row_ = run_heads_[heap_[0]];
        } else {
            cur_output_row_.clear();
        }
    }

    bool is_end() const override {
        if (!is_external_) return current_idx_ >= sorted_data_.size();
        return cur_output_row_.empty();
    }

    std::unique_ptr<RmRecord> Next() override {
        if (!is_external_) {
            if (current_idx_ >= sorted_data_.size()) return nullptr;
            auto r = std::make_unique<RmRecord>(len_);
            memcpy(r->data, sorted_data_[current_idx_].data(), len_);
            return r;
        }
        if (cur_output_row_.empty()) return nullptr;
        auto r = std::make_unique<RmRecord>(len_);
        memcpy(r->data, cur_output_row_.data(), len_);
        return r;
    }

    Rid &rid() override { return _abstract_rid; }
    size_t tupleLen() const override { return len_; }
    const std::vector<ColMeta> &cols() const override { return output_cols_; }
};
