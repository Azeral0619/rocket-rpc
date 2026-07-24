#include "rocket/common/etcd_registry.h"
#include "rocket/common/log.h"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <sstream>
#include <string_view>

namespace rocket {

// ── Base64 ────────────────────────────────────────────────────────────

static const char kBase64Table[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string EtcdRegistry::base64Encode(std::string_view data) {
    std::string out;
    out.reserve((data.size() + 2) / 3 * 4);
    for (size_t i = 0; i < data.size(); i += 3) {
        unsigned char b0 = static_cast<unsigned char>(data[i]);
        unsigned char b1 = (i + 1 < data.size()) ? static_cast<unsigned char>(data[i + 1]) : 0;
        unsigned char b2 = (i + 2 < data.size()) ? static_cast<unsigned char>(data[i + 2]) : 0;
        out.push_back(kBase64Table[b0 >> 2]);
        out.push_back(kBase64Table[((b0 & 3) << 4) | (b1 >> 4)]);
        out.push_back((i + 1 < data.size()) ? kBase64Table[((b1 & 15) << 2) | (b2 >> 6)] : '=');
        out.push_back((i + 2 < data.size()) ? kBase64Table[b2 & 63] : '=');
    }
    return out;
}

std::string EtcdRegistry::base64Decode(std::string_view data) {
    static int decode_table[256] = {};
    static bool init = false;
    if (!init) {
        for (int i = 0; i < 64; ++i) decode_table[static_cast<unsigned char>(kBase64Table[i])] = i;
        init = true;
    }
    std::string out;
    out.reserve(data.size() * 3 / 4);
    for (size_t i = 0; i < data.size(); i += 4) {
        unsigned char b0 = (i < data.size()) ? static_cast<unsigned char>(data[i]) : 'A';
        unsigned char b1 = (i + 1 < data.size()) ? static_cast<unsigned char>(data[i + 1]) : 'A';
        unsigned char b2 = (i + 2 < data.size()) ? static_cast<unsigned char>(data[i + 2]) : 'A';
        unsigned char b3 = (i + 3 < data.size()) ? static_cast<unsigned char>(data[i + 3]) : 'A';
        unsigned char v0 = decode_table[b0], v1 = decode_table[b1];
        unsigned char v2 = decode_table[b2], v3 = decode_table[b3];
        out.push_back(static_cast<char>((v0 << 2) | (v1 >> 4)));
        if (b2 != '=') out.push_back(static_cast<char>((v1 << 4) | (v2 >> 2)));
        if (b3 != '=') out.push_back(static_cast<char>((v2 << 6) | v3));
    }
    return out;
}

std::string EtcdRegistry::base64RangeEnd(std::string_view key) {
    // etcd range [key, end) — increment last byte to cover all keys with this prefix.
    std::string end(key);
    for (int i = static_cast<int>(end.size()) - 1; i >= 0; --i) {
        auto& b = reinterpret_cast<unsigned char&>(end[i]);
        if (b < 0xff) { b++; end.resize(static_cast<size_t>(i + 1)); return base64Encode(end); }
    }
    // Key is all 0xff — range end is \x00.
    return base64Encode(std::string_view("\0", 1));
}

// ── Minimal JSON helpers ──────────────────────────────────────────────

std::string EtcdRegistry::jsonField(std::string_view json, std::string_view key) {
    std::string pat = std::string("\"") + std::string(key) + std::string("\"");
    auto pos = json.find(pat);
    if (pos == std::string_view::npos) return "";
    pos = json.find(':', pos + pat.size());
    if (pos == std::string_view::npos) return "";
    // Skip ':' and whitespace
    pos++;
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t' || json[pos] == '\n')) pos++;
    if (pos >= json.size()) return "";
    if (json[pos] == '"') {
        // String value
        pos++;
        auto end = json.find('"', pos);
        if (end == std::string_view::npos) return "";
        return std::string(json.substr(pos, end - pos));
    }
    // Number value
    auto end = pos;
    while (end < json.size() && (json[end] >= '0' && json[end] <= '9' || json[end] == '-')) end++;
    return std::string(json.substr(pos, end - pos));
}

// ── HTTP POST ─────────────────────────────────────────────────────────

std::string EtcdRegistry::httpPost(std::string_view path, std::string_view body) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return "";

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(m_port));
    ::inet_pton(AF_INET, m_host.c_str(), &addr.sin_addr);

