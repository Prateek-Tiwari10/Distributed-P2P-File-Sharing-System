#include "util.h"

#include <chrono>
#include <cstdio>
#include <fstream>
#include <openssl/evp.h>
#include <openssl/sha.h>
#include <sstream>


// ---------------------------------------------------------------------------
// Strings
// ---------------------------------------------------------------------------

// I walk the string once and cut at every `sep`. Empty pieces are skipped,
// so "a  b" gives {"a", "b"} and not {"a", "", "b"}.
std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> out;
    std::string cur;

    for (char c : s) {
        if (c != sep) {
            cur += c;
            continue;
        }

        if (!cur.empty()) {
            out.push_back(cur);
        }
        cur.clear();
    }

    if (!cur.empty()) {
        out.push_back(cur);
    }

    return out;
}


std::string join(const std::vector<std::string>& v, char sep) {
    std::string out;

    for (size_t i = 0; i < v.size(); i++) {
        if (i > 0) {
            out += sep;
        }
        out += v[i];
    }

    return out;
}


// ---------------------------------------------------------------------------
// SHA1
// ---------------------------------------------------------------------------

// Each byte becomes two lowercase hex characters.
static std::string to_hex(const unsigned char* md, size_t n) {
    const char* digits = "0123456789abcdef";
    std::string out;

    for (size_t i = 0; i < n; i++) {
        out += digits[md[i] >> 4];    // high 4 bits
        out += digits[md[i] & 15];    // low 4 bits
    }

    return out;
}


std::string sha1_hex(const char* data, size_t n) {
    unsigned char md[SHA_DIGEST_LENGTH];
    SHA1((const unsigned char*)data, n, md);

    return to_hex(md, SHA_DIGEST_LENGTH);
}


// I read the file 1 MB at a time so even a huge file never sits in memory.
// I use the EVP calls because SHA1_Init/Update/Final are deprecated in OpenSSL 3
// and would give warnings with -Wall.
std::string sha1_file(const std::string& path) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) {
        return "";
    }

    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    EVP_DigestInit_ex(ctx, EVP_sha1(), nullptr);

    std::vector<char> buf(1 << 20);

    while (true) {
        size_t n = fread(buf.data(), 1, buf.size(), f);
        if (n == 0) {
            break;
        }
        EVP_DigestUpdate(ctx, buf.data(), n);
    }

    bool ok = !ferror(f);
    fclose(f);

    unsigned char md[EVP_MAX_MD_SIZE];
    unsigned int len = 0;
    EVP_DigestFinal_ex(ctx, md, &len);
    EVP_MD_CTX_free(ctx);

    if (!ok) {
        return "";
    }

    return to_hex(md, len);
}


// ---------------------------------------------------------------------------
// Config and time
// ---------------------------------------------------------------------------

// Each line is "id ip client_port sync_port". I just skip lines that don't parse.
std::vector<tracker_addr> read_config(const std::string& path) {
    std::vector<tracker_addr> out;

    std::ifstream in(path);
    std::string line;

    while (std::getline(in, line)) {
        std::istringstream iss(line);
        tracker_addr t;

        if (iss >> t.id >> t.ip >> t.client_port >> t.sync_port) {
            out.push_back(t);
        }
    }

    return out;
}


// steady_clock never jumps backwards, which is what I want for timeouts.
long long now_ms() {
    auto since_start = std::chrono::steady_clock::now().time_since_epoch();

    return std::chrono::duration_cast<std::chrono::milliseconds>(since_start).count();
}
