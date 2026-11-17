#include "state.h"

// The real definitions of everything declared `extern` in state.h.

std::unordered_map<std::string, user_rec> users;
std::mutex users_mtx;

std::unordered_map<int, std::pair<std::string, std::string>> sessions;
std::mutex session_mtx;

std::unordered_set<std::string> online;
std::unordered_map<std::string, std::string> peer_uid;
std::mutex peers_mtx;

std::unordered_map<std::string, std::shared_ptr<group_rec>> groups;
std::mutex groups_mtx;

std::vector<uint64_t> version;
uint64_t my_seq = 0;
std::mutex log_mtx;

int my_id, N;

std::list<int> tracker_order;
std::unordered_map<int, std::list<int>::iterator> tracker_pos;
int tracker_order_version = 0;

std::vector<long long> last_seen;
std::mutex cluster_mtx;

std::atomic<tracker_state> my_state(RECOVERY);

std::vector<tracker_addr> trackers;
