#include "vnc_proxy.h"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <filesystem>

#pragma comment(lib, "ws2_32.lib")

namespace fs = std::filesystem;

// ==================== WebSocket Frame Helpers ====================

// Minimal WebSocket frame parser/builder for the VNC proxy
// We only need to handle text/binary frames and pings/pongs

struct WSFrame {
    bool fin;
    uint8_t opcode;
    bool masked;
    uint64_t payloadLen;
    uint8_t maskKey[4];
    std::vector<uint8_t> payload;
};

static bool parseWSFrame(const uint8_t* data, size_t dataLen, WSFrame& frame, size_t& bytesConsumed) {
    if (dataLen < 2) return false;

    frame.fin = (data[0] & 0x80) != 0;
    frame.opcode = data[0] & 0x0F;
    frame.masked = (data[1] & 0x80) != 0;

    uint8_t lenByte = data[1] & 0x7F;
    size_t headerLen = 2;

    if (lenByte == 126) {
        if (dataLen < 4) return false;
        frame.payloadLen = (uint64_t(data[2]) << 8) | data[3];
        headerLen = 4;
    } else if (lenByte == 127) {
        if (dataLen < 10) return false;
        frame.payloadLen = 0;
        for (int i = 2; i < 10; ++i)
            frame.payloadLen = (frame.payloadLen << 8) | data[i];
        headerLen = 10;
    } else {
        frame.payloadLen = lenByte;
    }

    if (frame.masked) {
        if (dataLen < headerLen + 4) return false;
        memcpy(frame.maskKey, data + headerLen, 4);
        headerLen += 4;
    }

    if (dataLen < headerLen + frame.payloadLen) return false;

    frame.payload.resize((size_t)frame.payloadLen);
    if (frame.payloadLen > 0) {
        memcpy(frame.payload.data(), data + headerLen, (size_t)frame.payloadLen);
        if (frame.masked) {
            for (uint64_t i = 0; i < frame.payloadLen; ++i) {
                frame.payload[i] ^= frame.maskKey[i % 4];
            }
        }
    }

    bytesConsumed = headerLen + (size_t)frame.payloadLen;
    return true;
}

static std::vector<uint8_t> buildWSFrame(uint8_t opcode, const uint8_t* payload, size_t len, bool masked = false) {
    std::vector<uint8_t> frame;
    frame.push_back(0x80 | opcode); // fin=1

    if (len < 126) {
        frame.push_back((masked ? 0x80 : 0) | (uint8_t)len);
    } else if (len < 65536) {
        frame.push_back((masked ? 0x80 : 0) | 126);
        frame.push_back((len >> 8) & 0xFF);
        frame.push_back(len & 0xFF);
    } else {
        frame.push_back((masked ? 0x80 : 0) | 127);
        for (int i = 7; i >= 0; --i)
            frame.push_back((len >> (i * 8)) & 0xFF);
    }

    if (masked) {
        uint8_t mask[4] = {0, 0, 0, 0}; // zero mask for simplicity
        frame.insert(frame.end(), mask, mask + 4);
        for (size_t i = 0; i < len; ++i)
            frame.push_back(payload[i] ^ mask[i % 4]);
    } else {
        frame.insert(frame.end(), payload, payload + len);
    }
    return frame;
}

static std::string strToLower(const std::string& s) {
    std::string r = s;
    for (auto& c : r) c = (char)tolower((unsigned char)c);
    return r;
}

