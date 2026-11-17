#include "heartbeat.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <thread>

#include "../common/config.h"
#include "../common/util.h"
#include "control.h"
#include "data.h"
#include "oplog.h"
#include "state.h"
#include "sync.h"


// Master = tracker_order.front() (oldest live tracker by join time). A restarted tracker rejoins at the back, so an old master never takes over again.
// Limitation: assumes crash-stop failures and timeout-based detection; helpers may also be slow/partitioned. No consensus by design.


// ---------------------------------------------------------------------------
// Versions
// ---------------------------------------------------------------------------

// My version vector as "v0,v1,v2".
static std::string my_versions() {
    std::lock_guard<std::mutex> lk(log_mtx);

    std::vector<std::string> parts;
    for (uint64_t v : version) {
        parts.push_back(std::to_string(v));
    }

    return join(parts, ',');
}


// "v0,v1,v2" -> numbers. False if it doesn't have exactly N entries.
static bool parse_versions(const std::string& s, std::vector<uint64_t>& out) {
    std::vector<std::string> parts = split(s, ',');
    if ((int)parts.size() != N) {
        return false;
    }

    try {
        for (const std::string& p : parts) {
            out.push_back(std::stoull(p));
        }
    } catch (...) {
        return false;
    }

    return true;
}


static uint64_t my_version_of(int origin) {
    std::lock_guard<std::mutex> lk(log_mtx);
    return version[origin];
}


// ---------------------------------------------------------------------------
// Catch-up
// ---------------------------------------------------------------------------

// Asks tracker `from` for the ops of `origin` that I'm missing and applies them in order.
// Returns how many ops it applied.
static int sync_origin(int from, int origin) {
    uint64_t want = my_version_of(origin) + 1;

    std::string reply = send_to(from, "SYNC " + std::to_string(origin) + " " + std::to_string(want));

    std::vector<std::string> lines = split(reply, '\n');
    if (lines.empty() || lines[0] != "OPS") {
        return 0;
    }

    int applied = 0;

    for (size_t k = 1; k < lines.size(); k++) {
        std::vector<std::string> parts = split(lines[k]);
        if (parts.size() < 3) {
            continue;
        }

        uint64_t seq;
        try {
            seq = std::stoull(parts[1]);
        } catch (...) {
            continue;
        }

        std::vector<std::string> op(parts.begin() + 2, parts.end());

        // Same apply function as a BCAST.
        apply_op(origin, seq, op, true);
        applied++;

        // A create that lost (CONFLICT) is not logged and doesn't move version.
        // I move it here anyway, or I would ask for the same op on every heartbeat.
        std::lock_guard<std::mutex> lk(log_mtx);
        if (seq > version[origin]) {
            version[origin] = seq;
        }
    }

    return applied;
}


// For every origin where `from` is ahead of me, fetch what I'm missing.
static int catch_up(int from, const std::vector<uint64_t>& theirs) {
    int applied = 0;

    for (int origin = 0; origin < N; origin++) {
        if (theirs[origin] > my_version_of(origin)) {
            applied += sync_origin(from, origin);
        }
    }

    if (applied > 0) {
        fprintf(stderr, "[tracker %d] caught up: %d ops from tracker %d\n", my_id, applied, from);
    }

    return applied;
}


// ---------------------------------------------------------------------------
// The seniority list (every order_* helper: caller holds cluster_mtx)
// ---------------------------------------------------------------------------

static void order_remove(int id) {
    auto it = tracker_pos.find(id);
    if (it == tracker_pos.end()) {
        return;
    }

    tracker_order.erase(it->second);
    tracker_pos.erase(it);
}


// Puts `id` at the back. If it is already somewhere in the list, I move it.
static void order_push_back(int id) {
    order_remove(id);

    tracker_order.push_back(id);
    tracker_pos[id] = std::prev(tracker_order.end());
}


