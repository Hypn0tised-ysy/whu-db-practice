/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "ix_index_handle.h"

#include "ix_scan.h"

int IxNodeHandle::lower_bound(const char *target) const {
    int lo = 0, hi = page_hdr->num_key;
    while (lo < hi) {
        int mid = lo + (hi - lo) / 2;
        char *mid_key = get_key(mid);
        if (ix_compare(mid_key, target, file_hdr->col_types_, file_hdr->col_lens_) >= 0) {
            hi = mid;
        } else {
            lo = mid + 1;
        }
    }
    return lo;
}

int IxNodeHandle::upper_bound(const char *target) const {
    int lo = 0, hi = page_hdr->num_key;
    while (lo < hi) {
        int mid = lo + (hi - lo) / 2;
        char *mid_key = get_key(mid);
        if (ix_compare(mid_key, target, file_hdr->col_types_, file_hdr->col_lens_) > 0) {
            hi = mid;
        } else {
            lo = mid + 1;
        }
    }
    return lo;
}

bool IxNodeHandle::leaf_lookup(const char *key, Rid **value) {
    int pos = lower_bound(key);
    if (pos < page_hdr->num_key) {
        char *found_key = get_key(pos);
        if (ix_compare(found_key, key, file_hdr->col_types_, file_hdr->col_lens_) == 0) {
            if (value != nullptr) {
                *value = get_rid(pos);
            }
            return true;
        }
    }
    return false;
}

page_id_t IxNodeHandle::internal_lookup(const char *key) {
    int pos = upper_bound(key);
    // child pointer at pos (if pos == num_key, it's the last child pointer)
    if (pos >= page_hdr->num_key) pos = page_hdr->num_key - 1;
    // For non-leaf nodes, the child pointer before the position `pos` is at index `pos`
    // Actually: keys[i] >= key means go to child[i], otherwise go to child[i+1]
    // upper_bound returns first index where keys[i] > key
    // So child index = upper_bound(key) gives the right child to follow
    // Wait: standard B+tree internal lookup:
    // Find smallest i such that key < keys[i], then go to child[i]
    // If key >= all keys, go to child[num_key]
    int child_idx = upper_bound(key);
    return value_at(child_idx);
}

void IxNodeHandle::insert_pairs(int pos, const char *key, const Rid *rid, int n) {
    assert(pos >= 0 && pos <= page_hdr->num_key);
    // Shift existing keys and rids
    int num_key = page_hdr->num_key;
    for (int i = num_key - 1; i >= pos; i--) {
        memcpy(get_key(i + n), get_key(i), file_hdr->col_tot_len_);
        rids[i + n] = rids[i];
    }
    // Insert new pairs
    for (int i = 0; i < n; i++) {
        memcpy(get_key(pos + i), key + i * file_hdr->col_tot_len_, file_hdr->col_tot_len_);
        rids[pos + i] = rid[i];
    }
    page_hdr->num_key += n;
}

int IxNodeHandle::insert(const char *key, const Rid &value) {
    int pos = lower_bound(key);
    // Check for duplicate
    if (pos < page_hdr->num_key) {
        char *found_key = get_key(pos);
        if (ix_compare(found_key, key, file_hdr->col_types_, file_hdr->col_lens_) == 0) {
            // Duplicate key - throw error for unique index
            throw RMDBError("Duplicate entry for unique index");
        }
    }
    insert_pair(pos, key, value);
    return page_hdr->num_key;
}

void IxNodeHandle::erase_pair(int pos) {
    assert(pos >= 0 && pos < page_hdr->num_key);
    int num_key = page_hdr->num_key;
    for (int i = pos; i < num_key - 1; i++) {
        memcpy(get_key(i), get_key(i + 1), file_hdr->col_tot_len_);
        rids[i] = rids[i + 1];
    }
    page_hdr->num_key--;
}

int IxNodeHandle::remove(const char *key) {
    int pos = lower_bound(key);
    if (pos < page_hdr->num_key) {
        char *found_key = get_key(pos);
        if (ix_compare(found_key, key, file_hdr->col_types_, file_hdr->col_lens_) == 0) {
            erase_pair(pos);
        }
    }
    return page_hdr->num_key;
}

