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

#include <cassert>
#include <cstring>
#include <memory>
#include <string>
#include <vector>
#include "defs.h"
#include "record/rm_defs.h"


struct TabCol {
    std::string tab_name;
    std::string col_name;

    friend bool operator<(const TabCol &x, const TabCol &y) {
        return std::make_pair(x.tab_name, x.col_name) < std::make_pair(y.tab_name, y.col_name);
    }
};

struct Value {
    ColType type;  // type of value
    union {
        int64_t bigint_val;   // bigint/int value (64-bit)
        float float_val;      // float value
        int64_t datetime_val; // datetime value (packed 64-bit)
    };
    std::string str_val;  // string value

    std::shared_ptr<RmRecord> raw;  // raw record buffer

    void set_int(int64_t int_val_) {
        type = TYPE_INT;
        bigint_val = int_val_;
    }

    void set_float(float float_val_) {
        type = TYPE_FLOAT;
        float_val = float_val_;
    }

    void set_str(std::string str_val_) {
        type = TYPE_STRING;
        str_val = std::move(str_val_);
    }

    void set_bigint(int64_t bigint_val_) {
        type = TYPE_BIGINT;
        bigint_val = bigint_val_;
    }

    void set_datetime(int64_t dt_val) {
        type = TYPE_DATETIME;
        datetime_val = dt_val;
    }

    void init_raw(int len) {
        assert(raw == nullptr);
        raw = std::make_shared<RmRecord>(len);
        if (type == TYPE_INT) {
            assert(len == sizeof(int32_t));
            *(int32_t *)(raw->data) = (int32_t)bigint_val;
        } else if (type == TYPE_BIGINT) {
            assert(len == sizeof(int64_t));
            *(int64_t *)(raw->data) = bigint_val;
        } else if (type == TYPE_DATETIME) {
            assert(len == 8);
            *(int64_t *)(raw->data) = datetime_val;
        } else if (type == TYPE_FLOAT) {
            assert(len == sizeof(float));
            *(float *)(raw->data) = float_val;
        } else if (type == TYPE_STRING) {
            if (len < (int)str_val.size()) {
                throw StringOverflowError();
            }
            memset(raw->data, 0, len);
            memcpy(raw->data, str_val.c_str(), str_val.size());
        }
    }
};

enum CompOp { OP_EQ, OP_NE, OP_LT, OP_GT, OP_LE, OP_GE };

struct Condition {
    TabCol lhs_col;   // left-hand side column
    CompOp op;        // comparison operator
    bool is_rhs_val;  // true if right-hand side is a value (not a column)
    TabCol rhs_col;   // right-hand side column
    Value rhs_val;    // right-hand side value
};

struct SetClause {
    TabCol lhs;
    Value rhs;
};

// DATETIME helper functions
// Packed format: [second:6][minute:6][hour:5][day:5][month:4][year:14] = 40 bits in int64_t

inline int64_t parse_datetime(const std::string &str) {
    // Expected format: YYYY-MM-DD HH:MM:SS (19 chars)
    if (str.size() != 19) {
        throw RMDBError("Invalid datetime format");
    }
    if (str[4] != '-' || str[7] != '-' || str[10] != ' ' || str[13] != ':' || str[16] != ':') {
        throw RMDBError("Invalid datetime format");
    }

    int year = std::stoi(str.substr(0, 4));
    int month = std::stoi(str.substr(5, 2));
    int day = std::stoi(str.substr(8, 2));
    int hour = std::stoi(str.substr(11, 2));
    int minute = std::stoi(str.substr(14, 2));
    int second = std::stoi(str.substr(17, 2));

    // Validate ranges
    if (year < 1000 || year > 9999) throw RMDBError("Invalid datetime: year out of range");
    if (month < 1 || month > 12) throw RMDBError("Invalid datetime: month out of range");
    if (day < 1 || day > 31) throw RMDBError("Invalid datetime: day out of range");
    if (hour < 0 || hour > 23) throw RMDBError("Invalid datetime: hour out of range");
    if (minute < 0 || minute > 59) throw RMDBError("Invalid datetime: minute out of range");
    if (second < 0 || second > 59) throw RMDBError("Invalid datetime: second out of range");

    // Validate day against month (leap year aware)
    int days_in_month[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if ((year % 4 == 0 && year % 100 != 0) || year % 400 == 0) {
        days_in_month[1] = 29;
    }
    if (day > days_in_month[month - 1]) {
        throw RMDBError("Invalid datetime: day out of range for month");
    }

    // Pack into int64_t
    int64_t result = 0;
    result |= (int64_t)second;
    result |= (int64_t)minute << 6;
    result |= (int64_t)hour << 12;
    result |= (int64_t)day << 17;
    result |= (int64_t)month << 22;
    result |= (int64_t)year << 26;
    return result;
}

inline std::string format_datetime(int64_t dt) {
    int second = dt & 0x3F;
    int minute = (dt >> 6) & 0x3F;
    int hour = (dt >> 12) & 0x1F;
    int day = (dt >> 17) & 0x1F;
    int month = (dt >> 22) & 0xF;
    int year = (dt >> 26) & 0x3FFF;
    char buf[20];
    snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d",
             year, month, day, hour, minute, second);
    return std::string(buf);
}