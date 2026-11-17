#pragma once

#include <string>
#include <vector>

// Heartbeat, catch-up, master succession and recovery (DESIGN.md §6.7, §6.8).

void read_test_switches();  // P2P_DROP_HB_TO / P2P_DROP_HB_COUNT / P2P_DEBUG, once at startup
void startup();             // WHO_IS_MASTER: cold start (ACTIVE), or rejoin the master (RECOVERY -> ACTIVE)
void start_heartbeat();     // the role thread: master checks liveness, slave sends HB

// Message handlers, called from sync.cpp.
std::string on_hb(const std::vector<std::string>& a);              // HB <id> <hb_seq> <v0,v1,...>
void after_hb(const std::vector<std::string>& a);                  // run after the HB_ACK went out
std::string on_sync(const std::vector<std::string>& a);            // SYNC <origin> <from_seq>
std::string on_pull_data(const std::vector<std::string>& a);       // PULL_DATA
std::string on_ping(const std::vector<std::string>& a);            // PING
std::string on_ping_req(const std::vector<std::string>& a);        // PING_REQ <master_id>
std::string on_who_is_master(const std::vector<std::string>& a);   // WHO_IS_MASTER