IxIndexHandle::IxIndexHandle(DiskManager *disk_manager, BufferPoolManager *buffer_pool_manager, int fd)
    : disk_manager_(disk_manager), buffer_pool_manager_(buffer_pool_manager), fd_(fd) {
    char* buf = new char[PAGE_SIZE];
    memset(buf, 0, PAGE_SIZE);
    disk_manager_->read_page(fd, IX_FILE_HDR_PAGE, buf, PAGE_SIZE);
    file_hdr_ = new IxFileHdr();
    file_hdr_->deserialize(buf);
    delete[] buf;

    int now_page_no = disk_manager_->get_fd2pageno(fd);
    disk_manager_->set_fd2pageno(fd, now_page_no + 1);
}

std::pair<IxNodeHandle *, bool> IxIndexHandle::find_leaf_page(const char *key, Operation operation,
                                                            Transaction *transaction, bool find_first) {
    page_id_t root_page = file_hdr_->root_page_;
    if (root_page == IX_NO_PAGE) {
        return std::make_pair(nullptr, false);
    }

    IxNodeHandle *node = fetch_node(root_page);

    while (!node->is_leaf_page()) {
        page_id_t child_page = node->internal_lookup(key);
        IxNodeHandle *child = fetch_node(child_page);
        buffer_pool_manager_->unpin_page(node->get_page_id(), false);
        node = child;
    }

    return std::make_pair(node, false);
}

bool IxIndexHandle::get_value(const char *key, std::vector<Rid> *result, Transaction *transaction) {
    auto [leaf, root_latched] = find_leaf_page(key, Operation::FIND, transaction);
    if (leaf == nullptr) return false;

    Rid *rid_ptr = nullptr;
    bool found = leaf->leaf_lookup(key, &rid_ptr);
    if (found && result != nullptr) {
        result->push_back(*rid_ptr);
    }
    buffer_pool_manager_->unpin_page(leaf->get_page_id(), false);
    return found;
}

IxNodeHandle *IxIndexHandle::split(IxNodeHandle *node) {
    IxNodeHandle *new_node = create_node();

    int total_keys = node->get_size();
    int left_keys = total_keys / 2;
    int right_keys = total_keys - left_keys;

    // Setup new node header
    new_node->page_hdr->is_leaf = node->is_leaf_page();
    new_node->page_hdr->parent = node->get_parent_page_no();
    new_node->page_hdr->num_key = 0;

    // Move right half of keys to new node
    new_node->insert_pairs(0, node->get_key(left_keys), node->get_rid(left_keys), right_keys);
    node->set_size(left_keys);

    if (node->is_leaf_page()) {
        // Update leaf linked list
        new_node->set_next_leaf(node->get_next_leaf());
        new_node->set_prev_leaf(node->get_page_no());
        node->set_next_leaf(new_node->get_page_no());

        // Update the next leaf's prev pointer
        if (new_node->get_next_leaf() != IX_LEAF_HEADER_PAGE) {
            IxNodeHandle *next = fetch_node(new_node->get_next_leaf());
            next->set_prev_leaf(new_node->get_page_no());
            buffer_pool_manager_->unpin_page(next->get_page_id(), true);
        } else {
            // This is the new last leaf
            file_hdr_->last_leaf_ = new_node->get_page_no();
        }
    } else {
        // Update children's parent pointers
        for (int i = 0; i < new_node->get_size(); i++) {
            maintain_child(new_node, i);
        }
    }

    return new_node;
}

void IxIndexHandle::insert_into_parent(IxNodeHandle *old_node, const char *key, IxNodeHandle *new_node,
                                     Transaction *transaction) {
    if (old_node->is_root_page()) {
        // Create new root
        IxNodeHandle *new_root = create_node();
        new_root->page_hdr->is_leaf = false;
        new_root->page_hdr->parent = IX_NO_PAGE;
        new_root->set_size(1);

        // Set first child pointer
        new_root->set_rid(0, Rid{old_node->get_page_no(), -1});
        // Set key as new_node's first key
        memcpy(new_root->get_key(0), new_node->get_key(0), file_hdr_->col_tot_len_);
        // Set second child pointer
        new_root->set_rid(1, Rid{new_node->get_page_no(), -1});

        old_node->set_parent_page_no(new_root->get_page_no());
        new_node->set_parent_page_no(new_root->get_page_no());

        update_root_page_no(new_root->get_page_no());

        buffer_pool_manager_->unpin_page(new_root->get_page_id(), true);
    } else {
        IxNodeHandle *parent = fetch_node(old_node->get_parent_page_no());

        // Insert the new_node's first key and new_node's rid into parent
        int child_idx = parent->find_child(old_node);
        char *insert_key = new_node->get_key(0);
        Rid insert_rid = Rid{new_node->get_page_no(), -1};
        parent->insert_pair(child_idx + 1, insert_key, insert_rid);

        if (parent->get_size() >= parent->get_max_size()) {
            IxNodeHandle *new_parent = split(parent);
            insert_into_parent(parent, new_parent->get_key(0), new_parent, transaction);
            buffer_pool_manager_->unpin_page(new_parent->get_page_id(), true);
        }

        buffer_pool_manager_->unpin_page(parent->get_page_id(), true);
    }
}