static void order_replace(const std::vector<int>& ids, int ver) {
    tracker_order.clear();
    tracker_pos.clear();

    for (int id : ids) {
        order_push_back(id);
    }

    tracker_order_version = ver;
}


// The list as "1,2,0".
static std::string order_string() {
    std::vector<std::string> parts;
    for (int id : tracker_order) {
        parts.push_back(std::to_string(id));
    }

    return join(parts, ',');
}


// -1 while I have no list yet.
static int master_locked() {
    if (tracker_order.empty()) {
        return -1;
    }

    return tracker_order.front();
}


static int current_master() {
    std::lock_guard<std::mutex> lk(cluster_mtx);
    return master_locked();
}


// "1,2,0" -> ids. False if it is empty or has an id outside 0..N-1.
static bool parse_order(const std::string& s, std::vector<int>& out) {
    try {
        for (const std::string& p : split(s, ',')) {
            int id = std::stoi(p);
            if (id < 0 || id >= N) {
                return false;
            }
            out.push_back(id);
        }
    } catch (...) {
        return false;
    }

    return !out.empty();
}


// ---------------------------------------------------------------------------
// Test switch: P2P_DROP_HB_TO
// ---------------------------------------------------------------------------

static bool debug = false;
static int drop_hb_to = -1;
static int drop_hb_count = HB_MISS;
static int dropped = 0;


void read_test_switches() {
    debug = (getenv("P2P_DEBUG") != nullptr);

    const char* to = getenv("P2P_DROP_HB_TO");
    if (to != nullptr) {
        drop_hb_to = atoi(to);
    }

    const char* count = getenv("P2P_DROP_HB_COUNT");
    if (count != nullptr) {
        drop_hb_count = atoi(count);
    }
}


// Test only. True means: don't send this heartbeat, count it as a timeout.
// Only the role thread calls this, so `dropped` needs no lock.
static bool drop_heartbeat(int to) {
    if (to != drop_hb_to || dropped >= drop_hb_count) {
        return false;
    }

    dropped++;

    if (debug) {
        fprintf(stderr, "[test] dropped heartbeat to tracker %d (%d/%d)\n", to, dropped, drop_hb_count);
    }

    return true;
}


// ---------------------------------------------------------------------------
// Heartbeat
// ---------------------------------------------------------------------------

// One heartbeat to `to`. On a valid HB_ACK I fill in its versions and return true.
// HB_ACK <hb_seq> <v0,v1,...> <order_version> <id,id,...>
//
// join = true for my first HB to a master I just found. It sends hb_seq 0, which puts
// me at the back of the master's list, and I take the master's list whatever its version.
// Design: a join is an HB with hb_seq 0 (normal HBs start at 1). That's how the master tells
// a rejoin from a normal HB: a tracker it hasn't removed yet must still move to the back.
static bool send_hb(int to, uint64_t hb_seq, std::vector<uint64_t>& theirs, bool join) {
    std::string msg = "HB " + std::to_string(my_id) + " " + std::to_string(hb_seq) + " " + my_versions();
    std::string reply = send_to(to, msg);

    // It must answer this very heartbeat.
    std::vector<std::string> a = split(reply);
    if (a.size() != 5 || a[0] != "HB_ACK" || a[1] != std::to_string(hb_seq)) {
        return false;
    }

    int ver;
    std::vector<int> ids;
    try {
        ver = std::stoi(a[3]);
    } catch (...) {
        return false;
    }

    if (!parse_versions(a[2], theirs) || !parse_order(a[4], ids)) {
        return false;
    }

    std::lock_guard<std::mutex> lk(cluster_mtx);

    // The master's list is the true one. I take it when it is newer than mine.
    if (join || ver > tracker_order_version) {
        order_replace(ids, ver);
        fprintf(stderr, "[tracker %d] tracker order is now %s (version %d)\n", my_id, order_string().c_str(), ver);
    }

    return true;
}