static void performWSHandshake(SOCKET client, const std::string& request) {
    // Case-insensitive search for upgrade header
    std::string lowerReq = strToLower(request);
    if (lowerReq.find("upgrade: websocket") == std::string::npos) {
        // Not a valid WebSocket upgrade
        return;
    }

    // Extract Sec-WebSocket-Key (case-insensitive header search)
    std::string key;
    std::istringstream reqStream(request);
    std::string line;
    while (std::getline(reqStream, line)) {
        std::string lowerLine = strToLower(line);
        if (lowerLine.find("sec-websocket-key:") != std::string::npos) {
            auto colonPos = line.find(':');
            if (colonPos != std::string::npos) {
                key = line.substr(colonPos + 1);
                // Trim whitespace
                while (!key.empty() && (key.front() == ' ' || key.front() == '\t')) key.erase(key.begin());
                while (!key.empty() && (key.back() == ' ' || key.back() == '\t' || key.back() == '\r')) key.pop_back();
            }
            break;
        }
    }

    // Check if client requested 'binary' sub-protocol
    bool binaryProtocol = (lowerReq.find("sec-websocket-protocol:") != std::string::npos &&
                           lowerReq.find("binary") != std::string::npos);

    // Concatenate with GUID and SHA-1
    key += "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

    // Simple SHA-1
    // We'll use a minimal SHA-1 implementation
    // SHA-1 constants and implementation
    uint32_t h0 = 0x67452301, h1 = 0xEFCDAB89, h2 = 0x98BADCFE, h3 = 0x10325476, h4 = 0xC3D2E1F0;

    std::vector<uint8_t> msg(key.begin(), key.end());
    uint64_t bitLen = msg.size() * 8;
    msg.push_back(0x80);
    while (msg.size() % 64 != 56) msg.push_back(0);
    for (int i = 7; i >= 0; --i) msg.push_back((bitLen >> (i * 8)) & 0xFF);

    for (size_t chunk = 0; chunk < msg.size(); chunk += 64) {
        uint32_t w[80];
        for (int i = 0; i < 16; ++i) {
            w[i] = (msg[chunk + i * 4] << 24) | (msg[chunk + i * 4 + 1] << 16) |
                    (msg[chunk + i * 4 + 2] << 8) | msg[chunk + i * 4 + 3];
        }
        for (int i = 16; i < 80; ++i) {
            uint32_t tmp = w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16];
            w[i] = (tmp << 1) | (tmp >> 31);
        }

        uint32_t a = h0, b = h1, c = h2, d = h3, e = h4;
        for (int i = 0; i < 80; ++i) {
            uint32_t f, k;
            if (i < 20) { f = (b & c) | (~b & d); k = 0x5A827999; }
            else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
            else { f = b ^ c ^ d; k = 0xCA62C1D6; }
            uint32_t temp = ((a << 5) | (a >> 27)) + f + e + k + w[i];
            e = d; d = c; c = (b << 30) | (b >> 2); b = a; a = temp;
        }
        h0 += a; h1 += b; h2 += c; h3 += d; h4 += e;
    }

    uint8_t hash[20];
    for (int i = 0; i < 4; ++i) {
        hash[i] = (h0 >> (24 - i * 8)) & 0xFF;
        hash[4 + i] = (h1 >> (24 - i * 8)) & 0xFF;
        hash[8 + i] = (h2 >> (24 - i * 8)) & 0xFF;
        hash[12 + i] = (h3 >> (24 - i * 8)) & 0xFF;
        hash[16 + i] = (h4 >> (24 - i * 8)) & 0xFF;
    }

    // Base64 encode
    static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string acceptKey;
    for (int i = 0; i < 20; i += 3) {
        uint32_t n = (hash[i] << 16) | ((i + 1 < 20 ? hash[i + 1] : 0) << 8) | (i + 2 < 20 ? hash[i + 2] : 0);
        acceptKey += b64[(n >> 18) & 0x3F];
        acceptKey += b64[(n >> 12) & 0x3F];
        acceptKey += (i + 1 < 20) ? b64[(n >> 6) & 0x3F] : '=';
        acceptKey += (i + 2 < 20) ? b64[n & 0x3F] : '=';
    }

    std::string response = "HTTP/1.1 101 Switching Protocols\r\n"
                           "Upgrade: websocket\r\n"
                           "Connection: Upgrade\r\n"
                           "Sec-WebSocket-Accept: " + acceptKey + "\r\n";
    if (binaryProtocol) {
        response += "Sec-WebSocket-Protocol: binary\r\n";
    }
    response += "\r\n";
    send(client, response.c_str(), (int)response.size(), 0);
}

// ==================== VNCProxy ====================

VNCProxy::VNCProxy() {}

VNCProxy::~VNCProxy() { stopAllProxies(); }

int VNCProxy::startProxy(const std::string& vncHost, int vncPort) {
    // Find available port
    SOCKET listenSock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listenSock == INVALID_SOCKET) return 0;

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0; // let OS pick

    if (bind(listenSock, (sockaddr*)&addr, sizeof(addr)) != 0) {
        closesocket(listenSock);
        return 0;
    }

    int addrLen = sizeof(addr);
    getsockname(listenSock, (sockaddr*)&addr, &addrLen);
    int port = ntohs(addr.sin_port);

    if (listen(listenSock, 5) != 0) {
        closesocket(listenSock);
        return 0;
    }

    int id = m_nextId++;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_proxies[id] = {id, port, vncHost, vncPort};
        m_listenSockets[id] = listenSock;
    }

    // Start accept loop in a thread - store the thread
    {
        std::lock_guard<std::mutex> lock(m_threadMutex);
        // Join any previous thread for this id (shouldn't happen, but be safe)
        auto it = m_acceptThreads.find(id);
        if (it != m_acceptThreads.end() && it->second.joinable()) {
            it->second.join();
            m_acceptThreads.erase(it);
        }
        m_acceptThreads[id] = std::thread(&VNCProxy::acceptLoop, this, id);
    }

    return id;
}

