#pragma once

// Windows headers must come first for architecture macros
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <string>
#include <functional>
#include <vector>
#include <memory>
#include <atomic>
#include <mutex>
#include <thread>

// SSH result for a single connection
struct SSHResult {
    std::string ip;
    bool success = false;
    std::string output;
    std::string error;
};

// Progress info
struct ProgressInfo {
    int current = 0;
    int total = 0;
    int percent = 0;
};

// Callback types
using OutputCallback = std::function<void(const std::string&)>;
using ProgressCallback = std::function<void(const ProgressInfo&)>;
using CompleteCallback = std::function<void(int total, int success, int failed)>;

// IP parsing
std::vector<std::string> ParseIPList(const std::string& input);
std::vector<std::string> ParseShortRange(const std::string& rangeStr);
std::vector<std::string> GenerateIPRange(const std::string& startIP, const std::string& endIP);

// SSH single connection
SSHResult SSHConnect(const std::string& ip, const std::string& username,
                     const std::string& password, const std::string& command);

// SCP single connection: push a local file to one host's remote path
SSHResult SCPPush(const std::string& ip, const std::string& username,
                  const std::string& password,
                  const std::string& localPath, const std::string& remotePath);

// SCP single connection: pull a remote file to local path (overwrite)
SSHResult SCPPull(const std::string& ip, const std::string& username,
                  const std::string& password,
                  const std::string& remotePath, const std::string& localPath);

// Batch SSH execution
class BatchSSHExecutor {
public:
    BatchSSHExecutor();
    ~BatchSSHExecutor();

    void execute(const std::vector<std::string>& ipList,
                 const std::string& username,
                 const std::string& password,
                 const std::string& command,
                 int delaySeconds,
                 bool loopEnabled,
                 int loopIntervalSeconds);

    // Push a local file to ALL hosts in ipList (concurrently).
    void executeScpPush(const std::vector<std::string>& ipList,
                        const std::string& username,
                        const std::string& password,
                        const std::string& localPath,
                        const std::string& remotePath);

    // Pull a remote file from hosts. Stops as soon as ANY host succeeds.
    void executeScpPull(const std::vector<std::string>& ipList,
                        const std::string& username,
                        const std::string& password,
                        const std::string& remotePath,
                        const std::string& localPath);

    void stopLoop();

    void setOutputCallback(OutputCallback cb) { m_outputCb = cb; }
    void setProgressCallback(ProgressCallback cb) { m_progressCb = cb; }
    void setCompleteCallback(CompleteCallback cb) { m_completeCb = cb; }

    bool isRunning() const { return m_running.load(); }

private:
    void batchExecute(const std::vector<std::string>& ipList,
                      const std::string& username,
                      const std::string& password,
                      const std::string& command,
                      int delaySeconds);

    // Generic concurrent worker-pool runner. taskFn returns true on a
    // "stop-after-first-success" outcome (pull), false otherwise.
    void runConcurrent(const std::vector<std::string>& ipList,
                       const std::string& username,
                       const std::string& password,
                       bool stopOnFirstSuccess,
                       const std::function<SSHResult(const std::string& ip,
                                                     const std::string& user,
                                                     const std::string& pass)>& taskFn);

    OutputCallback m_outputCb;
    ProgressCallback m_progressCb;
    CompleteCallback m_completeCb;

    std::atomic<bool> m_running{false};
    std::atomic<bool> m_stopRequested{false};
    std::thread m_workerThread;  // Store thread handle for proper cleanup
    static constexpr int MAX_CONCURRENT = 8;
};

// SSH connect timeout (seconds) — lowered to 1s so unreachable hosts don't
// stall the batch; LAN hosts respond well within this.
constexpr int SSH_CONNECT_TIMEOUT = 1;