// A new master gives everyone in its list a full HB_MISS heartbeats before removing them.
static void become_master() {
    std::lock_guard<std::mutex> lk(cluster_mtx);

    long long now = now_ms();
    for (int id = 0; id < N; id++) {
        last_seen[id] = now;
    }

    fprintf(stderr, "[tracker %d] I am the master, order %s\n", my_id, order_string().c_str());
}


// Master: anyone silent for more than HB_MISS heartbeats leaves the list.
static void master_check() {
    std::lock_guard<std::mutex> lk(cluster_mtx);

    long long now = now_ms();
    long long limit = (long long)HB_MISS * HB_INTERVAL * 1000;

    // I collect first and remove after, so I don't erase from the list I'm walking.
    std::vector<int> dead;
    for (int id : tracker_order) {
        if (id != my_id && now - last_seen[id] > limit) {
            dead.push_back(id);
        }
    }

    for (int id : dead) {
        order_remove(id);
        tracker_order_version++;

        fprintf(stderr, "[tracker %d] tracker %d missed %d heartbeats, removed; order %s (version %d)\n",
                my_id, id, HB_MISS, order_string().c_str(), tracker_order_version);
    }
}


// ---------------------------------------------------------------------------
// Indirect ping (SWIM-style)
// ---------------------------------------------------------------------------

// Up to INDIRECT_K trackers from my list, oldest first, leaving out the master and me.
static std::vector<int> pick_helpers(int master) {
    std::lock_guard<std::mutex> lk(cluster_mtx);

    std::vector<int> helpers;
    for (int id : tracker_order) {
        if (id == master || id == my_id) {
            continue;
        }
        if ((int)helpers.size() == INDIRECT_K) {
            break;
        }
        helpers.push_back(id);
    }

    return helpers;
}


// One helper thread: the helper pings the master itself, so I give it PING_TIMEOUT on top of my own.
static void ask_helper(int helper, std::string msg, std::string* out) {
    *out = send_to(helper, msg, PING_TIMEOUT + SYNC_TIMEOUT);
}


// After HB_MISS misses I ask helpers to ping the master right now.
// True = the master is dead. False = the problem is on my side (my link, or I'm cut off).
static bool master_is_dead(int master) {
    std::vector<int> helpers = pick_helpers(master);

    if (helpers.empty()) {
        // Limitation (N=2): with no helpers, a slave cannot tell master failure from its own isolation; it promotes itself, which can briefly create two masters.
        fprintf(stderr, "[tracker %d] no helpers to ask, I treat master %d as dead\n", my_id, master);
        return true;
    }

    std::string msg = "PING_REQ " + std::to_string(master);

    // All helpers at once, each thread writes only its own slot.
    std::vector<std::string> replies(helpers.size());
    std::vector<std::thread> threads;

    for (size_t k = 0; k < helpers.size(); k++) {
        threads.emplace_back(ask_helper, helpers[k], msg, &replies[k]);
    }
    for (std::thread& t : threads) {
        t.join();
    }

    int reachable = 0;
    int unreachable = 0;

    for (const std::string& r : replies) {
        if (r == "REACHABLE") {
            reachable++;
        } else if (r == "UNREACHABLE") {
            unreachable++;
        }
    }

    int silent = (int)helpers.size() - reachable - unreachable;
    fprintf(stderr, "[tracker %d] PING_REQ %d to %d helpers: %d reachable, %d unreachable, %d no reply\n",
            my_id, master, (int)helpers.size(), reachable, unreachable, silent);

    // Someone can still reach the master, so it's me. And if nobody answers at all, I'm the one cut off.
    if (reachable > 0) {
        return false;
    }

    return unreachable > 0;
}


// ---------------------------------------------------------------------------
// Finding the master, recovery, startup
// ---------------------------------------------------------------------------

