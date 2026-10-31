#include "ssh_engine.h"
#include <libssh/libssh.h>
#include <libssh/sftp.h>
#include <libssh/server.h>   // for ssh_scp typedef on some libssh builds
#include <sstream>
#include <algorithm>
#include <numeric>
#include <chrono>
#include <fstream>
#include <cstring>
#include <filesystem>
#include <fcntl.h>

#pragma comment(lib, "ws2_32.lib")

// Compatibility macro for older libssh versions
#ifndef SSH_EXTENDED_DATA_STDERR
#define SSH_EXTENDED_DATA_STDERR 1
#endif

// libssh marks the whole ssh_scp_* API as SSH_DEPRECATED. We still use it on
// purpose, so silence C4996 across this translation unit.
#if defined(_MSC_VER)
#pragma warning(disable: 4996)
#endif

// ==================== Logging ====================
static std::mutex g_logMutex;
static void WriteLog(const std::string& message) {
    std::lock_guard<std::mutex> lock(g_logMutex);
    std::ofstream logFile("debug.log", std::ios::app);
    if (logFile.is_open()) {
        logFile << "[" << __TIME__ << "] " << message << std::endl;
        logFile.close();
    }
    OutputDebugStringA(("SeewoKiller: " + message + "\n").c_str());
}

// Open and authenticate an SSH session to one host. Returns the live session
// (caller disconnects/frees on failure or when done). On failure sets *errOut.
static ssh_session openSession(const std::string& ip,
                               const std::string& username,
                               const std::string& password,
                               std::string* errOut) {
    ssh_session session = ssh_new();
    if (!session) {
        if (errOut) *errOut = "Failed to create SSH session";
        return nullptr;
    }

    int port = 22;
    int timeout = SSH_CONNECT_TIMEOUT;
    int verbosity = SSH_LOG_NOLOG;
    ssh_options_set(session, SSH_OPTIONS_HOST, ip.c_str());
    ssh_options_set(session, SSH_OPTIONS_USER, username.c_str());
    ssh_options_set(session, SSH_OPTIONS_TIMEOUT, &timeout);
    ssh_options_set(session, SSH_OPTIONS_LOG_VERBOSITY, &verbosity);
    ssh_options_set(session, SSH_OPTIONS_PORT, &port);

    if (ssh_connect(session) != SSH_OK) {
        if (errOut) *errOut = "SSH connection failed: " + std::string(ssh_get_error(session));
        ssh_free(session);
        return nullptr;
    }

    if (ssh_userauth_password(session, nullptr, password.c_str()) != SSH_AUTH_SUCCESS) {
        if (errOut) *errOut = "Authentication failed: " + std::string(ssh_get_error(session));
        ssh_disconnect(session);
        ssh_free(session);
        return nullptr;
    }
    return session;
}

// ==================== IP Parsing ====================

std::vector<std::string> GenerateIPRange(const std::string& startIP, const std::string& endIP) {
    std::vector<std::string> result;
    in_addr startAddr, endAddr;
    if (inet_pton(AF_INET, startIP.c_str(), &startAddr) != 1 ||
        inet_pton(AF_INET, endIP.c_str(), &endAddr) != 1) {
        return result;
    }
    uint32_t start = ntohl(startAddr.s_addr);
    uint32_t end = ntohl(endAddr.s_addr);
    if (start > end) std::swap(start, end);
    // Safety limit
    if (end - start > 65536) end = start + 65536;
    for (uint32_t i = start; i <= end; ++i) {
        in_addr addr;
        addr.s_addr = htonl(i);
        char buf[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &addr, buf, INET_ADDRSTRLEN);
        result.emplace_back(buf);
    }
    return result;
}