page_id_t IxIndexHandle::insert_entry(const char *key, const Rid &value, Transaction *transaction) {
    auto [leaf, root_latched] = find_leaf_page(key, Operation::INSERT, transaction);
    if (leaf == nullptr) {
        throw InternalError("Cannot find leaf page for insert");
    }

    int old_size = leaf->get_size();
    leaf->insert(key, value);
    page_id_t leaf_page = leaf->get_page_no();

    if (leaf->get_size() >= leaf->get_max_size()) {
        IxNodeHandle *new_leaf = split(leaf);
        insert_into_parent(leaf, new_leaf->get_key(0), new_leaf, transaction);
        buffer_pool_manager_->unpin_page(new_leaf->get_page_id(), true);
    }

    buffer_pool_manager_->unpin_page(leaf->get_page_id(), true);
    return leaf_page;
}

bool IxIndexHandle::delete_entry(const char *key, Transaction *transaction) {
    auto [leaf, root_latched] = find_leaf_page(key, Operation::DELETE, transaction);
    if (leaf == nullptr) return false;

    int pos = leaf->lower_bound(key);
    if (pos >= leaf->get_size() ||
        ix_compare(leaf->get_key(pos), key, file_hdr_->col_types_, file_hdr_->col_lens_) != 0) {
        buffer_pool_manager_->unpin_page(leaf->get_page_id(), false);
        return false;
    }

    leaf->erase_pair(pos);

    bool node_deleted = coalesce_or_redistribute(leaf, transaction);

    buffer_pool_manager_->unpin_page(leaf->get_page_id(), true);
    return true;
}

bool IxIndexHandle::coalesce_or_redistribute(IxNodeHandle *node, Transaction *transaction, bool *root_is_latched) {
    if (node->is_root_page()) {
        return adjust_root(node);
    }

    if (node->get_size() >= node->get_min_size()) {
        return false;
    }

    IxNodeHandle *parent = fetch_node(node->get_parent_page_no());
    int index = parent->find_child(node);

    // Try left sibling (predecessor) first, then right sibling
    IxNodeHandle *neighbor = nullptr;
    int neighbor_index = -1;
    bool neighbor_is_left = false;

    if (index > 0) {
        // Left sibling exists
        int left_child_page = parent->value_at(index - 1);
        neighbor = fetch_node(left_child_page);
        neighbor_index = index;
        neighbor_is_left = true;
    } else if (index < parent->get_size()) {
        // Right sibling exists (child at index+1)
        int right_child_page = parent->value_at(index + 1);
        neighbor = fetch_node(right_child_page);
        neighbor_index = index + 1;
        neighbor_is_left = false;
    }

    if (neighbor == nullptr) {
        buffer_pool_manager_->unpin_page(parent->get_page_id(), false);
        return false;
    }

    if (neighbor->get_size() + node->get_size() >= node->get_min_size() * 2) {
        redistribute(neighbor, node, parent, neighbor_index);
        buffer_pool_manager_->unpin_page(neighbor->get_page_id(), true);
        buffer_pool_manager_->unpin_page(parent->get_page_id(), true);
        return false;
    } else {
        // Ensure node is the right sibling
        if (!neighbor_is_left) {
            // Swap: neighbor (right) becomes node, node becomes neighbor (left)
            std::swap(neighbor, node);
            std::swap(neighbor_index, index);
        }
        bool parent_deleted = coalesce(&neighbor, &node, &parent, index, transaction, root_is_latched);
        buffer_pool_manager_->unpin_page(neighbor->get_page_id(), true);
        if (!parent_deleted) {
            buffer_pool_manager_->unpin_page(parent->get_page_id(), true);
        }
        return true;
    }
}

bool IxIndexHandle::adjust_root(IxNodeHandle *old_root_node) {
    if (old_root_node->is_leaf_page() && old_root_node->get_size() == 0) {
        update_root_page_no(IX_NO_PAGE);
        release_node_handle(*old_root_node);
        return true;
    }
    if (!old_root_node->is_leaf_page() && old_root_node->get_size() == 1) {
        // The only child becomes the new root
        page_id_t child_page = old_root_node->value_at(0);
        IxNodeHandle *child = fetch_node(child_page);
        child->set_parent_page_no(IX_NO_PAGE);
        buffer_pool_manager_->unpin_page(child->get_page_id(), true);

        update_root_page_no(child_page);
        release_node_handle(*old_root_node);
        return true;
    }
    return false;
}