// WHO_IS_MASTER to every other tracker in tracker_info.txt.
// Returns the first usable master id, or -1. `answered` is true if anyone replied MASTER at all.
static int find_master(bool& answered) {
    answered = false;

    std::vector<std::string> replies = send_to_all("WHO_IS_MASTER");

    for (int id = 0; id < N; id++) {
        std::vector<std::string> a = split(replies[id]);
        if (a.size() != 2 || a[0] != "MASTER") {
            continue;
        }

        int m;
        try {
            m = std::stoi(a[1]);
        } catch (...) {
            continue;
        }

        answered = true;

        // Design: they still name me, so they haven't noticed I was down. I never join myself;
        // I keep asking until they drop me (I don't answer PING while recovering, so they will).
        if (m < 0 || m >= N || m == my_id) {
            continue;
        }

        return m;
    }

    return -1;
}


// PULL_DATA to the master. The online set comes back as "ONLINE <peer> <uid>" lines.
static bool pull_from_master(int master) {
    std::vector<std::string> lines = split(send_to(master, "PULL_DATA"), '\n');
    if (lines.empty() || lines[0] != "DATA") {
        return false;
    }

    int count = 0;
    {
        // The master's set replaces mine, so peers that logged out while I was gone disappear.
        std::lock_guard<std::mutex> lk(peers_mtx);
        online.clear();

        for (size_t k = 1; k < lines.size(); k++) {
            std::vector<std::string> a = split(lines[k]);
            if (a.size() != 3 || a[0] != "ONLINE") {
                continue;
            }

            online.insert(a[1]);
            peer_uid[a[1]] = a[2];
            count++;
        }
    }

    fprintf(stderr, "[tracker %d] PULL_DATA from master %d: %d online peers\n", my_id, master, count);
    return true;
}


// Join `master` at the back of its list, catch up on ops, then pull the online set.
static bool rejoin(int master) {
    std::vector<uint64_t> theirs;

    if (!send_hb(master, 0, theirs, true)) {
        fprintf(stderr, "[tracker %d] join to master %d failed\n", my_id, master);
        return false;
    }

    // Control ops: my own log was replayed at startup, SYNC brings the rest from the master.
    catch_up(master, theirs);

    // The neighbour metadata pull used to be here.
    // TODO(design): data-plane file metadata owned by this tracker is NOT recovered after a restart; recovery gets only cluster-wide state from the master. Files it owned stay unavailable until this is designed.
    // A restarted replica also loses its copy, so RF=2 protection for those files is lost until metadata recovery is designed.
    // Seeders not recovered: only the owner reads them, and owned-file metadata recovery is deferred.
    return pull_from_master(master);
}


// Nobody answered WHO_IS_MASTER. `ids` is the new list and I am ACTIVE right away.
static void become_alone(const std::vector<int>& ids, int ver, const char* why) {
    {
        std::lock_guard<std::mutex> lk(cluster_mtx);
        order_replace(ids, ver);

        fprintf(stderr, "[tracker %d] %s, order %s\n", my_id, why, order_string().c_str());
    }

    my_state = ACTIVE;
}