std::vector<std::string> ParseShortRange(const std::string& rangeStr) {
    std::vector<std::string> result;
    size_t dashPos = rangeStr.find('-');
    if (dashPos == std::string::npos) {
        result.push_back(rangeStr);
        return result;
    }
    std::string prefix = rangeStr.substr(0, dashPos);
    std::string suffix = rangeStr.substr(dashPos + 1);

    size_t lastDot = prefix.find_last_of('.');
    if (lastDot == std::string::npos) {
        result.push_back(rangeStr);
        return result;
    }

    std::string ipPrefix = prefix.substr(0, lastDot + 1);
    std::string startNumStr = prefix.substr(lastDot + 1);
    std::string endNumStr = suffix;

    if (!std::all_of(startNumStr.begin(), startNumStr.end(), ::isdigit) ||
        !std::all_of(endNumStr.begin(), endNumStr.end(), ::isdigit)) {
        return GenerateIPRange(prefix, suffix);
    }

    int startNum = std::stoi(startNumStr);
    int endNum = std::stoi(endNumStr);
    if (startNum > endNum) std::swap(startNum, endNum);

    for (int i = startNum; i <= endNum; ++i) {
        result.push_back(ipPrefix + std::to_string(i));
    }
    return result;
}

std::vector<std::string> ParseIPList(const std::string& input) {
    std::vector<std::string> ipList;
    std::istringstream iss(input);
    std::string line;
    while (std::getline(iss, line)) {
        // trim
        line.erase(0, line.find_first_not_of(" \t\r\n"));
        line.erase(line.find_last_not_of(" \t\r\n") + 1);
        if (line.empty()) continue;

        std::istringstream lineIss(line);
        std::string item;
        while (std::getline(lineIss, item, ',')) {
            item.erase(0, item.find_first_not_of(" \t"));
            item.erase(item.find_last_not_of(" \t") + 1);
            if (item.empty()) continue;
            auto range = ParseShortRange(item);
            ipList.insert(ipList.end(), range.begin(), range.end());
        }
    }
    // deduplicate and sort
    std::sort(ipList.begin(), ipList.end());
    ipList.erase(std::unique(ipList.begin(), ipList.end()), ipList.end());
    return ipList;
}

// ==================== SSH Connection ====================

SSHResult SSHConnect(const std::string& ip, const std::string& username,
                     const std::string& password, const std::string& command) {
    WriteLog("SSH connecting to " + ip + " with username " + username + " and command: " + command);
    SSHResult result;
    result.ip = ip;
    result.success = false;

    try {
        std::string err;
        ssh_session session = openSession(ip, username, password, &err);
        if (!session) {
            result.error = err;
            WriteLog("SSH connect failed for " + ip + ": " + err);
            return result;
        }

        // Create channel to execute command
        ssh_channel channel = ssh_channel_new(session);
        if (!channel) {
            result.error = "Failed to create channel";
            ssh_disconnect(session);
            ssh_free(session);
            return result;
        }

        int rc = ssh_channel_open_session(channel);
        if (rc != SSH_OK) {
            result.error = "Failed to open session: " + std::string(ssh_get_error(session));
            ssh_channel_free(channel);
            ssh_disconnect(session);
            ssh_free(session);
            return result;
        }

        rc = ssh_channel_request_exec(channel, command.c_str());
        if (rc != SSH_OK) {
            result.error = "Command execution failed: " + std::string(ssh_get_error(session));
            ssh_channel_close(channel);
            ssh_channel_free(channel);
            ssh_disconnect(session);
            ssh_free(session);
            return result;
        }

        // Read stdout
        char buffer[4096];
        rc = ssh_channel_read(channel, buffer, sizeof(buffer), 0);
        while (rc > 0) {
            result.output.append(buffer, rc);
            rc = ssh_channel_read(channel, buffer, sizeof(buffer), 0);
        }

        // Read stderr
        rc = ssh_channel_read(channel, buffer, sizeof(buffer), SSH_EXTENDED_DATA_STDERR);
        while (rc > 0) {
            result.error.append(buffer, rc);
            rc = ssh_channel_read(channel, buffer, sizeof(buffer), SSH_EXTENDED_DATA_STDERR);
        }

        ssh_channel_send_eof(channel);
        ssh_channel_close(channel);
        ssh_channel_free(channel);
        ssh_disconnect(session);
        ssh_free(session);

        result.success = true;
        WriteLog("SSH operation completed for " + ip + ", output length: " + std::to_string(result.output.length()));
        return result;
    }
    catch (const std::exception& e) {
        WriteLog("Exception in SSHConnect for " + ip + ": " + std::string(e.what()));
        result.error = "Exception: " + std::string(e.what());
        return result;
    }
    catch (...) {
        WriteLog("Unknown exception in SSHConnect for " + ip);
        result.error = "Unknown exception";
        return result;
    }
}

