#pragma once

void handle_client(int sock);   // one detached thread per connection
void cleanup(int sock);         // end the session and close the socket
