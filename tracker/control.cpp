#include "control.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <iterator>
#include <thread>

#include "../common/util.h"
#include "oplog.h"
#include "state.h"
#include "sync.h"


// Set by the hidden debug_delay command: creates sleep this long between the
// local insert and the BCAST, so two trackers can be made to create the same name at once.
static std::atomic<int> create_delay_ms{0};

// Set by the hidden debug_drop_next_bcast command: the next BCAST is not sent at all.
static std::atomic<bool> drop_next_bcast{false};


// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// My next sequence number. Every op I start gets one.
static uint64_t take_seq() {
    std::lock_guard<std::mutex> lk(log_mtx);

    my_seq++;
    return my_seq;
}


// Ids look like (origin << 48) | seq, so two trackers can never hand out the same one,
// and comparing two ids is the "smaller id wins" rule.
static uint64_t make_id(int origin, uint64_t seq) {
    return ((uint64_t)origin << 48) | seq;
}


static uint64_t to_u64(const std::string& s) {
    try {
        return std::stoull(s);
    } catch (...) {
        return 0;
    }
}


// I hold groups_mtx only for the lookup. The caller locks g->mtx afterwards.
std::shared_ptr<group_rec> get_group(const std::string& gid) {
    std::lock_guard<std::mutex> lk(groups_mtx);

    auto it = groups.find(gid);
    if (it == groups.end()) {
        return nullptr;
    }

    return it->second;
}


// When the last member leaves, the group is erased from the map. Someone who grabbed
// the pointer just before that will see an empty member list, so I treat that as "gone".
bool gone(const group_rec& g) {
    return g.members.empty();
}


bool is_member(const group_rec& g, const std::string& uid) {
    return g.pos.count(uid) > 0;
}


// The owner is whoever is first in the members list.
static bool is_owner(const group_rec& g, const std::string& uid) {
    if (g.members.empty()) {
        return false;
    }

    return g.members.front() == uid;
}


// A brand-new group whose only member (and so its owner) is the creator.
static std::shared_ptr<group_rec> new_group(uint64_t id, const std::string& creator) {
    auto g = std::make_shared<group_rec>();
    g->id = id;

    g->members.push_back(creator);
    g->pos[creator] = g->members.begin();

    return g;
}


// "BCAST <my_id> <seq> <OP...>" to every other tracker. Returns their replies.
static std::vector<std::string> broadcast(uint64_t seq, const std::vector<std::string>& op) {
    std::string msg = "BCAST " + std::to_string(my_id) + " " + std::to_string(seq) + " " + join(op, ' ');

    // debug_drop_next_bcast: pretend this one got lost on the way to everyone.
    // The op is still in my log, so HB + SYNC must repair it.
    if (drop_next_bcast.exchange(false)) {
        fprintf(stderr, "[tracker %d] debug: dropping %s\n", my_id, msg.c_str());
        return std::vector<std::string>(N);
    }

    return send_to_all(msg);
}


static bool any_conflict(const std::vector<std::string>& replies) {
    for (const std::string& r : replies) {
        if (r == "CONFLICT") {
            return true;
        }
    }

    return false;
}


static void sleep_for_debug() {
    int ms = create_delay_ms;

    if (ms > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(ms));
    }
}


// ---------------------------------------------------------------------------
// Applying replicated ops
// ---------------------------------------------------------------------------

// CREATE_USER <uid> <pwd> <id>
static std::string apply_create_user(const std::vector<std::string>& op) {
    const std::string& uid = op[1];
    uint64_t id = to_u64(op[3]);

    std::lock_guard<std::mutex> lk(users_mtx);

    auto it = users.find(uid);

    // Free name, or the same op seen again: just (re)write it.
    // An existing user with a bigger id loses to this one, so I replace it.
    if (it == users.end() || it->second.id >= id) {
        user_rec rec;
        rec.pwd = op[2];
        rec.id  = id;
        users[uid] = rec;
        return "ACK";
    }

    // TODO(design): phase 6 says "smaller or equal id -> CONFLICT". An equal id can only be
    // the very same op arriving twice, so I treat it as ACK (handled above) instead.
    return "CONFLICT";
}


// CREATE_GROUP <gid> <id> <creator_uid>
static std::string apply_create_group(const std::vector<std::string>& op) {
    const std::string& gid = op[1];
    uint64_t id = to_u64(op[2]);
    const std::string& creator = op[3];

    std::lock_guard<std::mutex> lk(groups_mtx);

    auto it = groups.find(gid);

    if (it == groups.end()) {
        groups[gid] = new_group(id, creator);
        return "ACK";
    }

    // Reading g->id is safe here: it is only set before a group goes into the map.
    uint64_t have = it->second->id;

    if (have == id) {
        return "ACK";   // the same op seen again
    }

    if (have > id) {
        // The incoming create is older, so it wins: a fresh record with only its creator.
        it->second = new_group(id, creator);
        return "ACK";
    }

    return "CONFLICT";
}