// ==================== SCP single-host ====================

// Filename-only part of a path.
static std::string baseName(const std::string& path) {
    auto pos = path.find_last_of("/\\");
    if (pos == std::string::npos) return path;
    if (pos == path.size() - 1) {
        std::string trimmed = path.substr(0, pos);
        auto pos2 = trimmed.find_last_of("/\\");
        return (pos2 == std::string::npos) ? trimmed : trimmed.substr(pos2 + 1);
    }
    return path.substr(pos + 1);
}

// Parent directory of a path. If the path is a root-only or bare filename,
// returns "." (current directory on the remote host).
static std::string parentDir(const std::string& path) {
    if (path.empty()) return ".";
    std::string trimmed = path;
    while (!trimmed.empty() && (trimmed.back() == '/' || trimmed.back() == '\\'))
        trimmed.pop_back();
    auto pos = trimmed.find_last_of("/\\");
    if (pos == std::string::npos || pos == 0) return ".";
    return trimmed.substr(0, pos);
}

// Replace Windows backslashes with POSIX forward slashes for the remote side.
static std::string toPosixPath(const std::string& path) {
    std::string out = path;
    std::replace(out.begin(), out.end(), '\\', '/');
    return out;
}

#include <libssh/sftp.h>
#include <fcntl.h>

// ==================== SFTP file transfer (UI still calls it SCP) ====================

static std::string joinRemotePath(const std::string& dir, const std::string& name) {
    if (dir.empty()) return name;
    if (dir.back() == '/' || dir.back() == '\\') return dir + name;
    return dir + '\\' + name;
}

SSHResult SCPPush(const std::string& ip, const std::string& username,
                  const std::string& password,
                  const std::string& localPath, const std::string& remotePath) {
    SSHResult result;
    result.ip = ip;

    // Read local file fully into memory (expected: small scripts/configs)
    std::ifstream file(localPath, std::ios::binary);
    if (!file.is_open()) {
        result.error = "Cannot open local file: " + localPath;
        return result;
    }
    std::string content((std::istreambuf_iterator<char>(file)),
                         std::istreambuf_iterator<char>());

    try {
        std::string err;
        ssh_session session = openSession(ip, username, password, &err);
        if (!session) {
            result.error = err;
            return result;
        }

        sftp_session sftp = sftp_new(session);
        if (!sftp) {
            result.error = "sftp_new failed: " + std::string(ssh_get_error(session));
            ssh_disconnect(session); ssh_free(session);
            return result;
        }
        if (sftp_init(sftp) != SSH_OK) {
            result.error = "sftp_init failed: " + std::string(ssh_get_error(session));
            sftp_free(sftp); ssh_disconnect(session); ssh_free(session);
            return result;
        }

        // If remote path ends with a separator, treat it as a directory.
        std::string destPath = remotePath;
        if (!destPath.empty() && (destPath.back() == '/' || destPath.back() == '\\')) {
            destPath = joinRemotePath(destPath, baseName(localPath));
        }

        sftp_file f = sftp_open(sftp, destPath.c_str(),
                                O_WRONLY | O_CREAT | O_TRUNC,
                                0644);
        if (!f) {
            result.error = "sftp_open failed: " + std::string(ssh_get_error(session));
            sftp_free(sftp); ssh_disconnect(session); ssh_free(session);
            return result;
        }

        size_t off = 0;
        while (off < content.size()) {
            size_t chunk = (std::min)(size_t{65536}, content.size() - off);
            auto written = sftp_write(f, content.data() + off, chunk);
            if (written < 0 || static_cast<size_t>(written) != chunk) {
                result.error = "sftp_write failed: " + std::string(ssh_get_error(session));
                sftp_close(f); sftp_free(sftp); ssh_disconnect(session); ssh_free(session);
                return result;
            }
            off += chunk;
        }

        sftp_close(f);
        sftp_free(sftp);
        ssh_disconnect(session);
        ssh_free(session);

        result.success = true;
        result.output = "Uploaded " + std::to_string(content.size()) + " bytes to " + destPath;
        return result;
    }
    catch (const std::exception& e) {
        result.error = "Exception: " + std::string(e.what());
        return result;
    }
    catch (...) {
        result.error = "Unknown exception";
        return result;
    }
}