void VNCProxy::acceptLoop(int proxyId) {
    SOCKET listenSock;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_listenSockets.find(proxyId);
        if (it == m_listenSockets.end()) return;
        listenSock = it->second;
    }

    while (!m_stopping.load()) {
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(listenSock, &readfds);
        timeval tv{1, 0}; // 1 second
        int sel = select(0, &readfds, nullptr, nullptr, &tv);
        if (sel <= 0) {
            if (sel < 0) {
                // Socket likely closed, exit loop
                break;
            }
            continue;
        }

        SOCKET client = accept(listenSock, nullptr, nullptr);
        if (client == INVALID_SOCKET) {
            if (m_stopping.load()) break;
            continue;
        }

        std::string vncHost;
        int vncPort;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            auto it = m_proxies.find(proxyId);
            if (it == m_proxies.end()) { closesocket(client); continue; }
            vncHost = it->second.vncHost;
            vncPort = it->second.vncPort;
        }

        // Handle in a detached thread - bridge has its own socket lifecycle
        std::thread(&VNCProxy::bridgeConnection, this, client, vncHost, vncPort).detach();
    }
}

void VNCProxy::bridgeConnection(SOCKET wsClient, const std::string& vncHost, int vncPort) {
    // Read HTTP upgrade request with a larger buffer
    std::vector<char> httpBuf;
    httpBuf.reserve(16384);
    char tmpBuf[4096];
    bool gotHeader = false;

    while (!gotHeader && !m_stopping.load()) {
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(wsClient, &readfds);
        timeval tv{0, 500000}; // 500ms
        int sel = select(0, &readfds, nullptr, nullptr, &tv);
        if (sel <= 0) {
            if (httpBuf.empty()) continue;
            break; // timeout after partial data
        }

        int n = recv(wsClient, tmpBuf, sizeof(tmpBuf), 0);
        if (n <= 0) { closesocket(wsClient); return; }
        httpBuf.insert(httpBuf.end(), tmpBuf, tmpBuf + n);

        // Check for end of HTTP headers
        std::string currentRequest(httpBuf.data(), httpBuf.size());
        if (currentRequest.find("\r\n\r\n") != std::string::npos) {
            gotHeader = true;
        }
        if (httpBuf.size() > 65536) {
            // Too much data without finding header end - abort
            closesocket(wsClient);
            return;
        }
    }

    if (httpBuf.empty()) { closesocket(wsClient); return; }

    std::string request(httpBuf.data(), httpBuf.size());
    // Case-insensitive check for WebSocket upgrade
    std::string lowerReq = strToLower(request);
    if (lowerReq.find("upgrade: websocket") == std::string::npos) {
        closesocket(wsClient);
        return;
    }

    // Perform WebSocket handshake
    performWSHandshake(wsClient, request);

    // Connect to VNC server
    SOCKET vncSock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in vncAddr{};
    vncAddr.sin_family = AF_INET;
    vncAddr.sin_port = htons(vncPort);
    inet_pton(AF_INET, vncHost.c_str(), &vncAddr.sin_addr);

    if (connect(vncSock, (sockaddr*)&vncAddr, sizeof(vncAddr)) != 0) {
        closesocket(wsClient);
        closesocket(vncSock);
        return;
    }

    // Set non-blocking mode for both sockets
    u_long mode = 1;
    ioctlsocket(wsClient, FIONBIO, &mode);
    ioctlsocket(vncSock, FIONBIO, &mode);

    // Bridge loop: WS client <-> VNC TCP socket
    std::vector<uint8_t> wsBuffer;
    uint8_t tcpBuf[65536];
    bool running = true;

    while (running && !m_stopping.load()) {
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(wsClient, &readfds);
        FD_SET(vncSock, &readfds);
        timeval tv{0, 100000}; // 100ms

        int sel = select(0, &readfds, nullptr, nullptr, &tv);
        if (sel < 0) break;

        // Data from VNC -> send as WS binary frame to browser
        if (FD_ISSET(vncSock, &readfds)) {
            int rd = recv(vncSock, (char*)tcpBuf, sizeof(tcpBuf), 0);
            if (rd <= 0) { running = false; break; }
            auto frame = buildWSFrame(0x02, tcpBuf, rd); // binary frame
            send(wsClient, (char*)frame.data(), (int)frame.size(), 0);
        }

        // Data from browser (WS) -> parse frames -> send raw to VNC
        if (FD_ISSET(wsClient, &readfds)) {
            int rd = recv(wsClient, (char*)tcpBuf, sizeof(tcpBuf), 0);
            if (rd <= 0) { running = false; break; }
            wsBuffer.insert(wsBuffer.end(), tcpBuf, tcpBuf + rd);

            // Parse all complete frames
            while (!wsBuffer.empty()) {
                WSFrame frame;
                size_t consumed = 0;
                if (!parseWSFrame(wsBuffer.data(), wsBuffer.size(), frame, consumed)) break;
                wsBuffer.erase(wsBuffer.begin(), wsBuffer.begin() + consumed);

                if (frame.opcode == 0x08) { // close
                    running = false;
                    break;
                } else if (frame.opcode == 0x09) { // ping -> pong
                    auto pong = buildWSFrame(0x0A, frame.payload.data(), frame.payload.size());
                    send(wsClient, (char*)pong.data(), (int)pong.size(), 0);
                } else if (frame.opcode == 0x02 || frame.opcode == 0x00) { // binary or continuation
                    send(vncSock, (char*)frame.payload.data(), (int)frame.payload.size(), 0);
                }
            }
        }
    }

    closesocket(wsClient);
    closesocket(vncSock);
}

