#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "state.h"

// One function per control-plane command.
// `a` is the full token list, so a[0] is always the command name.
// Commands that need a login get the caller's uid (dispatch already checked the session).

std::string create_user(const std::vector<std::string>& a);
std::string login(int sock, const std::vector<std::string>& a);
std::string logout(int sock);

std::string create_group(const std::string& uid, const std::vector<std::string>& a);
std::string join_group(const std::string& uid, const std::vector<std::string>& a);
std::string leave_group(const std::string& uid, const std::vector<std::string>& a);
std::string list_requests(const std::string& uid, const std::vector<std::string>& a);
std::string accept_request(const std::string& uid, const std::vector<std::string>& a);
std::string list_groups(const std::vector<std::string>& a);
std::string list_files(const std::string& uid, const std::vector<std::string>& a);

std::string debug_delay(const std::vector<std::string>& a);   // debug_delay <ms>: creates sleep before BCAST
std::string debug_drop_next_bcast(const std::vector<std::string>& a);   // the next BCAST is never sent


// Replicated ops (DESIGN.md §4.3), e.g. {"JOIN_REQ", "g1", "b"}.
//
// apply_change: change my state only. "ACK", or "CONFLICT" for a create that loses.
// apply_op:     apply_change + op log + version, for one op with a known origin and seq.
//               remote = true for BCAST / SYNC: an op I have already seen is skipped.
// local_op:     a change made here: new seq, apply_op, then BCAST to everyone (replies ignored).
std::string apply_change(const std::vector<std::string>& op);
std::string apply_op(int origin, uint64_t seq, const std::vector<std::string>& op, bool remote);
void local_op(const std::vector<std::string>& op);


// Session helpers.
std::string session_uid(int sock);   // "" if this socket is not logged in
void end_session(int sock);          // erase session + mark peer offline (used by logout and cleanup)


// Group helpers, also used by data.cpp.
std::shared_ptr<group_rec> get_group(const std::string& gid);   // nullptr if no such group
bool gone(const group_rec& g);                                  // caller holds g.mtx
bool is_member(const group_rec& g, const std::string& uid);     // caller holds g.mtx