// JOIN_REQ <gid> <uid>
static std::string apply_join(const std::vector<std::string>& op) {
    auto g = get_group(op[1]);
    if (!g) {
        return "ACK";
    }

    std::lock_guard<std::mutex> lk(g->mtx);

    if (!gone(*g) && !is_member(*g, op[2])) {
        g->pending.insert(op[2]);
    }

    return "ACK";
}


// ACCEPT <gid> <uid>
static std::string apply_accept(const std::vector<std::string>& op) {
    auto g = get_group(op[1]);
    if (!g) {
        return "ACK";
    }

    const std::string& uid = op[2];

    std::lock_guard<std::mutex> lk(g->mtx);

    if (gone(*g)) {
        return "ACK";
    }

    g->pending.erase(uid);

    // Moving them to the end of the members list, only once.
    if (!is_member(*g, uid)) {
        g->members.push_back(uid);
        g->pos[uid] = std::prev(g->members.end());
    }

    return "ACK";
}


// Someone who left a group must not be handed out as a seeder of its files any more.
// Every peer that belongs to this uid is erased from every chunk of every file I hold
// metadata for. Caller holds g.mtx (and so may take peers_mtx, which comes after it).
static void remove_user_seeders(group_rec& g, const std::string& uid) {
    std::lock_guard<std::mutex> lk(peers_mtx);

    for (auto& kv : g.files) {
        file_meta* meta = kv.second.meta.get();
        if (!meta) {
            continue;
        }

        for (auto& chunk_seeders : meta->seeders) {
            for (auto it = chunk_seeders.begin(); it != chunk_seeders.end(); ) {
                auto who = peer_uid.find(*it);

                if (who != peer_uid.end() && who->second == uid) {
                    it = chunk_seeders.erase(it);
                } else {
                    ++it;
                }
            }
        }
    }
}


// LEAVE <gid> <uid>
static std::string apply_leave(const std::vector<std::string>& op) {
    const std::string& uid = op[2];

    // I hold groups_mtx the whole time here, because an empty group gets erased from the map.
    std::lock_guard<std::mutex> lk(groups_mtx);

    auto it = groups.find(op[1]);
    if (it == groups.end()) {
        return "ACK";
    }

    auto g = it->second;
    std::lock_guard<std::mutex> glk(g->mtx);

    auto p = g->pos.find(uid);

    if (p != g->pos.end()) {
        // If uid was the owner, the next member in the list becomes the owner automatically.
        g->members.erase(p->second);
        g->pos.erase(p);
    }
    g->pending.erase(uid);

    remove_user_seeders(*g, uid);

    if (g->members.empty()) {
        groups.erase(it);
    }

    return "ACK";
}


// ADD_FILE <gid> <name> <owner_id>
static std::string apply_add_file(const std::vector<std::string>& op) {
    auto g = get_group(op[1]);
    if (!g) {
        return "ACK";
    }

    std::lock_guard<std::mutex> lk(g->mtx);

    // If I already have it (maybe with metadata, on the owner/backup), I leave it alone.
    if (g->files.count(op[2]) == 0) {
        file_entry entry;
        entry.owner = (int)to_u64(op[3]);
        entry.meta  = nullptr;
        g->files[op[2]] = entry;
    }

    return "ACK";
}


std::string apply_change(const std::vector<std::string>& op) {
    if (op.empty()) {
        return "ACK";
    }

    const std::string& kind = op[0];

    if (kind == "CREATE_USER"  && op.size() == 4) return apply_create_user(op);
    if (kind == "CREATE_GROUP" && op.size() == 4) return apply_create_group(op);
    if (kind == "JOIN_REQ"     && op.size() == 3) return apply_join(op);
    if (kind == "ACCEPT"       && op.size() == 3) return apply_accept(op);
    if (kind == "LEAVE"        && op.size() == 3) return apply_leave(op);
    if (kind == "ADD_FILE"     && op.size() == 4) return apply_add_file(op);

    fprintf(stderr, "[tracker %d] unknown op %s\n", my_id, join(op, ' ').c_str());
    return "ACK";
}


std::string apply_op(int origin, uint64_t seq, const std::vector<std::string>& op, bool remote) {
    // An op I have already applied is skipped (they are idempotent anyway).
    //
    // TODO(design): if seq k+1 from an origin arrives before seq k, version jumps to k+1
    // and seq k is then skipped for good. DESIGN.md §4.3 uses this exact rule, so I keep it.
    if (remote) {
        std::lock_guard<std::mutex> lk(log_mtx);

        if (seq <= version[origin]) {
            return "ACK";
        }
    }

    std::string r = apply_change(op);

    // A create that lost is not logged and does not move version forward.
    if (r == "CONFLICT") {
        return r;
    }

    std::lock_guard<std::mutex> lk(log_mtx);

    oplog_append(origin, seq, join(op, ' '));

    if (seq > version[origin]) {
        version[origin] = seq;
    }

    return "ACK";
}


