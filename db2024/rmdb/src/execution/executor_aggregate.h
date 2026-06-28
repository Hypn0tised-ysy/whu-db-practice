#pragma once

#include "execution_defs.h"
#include "executor_abstract.h"
#include "system/sm.h"

class AggregateExecutor : public AbstractExecutor {
private:
    std::unique_ptr<AbstractExecutor> prev_;
    std::vector<TabCol> sel_cols_;
    std::vector<ColMeta> out_cols_;
    size_t len_;
    bool computed_;
    bool returned_;
    AggType agg_type_;
    ColType agg_col_type_;
    int agg_col_len_;
    int agg_col_offset_;
    bool agg_is_int_;
    int64_t int_result_;
    float float_result_;
    std::string str_result_;
    int count_;

public:
    AggregateExecutor(std::unique_ptr<AbstractExecutor> prev, std::vector<TabCol> sel_cols)
        : prev_(std::move(prev)), sel_cols_(std::move(sel_cols)), computed_(false), returned_(false),
          int_result_(0), float_result_(0.0f), count_(0) {

        // Determine aggregate type from the first col
        agg_type_ = sel_cols_[0].agg_type;

        // Set up output column
        ColMeta out_col;
        out_col.tab_name = "";
        out_col.offset = 0;

        if (agg_type_ == AGG_SUM || agg_type_ == AGG_MAX || agg_type_ == AGG_MIN ||
            agg_type_ == AGG_COUNT || agg_type_ == AGG_COUNT_STAR) {

            // Find the source column type from the child executor
            auto &prev_cols = prev_->cols();
            if (agg_type_ == AGG_COUNT || agg_type_ == AGG_COUNT_STAR) {
                // COUNT always returns INT
                out_col.type = TYPE_INT;
                out_col.len = sizeof(int32_t);
                agg_is_int_ = true;
            } else {
                // SUM/MAX/MIN - find the referenced column
                for (auto &col : prev_cols) {
                    if (col.name == sel_cols_[0].col_name &&
                        (sel_cols_[0].tab_name.empty() || col.tab_name == sel_cols_[0].tab_name)) {
                        agg_col_type_ = col.type;
                        agg_col_len_ = col.len;
                        agg_col_offset_ = col.offset;
                        if (col.type == TYPE_INT) {
                            out_col.type = TYPE_INT;
                            out_col.len = sizeof(int32_t);
                            agg_is_int_ = true;
                        } else if (col.type == TYPE_FLOAT || col.type == TYPE_BIGINT) {
                            out_col.type = TYPE_FLOAT;
                            out_col.len = sizeof(float);
                            agg_is_int_ = false;
                        } else if (col.type == TYPE_STRING) {
                            out_col.type = TYPE_STRING;
                            out_col.len = col.len;
                            agg_is_int_ = false;
                        }
                        break;
                    }
                }
            }
        }

        out_col.name = sel_cols_[0].col_name;
        out_cols_.push_back(out_col);
        len_ = out_col.len;

        // For non-COUNT aggregates, initialize
        if (agg_type_ == AGG_MAX && !agg_is_int_ && agg_col_type_ == TYPE_FLOAT) {
            float_result_ = -1e38f;
        } else if (agg_type_ == AGG_MIN && !agg_is_int_ && agg_col_type_ == TYPE_FLOAT) {
            float_result_ = 1e38f;
        } else if (agg_type_ == AGG_MAX && agg_is_int_ && agg_col_type_ == TYPE_INT) {
            int_result_ = INT64_MIN;
        } else if (agg_type_ == AGG_MIN && agg_is_int_ && agg_col_type_ == TYPE_INT) {
            int_result_ = INT64_MAX;
        } else if (agg_type_ == AGG_MAX && agg_col_type_ == TYPE_STRING) {
            str_result_ = "";
        }
    }

    void beginTuple() override {
        if (computed_) return;

        // Iterate through all records from child executor
        for (prev_->beginTuple(); !prev_->is_end(); prev_->nextTuple()) {
            auto rec = prev_->Next();
            if (rec == nullptr) break;

            if (agg_type_ == AGG_COUNT_STAR) {
                count_++;
            } else if (agg_type_ == AGG_COUNT) {
                count_++;
            } else if (agg_type_ == AGG_SUM) {
                if (agg_col_type_ == TYPE_INT) {
                    int32_t val = *(int32_t*)(rec->data + agg_col_offset_);
                    int_result_ += val;
                } else if (agg_col_type_ == TYPE_FLOAT) {
                    float val = *(float*)(rec->data + agg_col_offset_);
                    float_result_ += val;
                } else if (agg_col_type_ == TYPE_BIGINT) {
                    int64_t val = *(int64_t*)(rec->data + agg_col_offset_);
                    float_result_ += (float)val;
                }
            } else if (agg_type_ == AGG_MAX) {
                if (agg_col_type_ == TYPE_INT) {
                    int32_t val = *(int32_t*)(rec->data + agg_col_offset_);
                    if (val > int_result_) int_result_ = val;
                } else if (agg_col_type_ == TYPE_FLOAT) {
                    float val = *(float*)(rec->data + agg_col_offset_);
                    if (val > float_result_) float_result_ = val;
                } else if (agg_col_type_ == TYPE_STRING) {
                    std::string val((char*)(rec->data + agg_col_offset_), agg_col_len_);
                    val.resize(strlen(val.c_str()));
                    if (val > str_result_) str_result_ = val;
                }
            } else if (agg_type_ == AGG_MIN) {
                if (agg_col_type_ == TYPE_INT) {
                    int32_t val = *(int32_t*)(rec->data + agg_col_offset_);
                    if (val < int_result_) int_result_ = val;
                } else if (agg_col_type_ == TYPE_FLOAT) {
                    float val = *(float*)(rec->data + agg_col_offset_);
                    if (val < float_result_) float_result_ = val;
                } else if (agg_col_type_ == TYPE_STRING) {
                    std::string val((char*)(rec->data + agg_col_offset_), agg_col_len_);
                    val.resize(strlen(val.c_str()));
                    if (str_result_.empty() || val < str_result_) str_result_ = val;
                }
            }
        }
        computed_ = true;
        returned_ = false;
    }

    void nextTuple() override { returned_ = true; }

    bool is_end() const override { return !computed_ || returned_; }

    std::unique_ptr<RmRecord> Next() override {
        if (!computed_ || returned_) return nullptr;

        auto rec = std::make_unique<RmRecord>(len_);
        if (agg_type_ == AGG_COUNT || agg_type_ == AGG_COUNT_STAR) {
            *(int32_t*)(rec->data) = count_;
        } else if (agg_type_ == AGG_SUM) {
            if (agg_is_int_) {
                *(int32_t*)(rec->data) = (int32_t)int_result_;
            } else {
                *(float*)(rec->data) = float_result_;
            }
        } else if (agg_type_ == AGG_MAX || agg_type_ == AGG_MIN) {
            if (agg_col_type_ == TYPE_INT) {
                *(int32_t*)(rec->data) = (int32_t)int_result_;
            } else if (agg_col_type_ == TYPE_FLOAT) {
                *(float*)(rec->data) = float_result_;
            } else if (agg_col_type_ == TYPE_STRING) {
                memset(rec->data, 0, agg_col_len_);
                memcpy(rec->data, str_result_.c_str(), std::min((int)str_result_.size(), agg_col_len_));
            }
        }

        returned_ = true;
        return rec;
    }

    Rid &rid() override { return _abstract_rid; }
    size_t tupleLen() const override { return len_; }
    const std::vector<ColMeta> &cols() const override { return out_cols_; }
};