SSHResult SCPPull(const std::string& ip, const std::string& username,
                  const std::string& password,
                  const std::string& remotePath, const std::string& localPath) {
    SSHResult result;
    result.ip = ip;

    try {
        std::string err;
        ssh_session session = openSession(ip, username, password, &err);
        if (!session) {
            result.error = err;
            return result;
        }

        sftp_session sftp = sftp_new(session);
        if (!sftp) {
            result.error = "sftp_new failed: " + std::string(ssh_get_error(session));
            ssh_disconnect(session); ssh_free(session);
            return result;
        }
        if (sftp_init(sftp) != SSH_OK) {
            result.error = "sftp_init failed: " + std::string(ssh_get_error(session));
            sftp_free(sftp); ssh_disconnect(session); ssh_free(session);
            return result;
        }

        sftp_file f = sftp_open(sftp, remotePath.c_str(), O_RDONLY, 0);
        if (!f) {
            result.error = "sftp_open failed: " + std::string(ssh_get_error(session));
            sftp_free(sftp); ssh_disconnect(session); ssh_free(session);
            return result;
        }

        // Query size so we can reserve
        size_t fileSize = 0;
        sftp_attributes attrs = sftp_stat(sftp, remotePath.c_str());
        if (attrs) {
            fileSize = static_cast<size_t>(attrs->size);
            sftp_attributes_free(attrs);
        }

        std::string content;
        content.reserve(fileSize);
        char buf[65536];
        auto rc = sftp_read(f, buf, sizeof(buf));
        while (rc > 0) {
            content.append(buf, rc);
            rc = sftp_read(f, buf, sizeof(buf));
        }
        if (rc < 0) {
            result.error = "sftp_read failed: " + std::string(ssh_get_error(session));
            sftp_close(f); sftp_free(sftp); ssh_disconnect(session); ssh_free(session);
            return result;
        }

        sftp_close(f);
        sftp_free(sftp);
        ssh_disconnect(session);
        ssh_free(session);

        // Determine local destination
        std::string destPath = localPath;
        std::error_code ec;
        if (std::filesystem::is_directory(localPath, ec)) {
            auto pos = remotePath.find_last_of("/\\");
            std::string fname = (pos == std::string::npos) ? remotePath : remotePath.substr(pos + 1);
            if (fname.empty()) fname = "pulled.dat";
            destPath = joinRemotePath(std::filesystem::path(localPath).string(), fname);
        }

        std::ofstream out(destPath, std::ios::binary | std::ios::trunc);
        if (!out.is_open()) {
            result.error = "Cannot write local file: " + destPath;
            return result;
        }
        out.write(content.data(), content.size());
        out.close();

        result.success = true;
        result.output = "Pulled " + std::to_string(content.size()) + " bytes to " + destPath;
        return result;
    }
    catch (const std::exception& e) {
        result.error = "Exception: " + std::string(e.what());
        return result;
    }
    catch (...) {
        result.error = "Unknown exception";
        return result;
    }
}

// ==================== BatchSSHExecutor ====================