void IxIndexHandle::redistribute(IxNodeHandle *neighbor_node, IxNodeHandle *node, IxNodeHandle *parent, int index) {
    // index is the position of the key in parent that separates neighbor and node
    // If neighbor is left sibling: neighbor (index-1) | key(index-1) | node (index)
    // If neighbor is right sibling: node (index) | key(index) | neighbor (index+1)

    bool neighbor_is_left = (parent->value_at(index - 1) == neighbor_node->get_page_no());

    if (neighbor_is_left) {
        // Move last pair from neighbor to front of node
        int neighbor_last = neighbor_node->get_size() - 1;
        node->insert_pair(0, neighbor_node->get_key(neighbor_last), *neighbor_node->get_rid(neighbor_last));
        neighbor_node->set_size(neighbor_node->get_size() - 1);

        // Update parent key at index-1 to be node's new first key
        memcpy(parent->get_key(index - 1), node->get_key(0), file_hdr_->col_tot_len_);

        // Update child parent pointer if internal node
        if (!node->is_leaf_page()) {
            maintain_child(node, 0);
        }
    } else {
        // Move first pair from neighbor to end of node
        node->insert_pair(node->get_size(), neighbor_node->get_key(0), *neighbor_node->get_rid(0));
        neighbor_node->erase_pair(0);

        // Update parent key at index to be neighbor's new first key
        memcpy(parent->get_key(index), neighbor_node->get_key(0), file_hdr_->col_tot_len_);

        // Update child parent pointer if internal node
        if (!node->is_leaf_page()) {
            maintain_child(node, node->get_size() - 1);
        }
    }
}

bool IxIndexHandle::coalesce(IxNodeHandle **neighbor_node, IxNodeHandle **node, IxNodeHandle **parent, int index,
                             Transaction *transaction, bool *root_is_latched) {
    // Ensure neighbor is left, node is right
    int node_idx = (*parent)->find_child(*node);
    int neighbor_idx = (*parent)->find_child(*neighbor_node);

    if (neighbor_idx > node_idx) {
        std::swap(neighbor_node, node);
    }

    // Move all pairs from node to neighbor
    int neighbor_size = (*neighbor_node)->get_size();
    int node_size = (*node)->get_size();
    (*neighbor_node)->insert_pairs(neighbor_size, (*node)->get_key(0), (*node)->get_rid(0), node_size);

    if ((*node)->is_leaf_page()) {
        // Update leaf linked list
        (*neighbor_node)->set_next_leaf((*node)->get_next_leaf());

        if ((*node)->get_next_leaf() != IX_LEAF_HEADER_PAGE) {
            IxNodeHandle *next = fetch_node((*node)->get_next_leaf());
            next->set_prev_leaf((*neighbor_node)->get_page_no());
            buffer_pool_manager_->unpin_page(next->get_page_id(), true);
        } else {
            file_hdr_->last_leaf_ = (*neighbor_node)->get_page_no();
        }
    } else {
        // Update children's parent pointers for moved pairs
        for (int i = neighbor_size; i < (*neighbor_node)->get_size(); i++) {
            maintain_child(*neighbor_node, i);
        }
    }

    // Remove the entry for node from parent: the key at position neighbor_idx
    int key_pos = (*parent)->find_child(*node) - 1;
    if (key_pos < 0) key_pos = 0;
    (*parent)->erase_pair(key_pos);

    // Delete the node
    release_node_handle(**node);

    // Check if parent needs adjustment
    if ((*parent)->is_root_page()) {
        return adjust_root(*parent);
    }

    if ((*parent)->get_size() < (*parent)->get_min_size()) {
        return coalesce_or_redistribute(*parent, transaction, root_is_latched);
    }

    return false;
}

Iid IxIndexHandle::lower_bound(const char *key) {
    auto [leaf, root_latched] = find_leaf_page(key, Operation::FIND, nullptr);
    if (leaf == nullptr) {
        return leaf_end();
    }
    int slot = leaf->lower_bound(key);
    Iid iid = {.page_no = leaf->get_page_no(), .slot_no = slot};
    if (slot == leaf->get_size()) {
        // Move to next leaf
        page_id_t next = leaf->get_next_leaf();
        if (next != IX_LEAF_HEADER_PAGE && next != IX_NO_PAGE) {
            iid.page_no = next;
            iid.slot_no = 0;
        }
    }
    buffer_pool_manager_->unpin_page(leaf->get_page_id(), false);
    return iid;
}

