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

class NestedLoopJoinExecutor : public AbstractExecutor {
   private:
    std::unique_ptr<AbstractExecutor> left_;    // 左儿子节点（需要join的表）
    std::unique_ptr<AbstractExecutor> right_;   // 右儿子节点（需要join的表）
    size_t len_;                                // join后获得的每条记录的长度
    std::vector<ColMeta> cols_;                 // join后获得的记录的字段

    std::vector<Condition> fed_conds_;          // join条件
    bool isend;

   public:
    NestedLoopJoinExecutor(std::unique_ptr<AbstractExecutor> left, std::unique_ptr<AbstractExecutor> right, 
                            std::vector<Condition> conds) {
        left_ = std::move(left);
        right_ = std::move(right);
        len_ = left_->tupleLen() + right_->tupleLen();
        cols_ = left_->cols();
        auto right_cols = right_->cols();
        for (auto &col : right_cols) {
            col.offset += left_->tupleLen();
        }

        cols_.insert(cols_.end(), right_cols.begin(), right_cols.end());
        isend = false;
        fed_conds_ = std::move(conds);

    }

    void beginTuple() override {
        left_->beginTuple();
        right_->beginTuple();
        isend = false;

        if (left_->is_end() || right_->is_end()) {
            isend = true;
            return;
        }

        // Position at first matching pair
        if (!fed_conds_.empty()) {
            while (true) {
                if (left_->is_end()) {
                    isend = true;
                    return;
                }
                if (right_->is_end()) {
                    left_->nextTuple();
                    if (left_->is_end()) {
                        isend = true;
                        return;
                    }
                    right_->beginTuple();
                    continue;
                }
                if (check_join_conds()) {
                    break;
                }
                right_->nextTuple();
            }
        }
    }

    void nextTuple() override {
        if (isend) return;

        // Advance right first to move past the current pair
        right_->nextTuple();

        // Find next matching pair
        while (true) {
            if (left_->is_end()) {
                isend = true;
                return;
            }
            if (right_->is_end()) {
                left_->nextTuple();
                if (left_->is_end()) {
                    isend = true;
                    return;
                }
                right_->beginTuple();
                continue;
            }

            if (fed_conds_.empty() || check_join_conds()) {
                break;
            } else {
                right_->nextTuple();
            }
        }
    }

    std::unique_ptr<RmRecord> Next() override {
        if (isend) return nullptr;

        auto left_rec = left_->Next();
        auto right_rec = right_->Next();

        if (left_rec == nullptr || right_rec == nullptr) {
            return nullptr;
        }

        auto rec = std::make_unique<RmRecord>(len_);
        memcpy(rec->data, left_rec->data, left_->tupleLen());
        memcpy(rec->data + left_->tupleLen(), right_rec->data, right_->tupleLen());

        return rec;
    }

    Rid &rid() override { return _abstract_rid; }

    size_t tupleLen() const override { return len_; }

    const std::vector<ColMeta> &cols() const override { return cols_; }

    bool is_end() const override { return isend; }

private:
    bool check_join_conds() {
        auto left_rec = left_->Next();
        auto right_rec = right_->Next();

        if (left_rec == nullptr || right_rec == nullptr) {
            return false;
        }

        // 重置，因为Next()会移动指针
        // 这里需要保存记录内容
        auto &left_cols = left_->cols();
        auto &right_cols = right_->cols();

        // 临时保存left_rec和right_rec的内容
        std::vector<char> left_data(left_rec->data, left_rec->data + left_->tupleLen());
        std::vector<char> right_data(right_rec->data, right_rec->data + right_->tupleLen());

        for (auto &cond : fed_conds_) {
            char *lhs_val = nullptr;
            char *rhs_val = nullptr;
            ColType lhs_type, rhs_type;
            int lhs_len, rhs_len;

            // 查找lhs列的offset和类型
            if (cond.lhs_col.tab_name == left_cols[0].tab_name) {
                for (auto &c : left_cols) {
                    if (c.name == cond.lhs_col.col_name) {
                        lhs_val = left_data.data() + c.offset;
                        lhs_type = c.type;
                        lhs_len = c.len;
                        break;
                    }
                }
            } else {
                for (auto &c : right_cols) {
                    if (c.name == cond.lhs_col.col_name) {
                        lhs_val = right_data.data() + c.offset;
                        lhs_type = c.type;
                        lhs_len = c.len;
                        break;
                    }
                }
            }

            // 查找rhs列的offset和类型
            if (cond.rhs_col.tab_name == left_cols[0].tab_name) {
                for (auto &c : left_cols) {
                    if (c.name == cond.rhs_col.col_name) {
                        rhs_val = left_data.data() + c.offset;
                        rhs_type = c.type;
                        rhs_len = c.len;
                        break;
                    }
                }
            } else {
                for (auto &c : right_cols) {
                    if (c.name == cond.rhs_col.col_name) {
                        rhs_val = right_data.data() + c.offset;
                        rhs_type = c.type;
                        rhs_len = c.len;
                        break;
                    }
                }
            }

            if (!compare_values(lhs_val, rhs_val, lhs_type, cond.op)) {
                return false;
            }
        }
        return true;
    }

    bool compare_values(char *lhs, char *rhs, ColType type, CompOp op) {
        switch (type) {
            case TYPE_INT:
                return compare_bigint(*(int32_t *)lhs, *(int32_t *)rhs, op);
            case TYPE_BIGINT:
                return compare_bigint(*(int64_t *)lhs, *(int64_t *)rhs, op);
            case TYPE_DATETIME:
                return compare_bigint(*(int64_t *)lhs, *(int64_t *)rhs, op);
            case TYPE_FLOAT:
                return compare_float(*(float *)lhs, *(float *)rhs, op);
            case TYPE_STRING:
                return compare_string(lhs, rhs, op);
            default:
                return false;
        }
    }

    bool compare_bigint(int64_t lhs, int64_t rhs, CompOp op) {
        switch (op) {
            case OP_EQ: return lhs == rhs;
            case OP_NE: return lhs != rhs;
            case OP_LT: return lhs < rhs;
            case OP_GT: return lhs > rhs;
            case OP_LE: return lhs <= rhs;
            case OP_GE: return lhs >= rhs;
            default: return false;
        }
    }

    bool compare_float(float lhs, float rhs, CompOp op) {
        switch (op) {
            case OP_EQ: return lhs == rhs;
            case OP_NE: return lhs != rhs;
            case OP_LT: return lhs < rhs;
            case OP_GT: return lhs > rhs;
            case OP_LE: return lhs <= rhs;
            case OP_GE: return lhs >= rhs;
            default: return false;
        }
    }

    bool compare_string(char *lhs, char *rhs, CompOp op) {
        std::string lhs_str(lhs);
        std::string rhs_str(rhs);
        switch (op) {
            case OP_EQ: return lhs_str == rhs_str;
            case OP_NE: return lhs_str != rhs_str;
            case OP_LT: return lhs_str < rhs_str;
            case OP_GT: return lhs_str > rhs_str;
            case OP_LE: return lhs_str <= rhs_str;
            case OP_GE: return lhs_str >= rhs_str;
            default: return false;
        }
    }
};