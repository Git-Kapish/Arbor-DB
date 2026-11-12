#include "concurrency/lock_manager.h"

#include <algorithm>

namespace arbordb {

// A new (not yet granted) request may be granted when no request ahead of it conflicts (FIFO: later
// readers cannot overtake a waiting writer). An upgrader needs every OTHER holder gone.
bool LockManager::CanGrant(Queue& q, std::list<Request>::iterator it) {
    if (it->upgrading) {
        for (auto o = q.requests.begin(); o != q.requests.end(); ++o)
            if (o != it && o->granted) return false;
        return true;
    }
    for (auto o = q.requests.begin(); o != it; ++o)
        if (Conflicts(Effective(*o), it->mode)) return false;
    return true;
}

void LockManager::BuildGraph(Graph* g) {
    // Only queues somebody is blocked in can contribute edges; skipping the rest keeps this cheap.
    std::set<Queue*> queues;
    for (auto& [t, w] : waiting_) queues.insert(w.queue);
    for (Queue* q : queues) {
        for (auto it = q->requests.begin(); it != q->requests.end(); ++it) {
            bool blocked = !it->granted || it->upgrading;
            if (!blocked) continue;
            txn_id_t me = it->txn->Id();
            (*g)[me];  // ensure node exists
            if (it->upgrading) {
                for (auto o = q->requests.begin(); o != q->requests.end(); ++o)
                    if (o != it && o->granted) (*g)[me].insert(o->txn->Id());
            } else {
                for (auto o = q->requests.begin(); o != it; ++o)
                    if (Conflicts(Effective(*o), it->mode)) (*g)[me].insert(o->txn->Id());
            }
        }
    }
}

bool LockManager::ResolveDeadlocks(txn_id_t self) {
    bool self_victim = false;
    while (true) {
        Graph g;
        BuildGraph(&g);
        // Victims already flagged are on their way out: they will not wait on anything more.
        for (auto& [t, w] : waiting_)
            if (w.txn->State() == TxnState::kAborted) g.erase(t);

        std::map<txn_id_t, int> color;  // 0 white, 1 on stack, 2 done
        std::vector<txn_id_t> stack, cycle;
        std::function<bool(txn_id_t)> dfs = [&](txn_id_t u) -> bool {
            color[u] = 1;
            stack.push_back(u);
            auto it = g.find(u);
            if (it != g.end()) {
                for (txn_id_t v : it->second) {  // std::set: ascending ids
                    if (color[v] == 1) {
                        cycle.assign(std::find(stack.begin(), stack.end(), v), stack.end());
                        return true;
                    }
                    if (color[v] == 0 && dfs(v)) return true;
                }
            }
            stack.pop_back();
            color[u] = 2;
            return false;
        };
        bool found = false;
        for (auto& [t, edges] : g) {
            if (color[t] == 0 && dfs(t)) { found = true; break; }
        }
        if (!found) return self_victim;

        txn_id_t victim = *std::max_element(cycle.begin(), cycle.end());  // youngest transaction
        deadlocks_++;
        auto w = waiting_.find(victim);
        if (w != waiting_.end()) {
            w->second.txn->SetState(TxnState::kAborted);
            w->second.queue->cv.notify_all();
        }
        if (victim == self) self_victim = true;
    }
}

void LockManager::RemoveRequest(const RID& rid, Queue& q, std::list<Request>::iterator it, txn_id_t txn) {
    bool was_granted = it->granted;
    q.requests.erase(it);
    if (was_granted) {
        auto h = held_.find(txn);
        if (h != held_.end()) {
            h->second.erase(rid);
            if (h->second.empty()) held_.erase(h);
        }
    }
    if (q.requests.empty()) {
        table_.erase(rid);  // q is kept alive by the caller's shared_ptr
    } else {
        q.cv.notify_all();
    }
}

bool LockManager::Lock(Transaction* txn, const RID& rid, LockMode mode) {
    std::unique_lock<std::mutex> lk(mu_);
    txn_id_t id = txn->Id();
    if (txn->State() == TxnState::kAborted) return false;

    std::shared_ptr<Queue> qp = table_[rid];
    if (!qp) qp = table_[rid] = std::make_shared<Queue>();
    Queue& q = *qp;

    std::list<Request>::iterator mine = q.requests.end();
    for (auto it = q.requests.begin(); it != q.requests.end(); ++it)
        if (it->txn->Id() == id) { mine = it; break; }

    if (mine != q.requests.end()) {  // re-entrant request
        if (mine->mode == LockMode::kExclusive || mode == LockMode::kShared) return true;  // already strong enough
        mine->upgrading = true;  // S -> X
    } else {
        q.requests.push_back(Request{txn, mode, false, false});
        mine = std::prev(q.requests.end());
    }

    bool is_upgrade = mine->upgrading;
    while (!CanGrant(q, mine)) {
        waiting_[id] = Waiter{txn, &q};
        bool victim = ResolveDeadlocks(id);
        if (victim || txn->State() == TxnState::kAborted) {
            waiting_.erase(id);
            if (is_upgrade) {
                mine->upgrading = false;  // keep the S lock it already had
                q.cv.notify_all();
            } else {
                RemoveRequest(rid, q, mine, id);
            }
            return false;
        }
        q.cv.wait(lk);
    }
    waiting_.erase(id);
    if (is_upgrade) {
        mine->upgrading = false;
        mine->mode = LockMode::kExclusive;
    } else {
        mine->granted = true;
        held_[id].insert(rid);
    }
    return true;
}

void LockManager::Unlock(Transaction* txn, const RID& rid) {
    std::lock_guard<std::mutex> g(mu_);
    auto t = table_.find(rid);
    if (t == table_.end()) return;
    std::shared_ptr<Queue> qp = t->second;
    for (auto it = qp->requests.begin(); it != qp->requests.end(); ++it) {
        if (it->txn->Id() == txn->Id() && it->granted) {
            RemoveRequest(rid, *qp, it, txn->Id());
            return;
        }
    }
}

void LockManager::UnlockAll(Transaction* txn) {
    std::lock_guard<std::mutex> g(mu_);
    auto h = held_.find(txn->Id());
    if (h == held_.end()) return;
    std::set<RID> rids = h->second;  // copy: RemoveRequest edits held_
    for (const RID& rid : rids) {
        auto t = table_.find(rid);
        if (t == table_.end()) continue;
        std::shared_ptr<Queue> qp = t->second;
        for (auto it = qp->requests.begin(); it != qp->requests.end(); ++it) {
            if (it->txn->Id() == txn->Id() && it->granted) {
                RemoveRequest(rid, *qp, it, txn->Id());
                break;
            }
        }
    }
    held_.erase(txn->Id());
}

std::size_t LockManager::DeadlocksDetected() {
    std::lock_guard<std::mutex> g(mu_);
    return deadlocks_;
}

std::size_t LockManager::LockCount(txn_id_t txn) {
    std::lock_guard<std::mutex> g(mu_);
    auto h = held_.find(txn);
    return h == held_.end() ? 0 : h->second.size();
}

}  // namespace arbordb