void local_op(const std::vector<std::string>& op) {
    uint64_t seq = take_seq();

    apply_op(my_id, seq, op, false);

    // No lock is held here: the sends can take up to SYNC_TIMEOUT per tracker.
    broadcast(seq, op);
}


// ---------------------------------------------------------------------------
// Sessions
// ---------------------------------------------------------------------------

std::string session_uid(int sock) {
    std::lock_guard<std::mutex> lk(session_mtx);

    auto it = sessions.find(sock);
    if (it == sessions.end()) {
        return "";
    }

    return it->second.first;
}


void end_session(int sock) {
    std::string peer;

    {
        std::lock_guard<std::mutex> lk(session_mtx);

        auto it = sessions.find(sock);
        if (it == sessions.end()) {
            return;
        }

        peer = it->second.second;
        sessions.erase(it);
    }

    // I keep peer_uid on purpose (DESIGN.md §5): seeder entries still point at this peer.
    {
        std::lock_guard<std::mutex> lk(peers_mtx);
        online.erase(peer);
    }

    // Every other tracker must stop handing this peer out too. No lock is held here.
    send_to_all("PEER_DOWN " + peer);
}


// ---------------------------------------------------------------------------
// Users
// ---------------------------------------------------------------------------

// If the user still has my id, I take my record back out (my create lost somewhere).
static void remove_user_if_mine(const std::string& uid, uint64_t id) {
    std::lock_guard<std::mutex> lk(users_mtx);

    auto it = users.find(uid);
    if (it != users.end() && it->second.id == id) {
        users.erase(it);
    }
}


// Blocking create, DESIGN.md §6.1.
std::string create_user(const std::vector<std::string>& a) {
    if (a.size() != 3) {
        return "ERR usage: create_user <uid> <pwd>";
    }

    const std::string& uid = a[1];

    {
        std::lock_guard<std::mutex> lk(users_mtx);
        if (users.count(uid)) {
            return "ERR user exists";
        }
    }

    uint64_t seq = take_seq();
    uint64_t id  = make_id(my_id, seq);

    std::vector<std::string> op = {"CREATE_USER", uid, a[2], std::to_string(id)};

    // Someone may have created the same name a moment ago (here or by BCAST).
    if (apply_op(my_id, seq, op, false) == "CONFLICT") {
        return "ERR user exists";
    }

    sleep_for_debug();

    // I wait for every tracker (or its timeout) before answering the client.
    std::vector<std::string> replies = broadcast(seq, op);

    if (any_conflict(replies)) {
        remove_user_if_mine(uid, id);
        return "ERR creation failed, try again";
    }

    return "OK";
}


std::string login(int sock, const std::vector<std::string>& a) {
    if (a.size() != 4) {
        return "ERR usage: login <uid> <pwd> <my_ip:my_port>";
    }

    const std::string& uid  = a[1];
    const std::string& pwd  = a[2];
    const std::string& peer = a[3];

    {
        std::lock_guard<std::mutex> lk(users_mtx);

        auto it = users.find(uid);
        if (it == users.end() || it->second.pwd != pwd) {
            return "ERR bad credentials";
        }
    }

    // One session per socket. A second device simply comes in on its own socket.
    {
        std::lock_guard<std::mutex> lk(session_mtx);

        if (sessions.count(sock)) {
            return "ERR already logged in";
        }

        sessions[sock] = {uid, peer};
    }

    {
        std::lock_guard<std::mutex> lk(peers_mtx);
        online.insert(peer);
        peer_uid[peer] = uid;
    }

    // The owner of a file may be another tracker, and it filters seeders by `online`,
    // so everyone has to know this peer is back.
    send_to_all("PEER_UP " + peer + " " + uid);

    return "OK";
}


std::string logout(int sock) {
    end_session(sock);
    return "OK";
}


// ---------------------------------------------------------------------------
// Groups
// ---------------------------------------------------------------------------

static void remove_group_if_mine(const std::string& gid, uint64_t id) {
    std::lock_guard<std::mutex> lk(groups_mtx);

    auto it = groups.find(gid);
    if (it != groups.end() && it->second->id == id) {
        groups.erase(it);
    }
}