BatchSSHExecutor::BatchSSHExecutor() = default;
BatchSSHExecutor::~BatchSSHExecutor() {
    stopLoop();
    // Wait for thread to finish
    if (m_workerThread.joinable()) {
        m_workerThread.join();
    }
}

void BatchSSHExecutor::execute(const std::vector<std::string>& ipList,
                               const std::string& username,
                               const std::string& password,
                               const std::string& command,
                               int delaySeconds,
                               bool loopEnabled,
                               int loopIntervalSeconds) {
    WriteLog("BatchSSHExecutor::execute called with " + std::to_string(ipList.size()) + " IPs");
    
    if (m_running.load()) {
        WriteLog("BatchSSHExecutor already running, returning");
        return;
    }
    
    // Join previous thread if it exists
    if (m_workerThread.joinable()) {
        WriteLog("Joining previous worker thread...");
        m_workerThread.join();
        WriteLog("Previous thread joined");
    }
    
    m_running.store(true);
    m_stopRequested.store(false);
    WriteLog("Starting worker thread...");

    try {
        // Create joinable thread instead of detached
        m_workerThread = std::thread([this, ipList, username, password, command, delaySeconds, loopEnabled, loopIntervalSeconds]() {
            WriteLog("Worker thread started");
            try {
        if (loopEnabled) {
            // Loop mode
            int iteration = 0;
            while (!m_stopRequested.load()) {
                if (iteration > 0 && !m_stopRequested.load()) {
                    // Wait for loop interval, checking stop every 500ms
                    for (int waited = 0; waited < loopIntervalSeconds * 1000 && !m_stopRequested.load(); waited += 500) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(500));
                    }
                }
                if (m_stopRequested.load()) break;

                std::string header = "\r\n=== Loop iteration #" + std::to_string(++iteration) + " ===\r\n";
                if (m_outputCb) m_outputCb(header);
                batchExecute(ipList, username, password, command, (iteration == 1) ? delaySeconds : 0);
            }
            if (m_outputCb) m_outputCb("\r\n[Loop stopped]\r\n");
        } else {
            // Single execution
            batchExecute(ipList, username, password, command, delaySeconds);
        }
        m_running.store(false);
        WriteLog("Worker thread finished");
            }
            catch (const std::exception& e) {
                WriteLog("Exception in worker thread: " + std::string(e.what()));
                m_running.store(false);
            }
            catch (...) {
                WriteLog("Unknown exception in worker thread");
                m_running.store(false);
            }
        });
        WriteLog("Worker thread created successfully");
    }
    catch (const std::exception& e) {
        WriteLog("Failed to create worker thread: " + std::string(e.what()));
        m_running.store(false);
    }
    catch (...) {
        WriteLog("Failed to create worker thread (unknown error)");
        m_running.store(false);
    }
}

void BatchSSHExecutor::stopLoop() {
    m_stopRequested.store(true);
}