    if (::connect(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        ::close(fd);
        return "";
    }

    std::ostringstream req;
    req << "POST " << path << " HTTP/1.1\r\n"
        << "Host: " << m_host << ":" << m_port << "\r\n"
        << "Content-Type: application/json\r\n"
        << "Content-Length: " << body.size() << "\r\n"
        << "Connection: close\r\n"
        << "\r\n"
        << body;

    std::string req_str = req.str();
    ::write(fd, req_str.data(), req_str.size());

    char buf[4096];
    std::string response;
    ssize_t n;
    while ((n = ::read(fd, buf, sizeof(buf))) > 0)
        response.append(buf, static_cast<size_t>(n));
    ::close(fd);

    // Find body (after \r\n\r\n)
    auto hdr_end = response.find("\r\n\r\n");
    if (hdr_end == std::string::npos) return "";
    return std::string(response.substr(hdr_end + 4));
}

// ── Constructor / Destructor ────────────────────────────────────────

EtcdRegistry::EtcdRegistry(std::string endpoint) {
    // Parse "http://host:port"
    if (endpoint.starts_with("http://"))
        endpoint = endpoint.substr(7);
    auto colon = endpoint.find(':');
    if (colon != std::string::npos) {
        m_host = endpoint.substr(0, colon);
        m_port = std::stoi(endpoint.substr(colon + 1));
    } else {
        m_host = endpoint;
    }
    m_endpoint = endpoint;
}

EtcdRegistry::~EtcdRegistry() {
    m_lease_running.store(false);
    if (m_lease_thread.joinable()) m_lease_thread.join();
}

// ── Server-side ──────────────────────────────────────────────────────

int64_t EtcdRegistry::registerService(std::string_view name,
                                       std::string_view addr,
                                       int ttl_seconds) {
    // 1. Grant lease
    std::string lease_req = "{\"TTL\":" + std::to_string(ttl_seconds) + ",\"ID\":0}";
    auto lease_rsp = httpPost("/v3/lease/grant", lease_req);
    if (lease_rsp.empty()) return -1;
    auto lease_str = jsonField(lease_rsp, "ID");
    if (lease_str.empty()) return -1;
    int64_t lease_id = std::stoll(lease_str);

    // 2. Put key with lease
    std::string key = "/services/" + std::string(name) + "/" + std::string(addr);
    std::string put_req = "{\"key\":\"" + base64Encode(key) +
                          "\",\"value\":\"e30=\","  // "{}" in base64
                          "\"lease\":" + std::to_string(lease_id) + "}";
    auto put_rsp = httpPost("/v3/kv/put", put_req);
    if (put_rsp.empty()) return -1;

    // Start background heartbeat if not already running
    if (!m_lease_running.exchange(true)) {
        m_lease_thread = std::jthread([this] { leaseLoop(); });
    }
    {
        std::lock_guard lk(m_lease_mutex);
        m_leases[key] = {lease_id, ttl_seconds};
    }

    ROCKET_LOG_INFO("etcd: registered {} at {} (lease={}, ttl={}s)", name, addr, lease_id, ttl_seconds);
    return lease_id;
}

void EtcdRegistry::heartbeat(int64_t lease_id) {
    std::string req = "{\"ID\":" + std::to_string(lease_id) + "}";
    httpPost("/v3/lease/keepalive", req);
}

void EtcdRegistry::deregister(std::string_view name, std::string_view addr,
                               int64_t lease_id) {
    // Revoke lease (keys auto-deleted)
    std::string req = "{\"ID\":" + std::to_string(lease_id) + "}";
    httpPost("/v3/lease/revoke", req);
    ROCKET_LOG_INFO("etcd: deregistered {} at {}", name, addr);
}

void EtcdRegistry::leaseLoop() {
    while (m_lease_running.load()) {
        std::this_thread::sleep_for(std::chrono::seconds(2));
        std::lock_guard lk(m_lease_mutex);
        for (auto& [key, entry] : m_leases)
            heartbeat(entry.lease_id);
    }
}

// ── Client-side ──────────────────────────────────────────────────────

ServiceRegistry::AddrList EtcdRegistry::discover(std::string_view name) {
    AddrList result;
    std::string prefix = "/services/" + std::string(name) + "/";
    std::string req = "{\"key\":\"" + base64Encode(prefix) +
                      "\",\"range_end\":\"" + base64RangeEnd(prefix) + "\"}";
    auto rsp = httpPost("/v3/kv/range", req);
    if (rsp.empty()) return result;

    // Parse kvs array: find all "key":"..." entries
    // Simple approach: split on "key\":\"" to find keys
    std::string_view sv(rsp);
    auto pos = sv.find("\"kvs\"");
    if (pos == std::string_view::npos) return result;

    while (true) {
        pos = sv.find("\"key\":\"", pos);
        if (pos == std::string_view::npos) break;
        pos += 7; // skip "key":"
        auto end = sv.find('"', pos);
        if (end == std::string_view::npos) break;
        auto key_enc = sv.substr(pos, end - pos);
        auto key = base64Decode(key_enc);
        // Extract addr: after prefix
        if (key.starts_with(prefix)) {
            auto addr_str = key.substr(prefix.size());
            result.push_back(std::make_shared<IPNetAddr>(addr_str));
        }
        pos = end + 1;
    }
    return result;
}

void EtcdRegistry::watch(std::string_view name, WatchCb /*cb*/) {
    // Watching requires long-poll / streaming HTTP, which is more involved.
    // For now, the caller can poll discover() periodically.
    (void)name;
}

} // namespace rocket