// Blocking create, same steps as create_user.
std::string create_group(const std::string& uid, const std::vector<std::string>& a) {
    if (a.size() != 2) {
        return "ERR usage: create_group <gid>";
    }

    const std::string& gid = a[1];

    if (get_group(gid)) {
        return "ERR group exists";
    }

    uint64_t seq = take_seq();
    uint64_t id  = make_id(my_id, seq);

    std::vector<std::string> op = {"CREATE_GROUP", gid, std::to_string(id), uid};

    if (apply_op(my_id, seq, op, false) == "CONFLICT") {
        return "ERR group exists";
    }

    sleep_for_debug();

    std::vector<std::string> replies = broadcast(seq, op);

    if (any_conflict(replies)) {
        remove_group_if_mine(gid, id);
        return "ERR creation failed, try again";
    }

    return "OK";
}


std::string join_group(const std::string& uid, const std::vector<std::string>& a) {
    if (a.size() != 2) {
        return "ERR usage: join_group <gid>";
    }

    auto g = get_group(a[1]);
    if (!g) {
        return "ERR no group";
    }

    {
        std::lock_guard<std::mutex> lk(g->mtx);

        if (gone(*g)) {
            return "ERR no group";
        }
        if (is_member(*g, uid)) {
            return "ERR already member";
        }
    }

    // The lock is released before local_op, because it broadcasts.
    local_op({"JOIN_REQ", a[1], uid});
    return "OK request sent";
}


std::string leave_group(const std::string& uid, const std::vector<std::string>& a) {
    if (a.size() != 2) {
        return "ERR usage: leave_group <gid>";
    }

    auto g = get_group(a[1]);
    if (!g) {
        return "ERR not member";
    }

    {
        std::lock_guard<std::mutex> lk(g->mtx);

        if (gone(*g) || (!is_member(*g, uid) && g->pending.count(uid) == 0)) {
            return "ERR not member";
        }
    }

    local_op({"LEAVE", a[1], uid});
    return "OK";
}


std::string list_requests(const std::string& uid, const std::vector<std::string>& a) {
    if (a.size() != 2) {
        return "ERR usage: list_requests <gid>";
    }

    auto g = get_group(a[1]);
    if (!g) {
        return "ERR no group";
    }

    std::lock_guard<std::mutex> lk(g->mtx);

    if (gone(*g)) {
        return "ERR no group";
    }
    if (!is_owner(*g, uid)) {
        return "ERR not owner";
    }

    std::string out = "OK";
    for (const std::string& r : g->pending) {
        out += " " + r;
    }

    return out;
}


std::string accept_request(const std::string& uid, const std::vector<std::string>& a) {
    if (a.size() != 3) {
        return "ERR usage: accept_request <gid> <uid>";
    }

    const std::string& who = a[2];

    auto g = get_group(a[1]);
    if (!g) {
        return "ERR no group";
    }

    {
        std::lock_guard<std::mutex> lk(g->mtx);

        if (gone(*g)) {
            return "ERR no group";
        }
        if (!is_owner(*g, uid)) {
            return "ERR not owner";
        }
        if (!g->pending.count(who)) {
            return "ERR no such request";
        }
    }

    local_op({"ACCEPT", a[1], who});
    return "OK";
}


std::string list_groups(const std::vector<std::string>& a) {
    if (a.size() != 1) {
        return "ERR usage: list_groups";
    }

    // I copy the names out first, so groups_mtx is held for as short as possible.
    std::vector<std::string> ids;
    {
        std::lock_guard<std::mutex> lk(groups_mtx);
        for (auto& kv : groups) {
            ids.push_back(kv.first);
        }
    }

    std::sort(ids.begin(), ids.end());

    std::string out = "OK";
    for (const std::string& id : ids) {
        out += " " + id;
    }

    return out;
}


std::string list_files(const std::string& uid, const std::vector<std::string>& a) {
    if (a.size() != 2) {
        return "ERR usage: list_files <gid>";
    }

    auto g = get_group(a[1]);
    if (!g) {
        return "ERR no group";
    }

    std::lock_guard<std::mutex> lk(g->mtx);

    if (gone(*g)) {
        return "ERR no group";
    }
    if (!is_member(*g, uid)) {
        return "ERR not member";
    }

    std::string out = "OK";
    for (auto& kv : g->files) {
        out += " " + kv.first;
    }

    return out;
}


// ---------------------------------------------------------------------------
// Debug
// ---------------------------------------------------------------------------

std::string debug_delay(const std::vector<std::string>& a) {
    if (a.size() != 2) {
        return "ERR usage: debug_delay <ms>";
    }

    create_delay_ms = (int)to_u64(a[1]);
    return "OK delay " + std::to_string(create_delay_ms.load()) + " ms";
}


std::string debug_drop_next_bcast(const std::vector<std::string>& a) {
    if (a.size() != 1) {
        return "ERR usage: debug_drop_next_bcast";
    }

    drop_next_bcast = true;
    return "OK next BCAST will be dropped";
}