// While in RECOVERY I talk only to the master. I ask everyone who the master is (never just
// the old one, it may have died or changed while I was cut off), every HB_INTERVAL, until I get in.
// at_boot = true only at startup: there, a first round with no answer at all is a cold start.
static void recover(bool at_boot) {
    int silent_rounds = 0;

    while (true) {
        bool answered;
        int master = find_master(answered);

        if (!answered && at_boot) {
            // Assumption: at cold start, trackers are launched in tracker_info.txt order.
            std::vector<int> ids;
            for (const tracker_addr& t : trackers) {
                ids.push_back(t.id);
            }

            become_alone(ids, 1, "cold start");
            return;
        }

        if (at_boot) {
            fprintf(stderr, "[tracker %d] in recovery mode\n", my_id);
            at_boot = false;
        }

        if (master >= 0) {
            if (debug) {
                fprintf(stderr, "[tracker %d] recovery: WHO_IS_MASTER says %d\n", my_id, master);
            }

            if (rejoin(master)) {
                my_state = ACTIVE;
                fprintf(stderr, "[tracker %d] rejoined master %d, ACTIVE\n", my_id, master);
                return;
            }
        }

        // Only a round where nobody said MASTER counts. "ERR recovering" is the same as no reply.
        if (answered) {
            silent_rounds = 0;
        } else {
            silent_rounds++;
        }

        if (silent_rounds >= HB_MISS) {
            // No tracker answered: under crash-stop, all others are down, so become master alone (same rule as cold start). Limitation: a truly isolated tracker could become a second master; partitions are out of scope.
            int ver;
            {
                std::lock_guard<std::mutex> lk(cluster_mtx);
                ver = tracker_order_version + 1;
            }

            become_alone({my_id}, ver, "nobody answered WHO_IS_MASTER, I am master alone");
            return;
        }

        std::this_thread::sleep_for(std::chrono::seconds(HB_INTERVAL));
    }
}


// One WHO_IS_MASTER path for everything: no answer at all is a cold start, otherwise I rejoin.
void startup() {
    recover(true);
}


// ---------------------------------------------------------------------------
// The role thread
// ---------------------------------------------------------------------------

// Limitation: assumes crash-stop failures and timeout-based detection; a slave that suspects the master too early can briefly diverge. No consensus by design.
static void role_loop() {
    uint64_t hb_seq = 0;
    int missed = 0;
    bool acting_master = false;

    while (true) {
        std::this_thread::sleep_for(std::chrono::seconds(HB_INTERVAL));

        if (my_state == RECOVERY) {
            fprintf(stderr, "[tracker %d] in recovery mode\n", my_id);
            recover(false);
            missed = 0;
            acting_master = false;
            continue;
        }

        int master = current_master();

        if (master == my_id) {
            if (!acting_master) {
                become_master();
                acting_master = true;
            }

            master_check();
            continue;
        }

        acting_master = false;
        hb_seq++;

        std::vector<uint64_t> theirs;
        bool ok = false;

        if (!drop_heartbeat(master)) {
            ok = send_hb(master, hb_seq, theirs, false);
        }

        if (ok) {
            missed = 0;
            catch_up(master, theirs);
            continue;
        }

        missed++;
        fprintf(stderr, "[tracker %d] no HB_ACK from master %d (%d/%d)\n", my_id, master, missed, HB_MISS);

        if (missed < HB_MISS) {
            continue;
        }

        missed = 0;

        if (master_is_dead(master)) {
            // Every slave that sees the same death removes the same front, so the lists stay equal.
            std::lock_guard<std::mutex> lk(cluster_mtx);
            order_remove(master);
            tracker_order_version++;

            fprintf(stderr, "[tracker %d] master %d is dead, new master %d, order %s\n",
                    my_id, master, master_locked(), order_string().c_str());
        } else {
            fprintf(stderr, "[tracker %d] master %d not confirmed dead (reachable, or I am cut off), going to recovery\n", my_id, master);
            my_state = RECOVERY;
        }
    }
}


void start_heartbeat() {
    std::thread t(role_loop);
    t.detach();
}


// ---------------------------------------------------------------------------
// Handlers
// ---------------------------------------------------------------------------

