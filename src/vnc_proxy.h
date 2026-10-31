#pragma once
#include <winsock2.h>
#include <ws2tcpip.h>
#include <string>
#include <thread>
#include <atomic>
#include <functional>
#include <map>
#include <mutex>

// VNC WebSocket-to-TCP proxy
class VNCProxy {
public:
    VNCProxy();
    ~VNCProxy();

    // Start a WS proxy for a target VNC server, returns proxy ID (0=error)
    int startProxy(const std::string& vncHost, int vncPort = 5900);
    void stopProxy(int proxyId);
    void stopAllProxies();

    struct ProxyInfo {
        int id;
        int port;
        std::string vncHost;
        int vncPort;
    };
    ProxyInfo getProxyInfo(int proxyId) const;

private:
    void acceptLoop(int proxyId);
    void bridgeConnection(SOCKET wsClient, const std::string& vncHost, int vncPort);
    void cleanupThread(int proxyId);

    std::map<int, ProxyInfo> m_proxies;
    std::map<int, SOCKET> m_listenSockets;
    std::map<int, std::thread> m_acceptThreads;
    std::map<int, std::thread> m_bridgeThreads;
    std::mutex m_threadMutex;
    mutable std::mutex m_mutex;
    std::atomic<int> m_nextId{1};
    std::atomic<bool> m_stopping{false};
};

// Lightweight HTTP server for serving noVNC static files
class HttpFileServer {
public:
    HttpFileServer();
    ~HttpFileServer();

    int start(int preferredPort, const std::string& docRoot);
    void stop();
    int getPort() const { return m_port; }

private:
    void serveLoop();
    void handleRequest(SOCKET client, const std::string& request, const std::string& docRoot);
    std::string getMimeType(const std::string& path);

    SOCKET m_listenSocket{INVALID_SOCKET};
    std::mutex m_listenMutex;  // Protects m_listenSocket
    std::atomic<int> m_port{0};
    std::atomic<bool> m_running{false};
    std::thread m_thread;
    std::string m_docRoot;
};
