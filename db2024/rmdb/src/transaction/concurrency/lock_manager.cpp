#include "lock_manager.h"
#include "transaction/txn_defs.h"

using LM = LockManager::LockMode;
using GM = LockManager::GroupLockMode;

static bool compatible(LM requested, GM held) {
    if (held == GM::NON_LOCK) return true;
    switch (requested) {
        case LM::SHARED:            return held == GM::IS || held == GM::S;
        case LM::EXLUCSIVE:         return false;
        case LM::INTENTION_SHARED:  return held != GM::X;
        case LM::INTENTION_EXCLUSIVE: return held == GM::IS || held == GM::IX;
        case LM::S_IX:              return held == GM::IS;
    }
    return false;
}

static GM upgrade_group(GM cur, LM added) {
    if (cur == GM::X || added == LM::EXLUCSIVE) return GM::X;
    if (cur == GM::S && added == LM::INTENTION_EXCLUSIVE) return GM::SIX;
    if (cur == GM::IX && added == LM::SHARED) return GM::SIX;
    if (cur == GM::SIX) return GM::SIX;
    switch (added) {
        case LM::SHARED: return (cur == GM::IS || cur == GM::NON_LOCK) ? GM::S : GM::S;
        case LM::INTENTION_EXCLUSIVE: return (cur == GM::IS || cur == GM::NON_LOCK) ? GM::IX : GM::IX;
        case LM::INTENTION_SHARED: return (cur == GM::NON_LOCK) ? GM::IS : cur;
        case LM::S_IX: return GM::SIX;
        default: return cur;
    }
}

bool LockManager::lock_shared_on_record(Transaction* txn, const Rid& rid, int tab_fd)
{ return lock(txn, LockDataId(tab_fd, rid, LockDataType::RECORD), LM::SHARED); }
bool LockManager::lock_exclusive_on_record(Transaction* txn, const Rid& rid, int tab_fd)
{ return lock(txn, LockDataId(tab_fd, rid, LockDataType::RECORD), LM::EXLUCSIVE); }
bool LockManager::lock_shared_on_table(Transaction* txn, int tab_fd)
{ return lock(txn, LockDataId(tab_fd, LockDataType::TABLE), LM::SHARED); }
bool LockManager::lock_exclusive_on_table(Transaction* txn, int tab_fd)
{ return lock(txn, LockDataId(tab_fd, LockDataType::TABLE), LM::EXLUCSIVE); }
bool LockManager::lock_IS_on_table(Transaction* txn, int tab_fd)
{ return lock(txn, LockDataId(tab_fd, LockDataType::TABLE), LM::INTENTION_SHARED); }
bool LockManager::lock_IX_on_table(Transaction* txn, int tab_fd)
{ return lock(txn, LockDataId(tab_fd, LockDataType::TABLE), LM::INTENTION_EXCLUSIVE); }

bool LockManager::lock(Transaction* txn, const LockDataId &lid, LM mode) {
    std::unique_lock<std::mutex> lk(latch_);
    auto &q = lock_table_[lid];
    GM cur = q.group_lock_mode_;

    // Check if this txn already holds a lock here
    bool held = false; LM held_mode = LM::SHARED;
    for (auto &r : q.request_queue_) {
        if (r.txn_id_ == txn->get_transaction_id() && r.granted_) { held = true; held_mode = r.lock_mode_; break; }
    }
    if (held) {
        if (mode == held_mode) return true;
        // Upgrade S→X: must be the ONLY holder
        if (mode == LM::EXLUCSIVE && held_mode == LM::SHARED) {
            for (auto &r : q.request_queue_)
                if (r.txn_id_ != txn->get_transaction_id() && r.granted_)
                    { lk.unlock(); throw TransactionAbortException(txn->get_transaction_id(), AbortReason::UPGRADE_CONFLICT); }
            q.group_lock_mode_ = upgrade_group(cur, mode);
            // 更新已授予的锁模式
            for (auto &r : q.request_queue_)
                if (r.txn_id_ == txn->get_transaction_id() && r.granted_) { r.lock_mode_ = mode; break; }
            txn->get_lock_set()->insert(lid); return true;
        }
        // Upgrade S→IX (resulting in SIX): must be the ONLY S holder
        if (mode == LM::INTENTION_EXCLUSIVE && held_mode == LM::SHARED) {
            for (auto &r : q.request_queue_)
                if (r.txn_id_ != txn->get_transaction_id() && r.granted_ && r.lock_mode_ == LM::SHARED)
                    { lk.unlock(); throw TransactionAbortException(txn->get_transaction_id(), AbortReason::UPGRADE_CONFLICT); }
            // 升级为 SIX
            for (auto &r : q.request_queue_)
                if (r.txn_id_ == txn->get_transaction_id() && r.granted_) { r.lock_mode_ = LM::S_IX; break; }
            q.group_lock_mode_ = upgrade_group(cur, LM::INTENTION_EXCLUSIVE);
            txn->get_lock_set()->insert(lid);
            return true;
        }
        return true;
    }

    // No-wait: incompatible → abort
    if (!compatible(mode, cur))
        { lk.unlock(); throw TransactionAbortException(txn->get_transaction_id(), AbortReason::DEADLOCK_PREVENTION); }
    // No-wait: waiting requests ahead → abort
    for (auto &r : q.request_queue_)
        if (!r.granted_) { lk.unlock(); throw TransactionAbortException(txn->get_transaction_id(), AbortReason::DEADLOCK_PREVENTION); }

    // Grant
    LockRequest req(txn->get_transaction_id(), mode);
    req.granted_ = true;
    q.request_queue_.push_back(req);
    q.group_lock_mode_ = upgrade_group(cur, mode);
    txn->get_lock_set()->insert(lid);
    return true;
}

bool LockManager::unlock(Transaction* txn, LockDataId lid) {
    std::unique_lock<std::mutex> lk(latch_);
    auto it = lock_table_.find(lid);
    if (it == lock_table_.end()) return true;
    auto &q = it->second;
    for (auto ri = q.request_queue_.begin(); ri != q.request_queue_.end(); ++ri)
        if (ri->txn_id_ == txn->get_transaction_id() && ri->granted_) { q.request_queue_.erase(ri); break; }
    q.group_lock_mode_ = GM::NON_LOCK;
    for (auto &r : q.request_queue_)
        if (r.granted_) q.group_lock_mode_ = upgrade_group(q.group_lock_mode_, r.lock_mode_);
    if (q.request_queue_.empty()) lock_table_.erase(it);
    return true;
}