std::string on_hb(const std::vector<std::string>& a) {
    if (a.size() != 4) {
        return "ERR bad HB";
    }

    int id = -1;
    try {
        id = std::stoi(a[1]);
    } catch (...) {
    }

    if (id < 0 || id >= N) {
        return "ERR bad HB";
    }

    int ver;
    std::string order;
    {
        std::lock_guard<std::mutex> lk(cluster_mtx);

        // Only the master answers heartbeats. A slave that still sends to an old master
        // counts this as a miss.
        if (my_state == RECOVERY || master_locked() != my_id) {
            return "ERR not master";
        }

        // A joining tracker (hb_seq 0) goes to the back. So does one I had already removed.
        bool join = (a[2] == "0");
        if (join || tracker_pos.count(id) == 0) {
            order_push_back(id);
            tracker_order_version++;

            fprintf(stderr, "[tracker %d] tracker %d joined at the back, order %s (version %d)\n",
                    my_id, id, order_string().c_str(), tracker_order_version);
        }

        last_seen[id] = now_ms();

        ver = tracker_order_version;
        order = order_string();
    }

    return "HB_ACK " + a[2] + " " + my_versions() + " " + std::to_string(ver) + " " + order;
}


// If the slave that just sent me an HB knows ops I don't, I pull them from it.
// This runs after the HB_ACK has been sent, so the slave isn't kept waiting.
// It also covers a newly promoted master that missed a broadcast.
void after_hb(const std::vector<std::string>& a) {
    if (a.size() != 4) {
        return;
    }

    int id;
    try {
        id = std::stoi(a[1]);
    } catch (...) {
        return;
    }

    std::vector<uint64_t> theirs;
    if (id < 0 || id >= N || id == my_id || !parse_versions(a[3], theirs)) {
        return;
    }

    catch_up(id, theirs);
}


std::string on_sync(const std::vector<std::string>& a) {
    if (a.size() != 3) {
        return "ERR bad SYNC";
    }

    int origin;
    uint64_t from_seq;

    try {
        origin   = std::stoi(a[1]);
        from_seq = std::stoull(a[2]);
    } catch (...) {
        return "ERR bad SYNC";
    }

    std::string out = "OPS";
    for (const std::string& line : oplog_read_from(origin, from_seq)) {
        out += "\n" + line;
    }

    return out;
}


// PULL_DATA from a recovering tracker. Only the master answers, with the online set:
// "DATA" then one "ONLINE <peer> <uid>" line per peer (same shape as PEER_UP).
// File names per group already come through SYNC (ADD_FILE), and `sessions` stays local.
std::string on_pull_data(const std::vector<std::string>& a) {
    if (a.size() != 1) {
        return "ERR bad PULL_DATA";
    }

    if (current_master() != my_id) {
        return "ERR not master";
    }

    std::string out = "DATA";

    std::lock_guard<std::mutex> lk(peers_mtx);

    for (const std::string& peer : online) {
        auto who = peer_uid.find(peer);
        if (who == peer_uid.end()) {
            continue;
        }

        out += "\nONLINE " + peer + " " + who->second;
    }

    return out;
}


// Design: a recovering tracker doesn't answer PONG (and not WHO_IS_MASTER either). Otherwise
// helpers would call a restarted old master REACHABLE, every slave would think the fault is
// its own and go into recovery, and nobody would be left to rejoin.
std::string on_ping(const std::vector<std::string>& a) {
    (void)a;

    if (my_state == RECOVERY) {
        return "ERR recovering";
    }

    return "PONG";
}


// PING_REQ <master_id>: I ping it once myself. I don't touch my own list or counters.
std::string on_ping_req(const std::vector<std::string>& a) {
    if (a.size() != 2) {
        return "ERR bad PING_REQ";
    }

    int id = -1;
    try {
        id = std::stoi(a[1]);
    } catch (...) {
    }

    if (id < 0 || id >= N) {
        return "ERR bad PING_REQ";
    }

    if (send_to(id, "PING", PING_TIMEOUT) == "PONG") {
        return "REACHABLE";
    }

    return "UNREACHABLE";
}


// WHO_IS_MASTER -> MASTER <id>. A recovering tracker has no view worth sharing yet.
std::string on_who_is_master(const std::vector<std::string>& a) {
    (void)a;

    if (my_state == RECOVERY) {
        return "ERR recovering";
    }

    int m = current_master();
    if (m < 0) {
        return "ERR no master";
    }

    return "MASTER " + std::to_string(m);
}