void VNCProxy::stopProxy(int proxyId) {
    // Close the listen socket first (causes acceptLoop select to return error)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_listenSockets.find(proxyId);
        if (it != m_listenSockets.end()) {
            closesocket(it->second);
            m_listenSockets.erase(it);
        }
        m_proxies.erase(proxyId);
    }

    // Join the accept thread
    std::lock_guard<std::mutex> lock(m_threadMutex);
    auto it = m_acceptThreads.find(proxyId);
    if (it != m_acceptThreads.end()) {
        if (it->second.joinable()) it->second.join();
        m_acceptThreads.erase(it);
    }
}

void VNCProxy::stopAllProxies() {
    m_stopping.store(true);

    // Close all listen sockets FIRST to interrupt select()/accept()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (auto& [id, sock] : m_listenSockets) {
            closesocket(sock);
        }
        m_listenSockets.clear();
        m_proxies.clear();
    }

    // Then join all accept threads
    {
        std::lock_guard<std::mutex> lock(m_threadMutex);
        for (auto& [id, t] : m_acceptThreads) {
            if (t.joinable()) t.join();
        }
        m_acceptThreads.clear();
    }
}

VNCProxy::ProxyInfo VNCProxy::getProxyInfo(int proxyId) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_proxies.find(proxyId);
    if (it != m_proxies.end()) return it->second;
    return {0, 0, "", 0};
}

// ==================== HttpFileServer ====================

HttpFileServer::HttpFileServer() {}
HttpFileServer::~HttpFileServer() { stop(); }