Iid IxIndexHandle::upper_bound(const char *key) {
    auto [leaf, root_latched] = find_leaf_page(key, Operation::FIND, nullptr);
    if (leaf == nullptr) {
        return leaf_end();
    }
    int slot = leaf->upper_bound(key);
    Iid iid = {.page_no = leaf->get_page_no(), .slot_no = slot};
    if (slot == leaf->get_size()) {
        page_id_t next = leaf->get_next_leaf();
        if (next != IX_LEAF_HEADER_PAGE && next != IX_NO_PAGE) {
            iid.page_no = next;
            iid.slot_no = 0;
        }
    }
    buffer_pool_manager_->unpin_page(leaf->get_page_id(), false);
    return iid;
}

// --- Original helper functions (framework-provided, kept as-is) ---

Rid IxIndexHandle::get_rid(const Iid &iid) const {
    IxNodeHandle *node = fetch_node(iid.page_no);
    if (iid.slot_no >= node->get_size()) {
        throw IndexEntryNotFoundError();
    }
    buffer_pool_manager_->unpin_page(node->get_page_id(), false);
    return *node->get_rid(iid.slot_no);
}

Iid IxIndexHandle::leaf_end() const {
    IxNodeHandle *node = fetch_node(file_hdr_->last_leaf_);
    Iid iid = {.page_no = file_hdr_->last_leaf_, .slot_no = node->get_size()};
    buffer_pool_manager_->unpin_page(node->get_page_id(), false);
    return iid;
}

Iid IxIndexHandle::leaf_begin() const {
    Iid iid = {.page_no = file_hdr_->first_leaf_, .slot_no = 0};
    return iid;
}

IxNodeHandle *IxIndexHandle::fetch_node(int page_no) const {
    Page *page = buffer_pool_manager_->fetch_page(PageId{fd_, page_no});
    IxNodeHandle *node = new IxNodeHandle(file_hdr_, page);
    return node;
}

IxNodeHandle *IxIndexHandle::create_node() {
    IxNodeHandle *node;
    file_hdr_->num_pages_++;

    PageId new_page_id = {.fd = fd_, .page_no = INVALID_PAGE_ID};
    Page *page = buffer_pool_manager_->new_page(&new_page_id);
    node = new IxNodeHandle(file_hdr_, page);
    return node;
}

void IxIndexHandle::maintain_parent(IxNodeHandle *node) {
    IxNodeHandle *curr = node;
    while (curr->get_parent_page_no() != IX_NO_PAGE) {
        IxNodeHandle *parent = fetch_node(curr->get_parent_page_no());
        int rank = parent->find_child(curr);
        char *parent_key = parent->get_key(rank);
        char *child_first_key = curr->get_key(0);
        if (memcmp(parent_key, child_first_key, file_hdr_->col_tot_len_) == 0) {
            assert(buffer_pool_manager_->unpin_page(parent->get_page_id(), true));
            break;
        }
        memcpy(parent_key, child_first_key, file_hdr_->col_tot_len_);
        curr = parent;
        assert(buffer_pool_manager_->unpin_page(parent->get_page_id(), true));
    }
}

void IxIndexHandle::erase_leaf(IxNodeHandle *leaf) {
    assert(leaf->is_leaf_page());
    IxNodeHandle *prev = fetch_node(leaf->get_prev_leaf());
    prev->set_next_leaf(leaf->get_next_leaf());
    buffer_pool_manager_->unpin_page(prev->get_page_id(), true);
    IxNodeHandle *next = fetch_node(leaf->get_next_leaf());
    next->set_prev_leaf(leaf->get_prev_leaf());
    buffer_pool_manager_->unpin_page(next->get_page_id(), true);
}

void IxIndexHandle::release_node_handle(IxNodeHandle &node) {
    file_hdr_->num_pages_--;
}

void IxIndexHandle::maintain_child(IxNodeHandle *node, int child_idx) {
    if (!node->is_leaf_page()) {
        int child_page_no = node->value_at(child_idx);
        IxNodeHandle *child = fetch_node(child_page_no);
        child->set_parent_page_no(node->get_page_no());
        buffer_pool_manager_->unpin_page(child->get_page_id(), true);
    }
}