void BatchSSHExecutor::runConcurrent(
        const std::vector<std::string>& ipList,
        const std::string& username,
        const std::string& password,
        bool stopOnFirstSuccess,
        const std::function<SSHResult(const std::string& ip,
                                      const std::string& user,
                                      const std::string& pass)>& taskFn) {

    int total = (int)ipList.size();
    ProgressInfo prog{0, total, 0};
    if (m_progressCb) m_progressCb(prog);

    std::atomic<int> successCount{0};
    std::atomic<int> failedCount{0};
    std::atomic<int> completed{0};
    std::atomic<bool> found{false};          // for pull: stop-on-first-success
    std::mutex cbMutex;
    std::mutex indexMutex;
    size_t nextIndex = 0;

    auto worker = [&]() {
        while (true) {
            if (m_stopRequested.load()) break;
            if (stopOnFirstSuccess && found.load()) break;

            std::string ip;
            {
                std::lock_guard<std::mutex> lock(indexMutex);
                if (nextIndex >= ipList.size()) break;
                ip = ipList[nextIndex++];
            }

            // Skip work once a success already landed (pull mode)
            if (stopOnFirstSuccess && found.load()) break;

            auto res = taskFn(ip, username, password);

            int cur = ++completed;
            int pct = total ? (int)((cur * 100) / total) : 100;
            prog.current = cur;
            prog.percent = pct;
            if (m_progressCb) m_progressCb(prog);

            std::string line;
            if (res.success) {
                successCount++;
                if (stopOnFirstSuccess) found.store(true);
                line = "[+] [" + ip + "] Success\r\n";
                if (!res.output.empty()) line += "   " + res.output + "\r\n";
            } else {
                failedCount++;
                line = "[-] [" + ip + "] Failed: " + res.error + "\r\n";
            }
            {
                std::lock_guard<std::mutex> lock(cbMutex);
                if (m_outputCb) m_outputCb(line);
            }
        }
    };

    int numWorkers = (std::min)(MAX_CONCURRENT, total);
    std::vector<std::thread> threads;
    threads.reserve(numWorkers);
    for (int i = 0; i < numWorkers; ++i) threads.emplace_back(worker);
    for (auto& t : threads) if (t.joinable()) t.join();

    std::string summary = "\r\n================================\r\n";
    summary += "Done! Success: " + std::to_string(successCount.load()) +
               " | Failed: " + std::to_string(failedCount.load()) +
               " | Total: " + std::to_string(total) + "\r\n";
    if (m_outputCb) m_outputCb(summary);

    prog.percent = 100;
    if (m_progressCb) m_progressCb(prog);
    if (m_completeCb) m_completeCb(total, successCount.load(), failedCount.load());
}

void BatchSSHExecutor::executeScpPush(const std::vector<std::string>& ipList,
                                       const std::string& username,
                                       const std::string& password,
                                       const std::string& localPath,
                                       const std::string& remotePath) {
    WriteLog("executeScpPush: " + std::to_string(ipList.size()) + " IPs");
    if (m_outputCb) m_outputCb("SCP Upload: " + localPath + " -> " + remotePath + "\r\n");

    runConcurrent(ipList, username, password, /*stopOnFirstSuccess=*/false,
        [localPath, remotePath](const std::string& ip,
                                 const std::string& user,
                                 const std::string& pass) {
            return SCPPush(ip, user, pass, localPath, remotePath);
        });
}

void BatchSSHExecutor::executeScpPull(const std::vector<std::string>& ipList,
                                       const std::string& username,
                                       const std::string& password,
                                       const std::string& remotePath,
                                       const std::string& localPath) {
    WriteLog("executeScpPull: " + std::to_string(ipList.size()) + " IPs, remote=" + remotePath);
    if (m_outputCb) m_outputCb("SCP Pull (stop on first success): " + remotePath + " -> " + localPath + "\r\n");

    runConcurrent(ipList, username, password, /*stopOnFirstSuccess=*/true,
        [remotePath, localPath](const std::string& ip,
                                const std::string& user,
                                const std::string& pass) {
            return SCPPull(ip, user, pass, remotePath, localPath);
        });
}

void BatchSSHExecutor::batchExecute(const std::vector<std::string>& ipList,
                                    const std::string& username,
                                    const std::string& password,
                                    const std::string& command,
                                    int delaySeconds) {
    WriteLog("batchExecute called with " + std::to_string(ipList.size()) + " IPs");

    int total = (int)ipList.size();

    // Header
    std::string header = "Targets: " + std::to_string(total) + " | User: " + username + " | Cmd: " + command + "\r\n";
    header += "================================\r\n";
    if (m_outputCb) m_outputCb(header);

    // Delay
    if (delaySeconds > 0) {
        if (m_outputCb) m_outputCb("Delay " + std::to_string(delaySeconds) + "s...\r\n");
        for (int i = 0; i < delaySeconds * 2 && !m_stopRequested.load(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
        if (m_stopRequested.load()) return;
    }

    runConcurrent(ipList, username, password, /*stopOnFirstSuccess=*/false,
        [command](const std::string& ip, const std::string& user, const std::string& pass) {
            return SSHConnect(ip, user, pass, command);
        });
}
