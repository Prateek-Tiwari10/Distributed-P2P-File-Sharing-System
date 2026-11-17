#pragma once

// The part of the client that sends chunks to other clients (DESIGN.md §3, §4.4).
// I start one listener thread and SERVE_THREADS serve threads, all detached.
// Returns false if I cannot listen on my port.
bool start_server(int port);