int HttpFileServer::start(int preferredPort, const std::string& docRoot) {
    m_docRoot = docRoot;
    std::lock_guard<std::mutex> lock(m_listenMutex);
    m_listenSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (m_listenSocket == INVALID_SOCKET) return 0;

    // Allow address reuse
    int opt = 1;
    setsockopt(m_listenSocket, SOL_SOCKET, SO_REUSEADDR, (char*)&opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(preferredPort);

    if (bind(m_listenSocket, (sockaddr*)&addr, sizeof(addr)) != 0) {
        // Try port 0 (auto-assign)
        addr.sin_port = 0;
        if (bind(m_listenSocket, (sockaddr*)&addr, sizeof(addr)) != 0) {
            closesocket(m_listenSocket);
            return 0;
        }
    }

    int addrLen = sizeof(addr);
    getsockname(m_listenSocket, (sockaddr*)&addr, &addrLen);
    m_port.store(ntohs(addr.sin_port));

    if (listen(m_listenSocket, 10) != 0) {
        closesocket(m_listenSocket);
        return 0;
    }

    m_running.store(true);
    m_thread = std::thread(&HttpFileServer::serveLoop, this);
    return m_port.load();
}

void HttpFileServer::stop() {
    m_running.store(false);
    {
        std::lock_guard<std::mutex> lock(m_listenMutex);
        if (m_listenSocket != INVALID_SOCKET) {
            closesocket(m_listenSocket);
            m_listenSocket = INVALID_SOCKET;
        }
    }
    if (m_thread.joinable()) m_thread.join();
}

void HttpFileServer::serveLoop() {
    SOCKET localSock;
    {
        std::lock_guard<std::mutex> lock(m_listenMutex);
        localSock = m_listenSocket;
    }

    while (m_running.load()) {
        // Refresh socket in case it was replaced
        {
            std::lock_guard<std::mutex> lock(m_listenMutex);
            if (m_listenSocket != localSock) {
                localSock = m_listenSocket;
            }
            if (localSock == INVALID_SOCKET) break;
        }

        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(localSock, &readfds);
        timeval tv{0, 500000}; // 500ms
        int sel = select(0, &readfds, nullptr, nullptr, &tv);
        if (sel <= 0) continue;

        SOCKET client = accept(localSock, nullptr, nullptr);
        if (client == INVALID_SOCKET) continue;

        // Set short timeout for recv
        int timeout = 5000;
        setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, (char*)&timeout, sizeof(timeout));

        char buf[8192];
        int n = recv(client, buf, sizeof(buf), 0);
        if (n > 0) {
            handleRequest(client, std::string(buf, n), m_docRoot);
        }
        closesocket(client);
    }
}

void HttpFileServer::handleRequest(SOCKET client, const std::string& request, const std::string& docRoot) {
    // Parse HTTP method and path
    auto firstLine = request.substr(0, request.find("\r\n"));
    size_t pathStart = firstLine.find(' ');
    if (pathStart == std::string::npos) return;
    size_t pathEnd = firstLine.find(' ', pathStart + 1);
    if (pathEnd == std::string::npos) return;
    std::string fullPath = firstLine.substr(pathStart + 1, pathEnd - pathStart - 1);

    // Strip query string (e.g. /vnc.html?host=... -> /vnc.html)
    std::string path = fullPath;
    auto qsPos = path.find('?');
    if (qsPos != std::string::npos) {
        path = path.substr(0, qsPos);
    }

    // URL decode and path traversal check
    if (path.find("..") != std::string::npos) {
        std::string resp = "HTTP/1.1 403 Forbidden\r\nContent-Length: 0\r\n\r\n";
        send(client, resp.c_str(), (int)resp.size(), 0);
        return;
    }

    // Route: / -> vnc.html, /novnc/* -> novnc dir
    std::string filePath;
    if (path == "/") {
        filePath = docRoot + "/vnc.html";
    } else if (path.find("/novnc/") == 0) {
        filePath = docRoot + path;
    } else {
        filePath = docRoot + path;
    }

    // Read file
    std::ifstream file(filePath, std::ios::binary);
    if (!file.is_open()) {
        std::string resp = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n";
        send(client, resp.c_str(), (int)resp.size(), 0);
        return;
    }

    std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    file.close();

    // Build response
    std::string mime = getMimeType(filePath);
    std::string header = "HTTP/1.1 200 OK\r\n"
                         "Content-Type: " + mime + "\r\n"
                         "Content-Length: " + std::to_string(content.size()) + "\r\n"
                         "Access-Control-Allow-Origin: *\r\n"
                         "Connection: close\r\n\r\n";
    send(client, header.c_str(), (int)header.size(), 0);
    send(client, content.c_str(), (int)content.size(), 0);
}

std::string HttpFileServer::getMimeType(const std::string& path) {
    if (path.ends_with(".html") || path.ends_with(".htm")) return "text/html; charset=utf-8";
    if (path.ends_with(".js") || path.ends_with(".mjs")) return "application/javascript; charset=utf-8";
    if (path.ends_with(".css")) return "text/css; charset=utf-8";
    if (path.ends_with(".png")) return "image/png";
    if (path.ends_with(".jpg") || path.ends_with(".jpeg")) return "image/jpeg";
    if (path.ends_with(".ico")) return "image/x-icon";
    if (path.ends_with(".svg")) return "image/svg+xml";
    if (path.ends_with(".json")) return "application/json";
    if (path.ends_with(".wasm")) return "application/wasm";
    return "application/octet-stream";
}
