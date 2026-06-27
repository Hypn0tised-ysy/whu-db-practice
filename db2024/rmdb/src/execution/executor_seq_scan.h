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

class SeqScanExecutor : public AbstractExecutor {
   private:
    std::string tab_name_;              // 表的名称
    std::vector<Condition> conds_;      // scan的条件
    RmFileHandle *fh_;                  // 表的数据文件句柄
    std::vector<ColMeta> cols_;         // scan后生成的记录的字段
    size_t len_;                        // scan后生成的每条记录的长度
    std::vector<Condition> fed_conds_;  // 同conds_，两个字段相同

    Rid rid_;
    std::unique_ptr<RecScan> scan_;     // table_iterator

    SmManager *sm_manager_;

   public:
    SeqScanExecutor(SmManager *sm_manager, std::string tab_name, std::vector<Condition> conds, Context *context) {
        sm_manager_ = sm_manager;
        tab_name_ = std::move(tab_name);
        conds_ = std::move(conds);
        TabMeta &tab = sm_manager_->db_.get_table(tab_name_);
        fh_ = sm_manager_->fhs_.at(tab_name_).get();
        cols_ = tab.cols;
        len_ = cols_.back().offset + cols_.back().len;

        context_ = context;

        fed_conds_ = conds_;
    }

    void beginTuple() override {
        // 初始化扫描
        scan_ = std::make_unique<RmScan>(fh_);
        rid_ = scan_->rid();
    }

    void nextTuple() override {
        // 移动到下一个记录
        scan_->next();
        if (!scan_->is_end()) {
            rid_ = scan_->rid();
        }
    }

    std::unique_ptr<RmRecord> Next() override {
        if (scan_->is_end()) {
            return nullptr;
        }

        // 加S锁（读锁）
        if (context_ != nullptr && context_->txn_ != nullptr && context_->lock_mgr_ != nullptr) {
            context_->lock_mgr_->lock_shared_on_record(context_->txn_, rid_, fh_->GetFd());
        }

        // 读取当前记录
        auto rec = fh_->get_record(rid_, context_);

        // 检查条件
        if (eval_conds(conds_, rec, cols_)) {
            return rec;
        } else {
            // 如果不满足条件，跳过该记录
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
            char *lhs_val = nullptr;
            char *rhs_val = nullptr;
            Value lhs_value;
            Value rhs_value;

            // 获取lhs列的offset
            for (auto &col : rec_cols) {
                if (col.tab_name == cond.lhs_col.tab_name && col.name == cond.lhs_col.col_name) {
                    lhs_val = rec->data + col.offset;
                    if (cond.is_rhs_val) {
                        if (!eval_condition(lhs_val, col.tab_name, col.name, rec, rec_cols, cond.op, cond.rhs_val)) {
                            return false;
                        }
                    } else {
                        // 获取rhs列的offset
                        for (auto &r_col : rec_cols) {
                            if (r_col.tab_name == cond.rhs_col.tab_name && r_col.name == cond.rhs_col.col_name) {
                                rhs_val = rec->data + r_col.offset;
                                break;
                            }
                        }
                        if (!eval_binary(lhs_val, rhs_val, cond.op, col.type, col.len)) {
                            return false;
                        }
                    }
                    break;
                }
            }
        }
        return true;
    }

    bool eval_condition(char *lhs_val, const std::string &tab_name, const std::string &col_name,
                         const std::unique_ptr<RmRecord> &rec, const std::vector<ColMeta> &rec_cols,
                         CompOp op, const Value &rhs_val) {
        // 查找列的元数据
        ColMeta col;
        for (auto &c : rec_cols) {
            if (c.tab_name == tab_name && c.name == col_name) {
                col = c;
                break;
            }
        }

        // 根据列类型进行比较
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
        } else if (col.type == TYPE_BIGINT) {
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
        } else if (col.type == TYPE_DATETIME) {
            int64_t lhs = *(int64_t *)lhs_val;
            int64_t rhs = rhs_val.datetime_val;
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
        } else if (type == TYPE_BIGINT) {
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
        } else if (type == TYPE_DATETIME) {
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