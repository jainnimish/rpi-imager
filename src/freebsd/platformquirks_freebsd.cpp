/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright (C) 2025 Raspberry Pi Ltd
 *
 * FreeBSD platform-specific implementation.
 */

#include "../platformquirks.h"
#include <cstdlib>
#include <unistd.h>
#include <pwd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <sys/mount.h>
#include <libgeom.h>
#include <sys/eventfd.h>  // eventfd for modern thread signaling
#include <sys/utsname.h>   // uname() for kernel version logging
#include <poll.h>          // poll() instead of select()
#include <signal.h>        // Signal masking for worker thread
#include <ifaddrs.h>
#include <net/if.h>
#include <netlink/netlink.h>
#include <netlink/netlink_route.h>
#include <cstdio>
#include <cstring>
#include <cerrno>
#include <fcntl.h>
#include <pthread.h>
#include <atomic>
#include <mutex>
#include <net/if_arp.h>
#include <QDebug>
#include <QProcess>
#include <QFile>
#include <QStandardPaths>
#include <QDir>
#include <QFileInfo>
#include <QCryptographicHash>
#include <filesystem>
#include <QUuid>
#include <vector>
#include <string>

namespace {
    // Network monitoring state
    int g_netlinkSocket = -1;
    pthread_t g_monitorThread;
    std::atomic<bool> g_monitorRunning{false};
    int g_stopEventFd = -1;  // eventfd for signaling thread to stop (more efficient than pipe)
    PlatformQuirks::NetworkStatusCallback g_networkCallback = nullptr;
    pthread_mutex_t g_callbackMutex = PTHREAD_MUTEX_INITIALIZER;

    // Cached network connectivity state (updated by netlink monitor)
    std::atomic<bool> g_cachedNetworkConnectivity{false};
    std::atomic<bool> g_networkConnectivityCacheValid{false};

    void* netlinkMonitorThread(void* arg) {
        return nullptr;
    }

    // Grant root access to the user's X11 display via xhost.
    // Must be run as the original (non-root) user who owns the display session,
    // since root doesn't yet have permission to connect.
    // Uses +SI:localuser:root (server-interpreted, narrowly scoped to root only).
    void grantRootDisplayAccess(uid_t uid, gid_t gid) {
    }
}

namespace PlatformQuirks {

void applyQuirks() {
}

namespace {
    // Sound files in order of preference (various distro locations)
    static const char* const SOUND_FILES[] = {
        "/usr/share/sounds/freedesktop/stereo/complete.oga",       // Freedesktop (RPi OS, Debian, Fedora)
        "/usr/share/sounds/freedesktop/stereo/bell.oga",           // Freedesktop fallback
        "/usr/share/sounds/Yaru/stereo/complete.oga",              // Ubuntu Yaru theme
        "/usr/share/sounds/ocean/stereo/completion.oga",           // KDE Ocean theme
        "/usr/share/sounds/gnome/default/alerts/glass.ogg",        // GNOME legacy
        nullptr
    };

    // Find the first available sound file (cached, thread-safe)
    // Falls back to extracting a bundled chime from Qt resources if no system sound exists.
    static const char* findSoundFile() {
        static const char* cachedSoundFile = nullptr;
        static std::once_flag soundFileOnce;

        std::call_once(soundFileOnce, []() {
            // 1. Try system sound files
            for (int i = 0; SOUND_FILES[i] != nullptr; i++) {
                if (access(SOUND_FILES[i], R_OK) == 0) {
                    cachedSoundFile = SOUND_FILES[i];
                    return;
                }
            }

            // 2. Extract bundled fallback chime to a temp file.
            //    Use a random UUID in filename to avoid symlink attacks
            //    (rpi-imager runs as root via pkexec, so temp file security matters).
            QFile bundled(":/sounds/chime.wav");
            if (bundled.exists()) {
                static QString tempPath = QDir::tempPath()
                    + QString("/rpi-imager-chime-%1.wav").arg(
                        QUuid::createUuid().toString(QUuid::WithoutBraces));

                // Remove any pre-existing file or symlink at this path
                QFile::remove(tempPath);

                if (bundled.copy(tempPath)) {
                    QFile::setPermissions(tempPath,
                        QFileDevice::ReadOwner | QFileDevice::WriteOwner);
                    // static storage ensures pathBytes outlives the lambda so constData() remains valid
                    static QByteArray pathBytes = tempPath.toLocal8Bit();
                    cachedSoundFile = pathBytes.constData();
                    qDebug() << "Using bundled fallback chime:" << tempPath;

                    // Clean up temp file on application exit
                    std::atexit([]() { QFile::remove(tempPath); });
                }
            }
        });
        return cachedSoundFile;
    }

    // Command availability cache to avoid repeated PATH searches
    enum class AudioCommand {
        CanberraGtkPlay,
        PwPlay,
        Pactl,
        Beep,
        COUNT
    };

    // Thread-safe command availability cache using std::call_once per command
    static std::once_flag commandOnceFlags[static_cast<int>(AudioCommand::COUNT)];
    static bool commandExistsResult[static_cast<int>(AudioCommand::COUNT)] = {false};

    static bool commandExists(AudioCommand cmd) {
        int idx = static_cast<int>(cmd);
        std::call_once(commandOnceFlags[idx], [idx, cmd]() {
            const char* cmdName = nullptr;
            switch (cmd) {
                case AudioCommand::CanberraGtkPlay: cmdName = "canberra-gtk-play"; break;
                case AudioCommand::PwPlay: cmdName = "pw-play"; break;
                case AudioCommand::Pactl: cmdName = "pactl"; break;
                case AudioCommand::Beep: cmdName = "beep"; break;
                default: return;
            }
            commandExistsResult[idx] = !QStandardPaths::findExecutable(QString::fromLatin1(cmdName)).isEmpty();
        });
        return commandExistsResult[idx];
    }
}

bool isBeepAvailable() {
    // canberra-gtk-play uses the system sound theme - no file needed
    if (commandExists(AudioCommand::CanberraGtkPlay)) {
        return true;
    }

    // Other mechanisms need a sound file
    const char* soundFile = findSoundFile();
    if (soundFile) {
        if (commandExists(AudioCommand::PwPlay) ||
            commandExists(AudioCommand::Pactl)) {
            return true;
        }
    }

    // PC speaker beep - no dependencies
    if (commandExists(AudioCommand::Beep)) {
        return true;
    }

    qDebug() << "No beep mechanism available on this Linux system";
    return false;
}

void beep() {
    // 1. canberra-gtk-play (XDG Sound Theme - best option, uses system theme)
    if (commandExists(AudioCommand::CanberraGtkPlay)) {
        if (QProcess::execute("canberra-gtk-play", {"--id=complete"}) == 0) {
            return;
        }
    }

    // Find a sound file for the remaining mechanisms (result is cached)
    const char* soundFile = findSoundFile();

    if (soundFile) {
        // 2. pw-play (PipeWire - default on modern distros including Raspberry Pi OS)
        if (commandExists(AudioCommand::PwPlay)) {
            if (QProcess::execute("pw-play", {soundFile}) == 0) {
                return;
            }
        }

        // 3. pactl (PulseAudio - legacy systems)
        if (commandExists(AudioCommand::Pactl)) {
            if (QProcess::execute("pactl", {"upload-sample", soundFile, "imager-beep"}) == 0) {
                QProcess::execute("pactl", {"play-sample", "imager-beep"});
                return;
            }
        }
    }

    // 4. PC speaker beep command
    if (commandExists(AudioCommand::Beep)) {
        if (QProcess::execute("beep", {}) == 0) {
            return;
        }
    }

    // 5. System bell via echo (rarely works in GUI environments, but worth trying)
    QProcess::execute("echo", {"-e", "\\a"});

    qDebug() << "Beep requested but no suitable audio mechanism found on this Linux system";
}

bool hasNetworkConnectivity() {
    // Return cached value if valid (invalidated by netlink monitor on changes)
    if (g_networkConnectivityCacheValid.load(std::memory_order_relaxed)) {
        return g_cachedNetworkConnectivity.load(std::memory_order_relaxed);
    }

    struct ifaddrs *ifas, *ifa;

    getifaddrs(&ifas);

    for (ifa = ifas; ifa != NULL; ifa = ifa->ifa_next) {
        if (QString(ifa->ifa_name) == "lo") continue;

        if (ifa->ifa_flags & IFF_UP) {
            // Interface is up; cache and return
            g_cachedNetworkConnectivity.store(true, std::memory_order_relaxed);
            g_networkConnectivityCacheValid.store(true, std::memory_order_relaxed);
            return true;
        }
    }

    // All interfaces are down or none are available; cache and return
    g_cachedNetworkConnectivity.store(false, std::memory_order_relaxed);
    g_networkConnectivityCacheValid.store(true, std::memory_order_relaxed);
    return false;
}

bool isNetworkReady() {
    // Assume time is reliable
    return hasNetworkConnectivity();
}

void startNetworkMonitoring(NetworkStatusCallback callback) {
    // Stop any existing monitoring
    stopNetworkMonitoring();

    // Set callback under mutex
    pthread_mutex_lock(&g_callbackMutex);
    g_networkCallback = callback;
    pthread_mutex_unlock(&g_callbackMutex);

    // Create netlink socket for routing/link messages
    // Use SOCK_CLOEXEC to prevent fd leak to child processes after fork/exec
    g_netlinkSocket = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_ROUTE);
    if (g_netlinkSocket < 0) {
        int savedErrno = errno;
        fprintf(stderr, "Failed to create netlink socket: %s (errno %d)\n", strerror(savedErrno), savedErrno);
        return;
    }

    // Bind to multicast group for link changes
    struct sockaddr_nl addr = {};
    addr.nl_family = AF_NETLINK;
    addr.nl_groups = RTMGRP_LINK;  // Subscribe to link up/down events

    if (bind(g_netlinkSocket, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        int savedErrno = errno;
        fprintf(stderr, "Failed to bind netlink socket: %s (errno %d)\n", strerror(savedErrno), savedErrno);
        close(g_netlinkSocket);
        g_netlinkSocket = -1;
        return;
    }

    // Create eventfd to signal thread to stop
    // eventfd is more efficient than pipe (single fd, 8-byte counter, lighter weight)
    // EFD_CLOEXEC prevents fd leak to child processes
    g_stopEventFd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (g_stopEventFd < 0) {
        int savedErrno = errno;
        fprintf(stderr, "Failed to create stop eventfd: %s (errno %d)\n", strerror(savedErrno), savedErrno);
        close(g_netlinkSocket);
        g_netlinkSocket = -1;
        return;
    }

    // Start monitoring thread
    g_monitorRunning.store(true, std::memory_order_release);

    // Create thread with explicit attributes for better control
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE);

    int err = pthread_create(&g_monitorThread, &attr, netlinkMonitorThread, nullptr);
    pthread_attr_destroy(&attr);

    if (err != 0) {
        fprintf(stderr, "Failed to create monitor thread: %s (errno %d)\n", strerror(err), err);
        close(g_netlinkSocket);
        g_netlinkSocket = -1;
        close(g_stopEventFd);
        g_stopEventFd = -1;
        g_monitorRunning.store(false, std::memory_order_release);
        return;
    }

    fprintf(stderr, "Network monitoring started (netlink + poll)\n");
}

void stopNetworkMonitoring() {
        if (g_monitorRunning.load(std::memory_order_acquire)) {
        g_monitorRunning.store(false, std::memory_order_release);

        // Signal thread to stop via eventfd
        // Write any non-zero value to wake up poll()
        if (g_stopEventFd >= 0) {
            uint64_t val = 1;
            ssize_t written = write(g_stopEventFd, &val, sizeof(val));
            (void)written;  // Ignore errors - thread will exit on timeout anyway
        }

        // Wait for thread to finish
        pthread_join(g_monitorThread, nullptr);

        // Clean up file descriptors
        if (g_netlinkSocket >= 0) {
            close(g_netlinkSocket);
            g_netlinkSocket = -1;
        }
        if (g_stopEventFd >= 0) {
            close(g_stopEventFd);
            g_stopEventFd = -1;
        }

        fprintf(stderr, "Network monitoring stopped\n");
    }

    // Clear callback under mutex to prevent race with monitor thread
    pthread_mutex_lock(&g_callbackMutex);
    g_networkCallback = nullptr;
    pthread_mutex_unlock(&g_callbackMutex);
}

void bringWindowToForeground(void* windowHandle) {
    // No-op on FreeBSD - like on Linux, window management is handled by the
    // window manager and applications cannot force themselves to the foreground
    Q_UNUSED(windowHandle);
}

bool hasElevatedPrivileges() {
    // Check if running as root (UID 0)
    return ::geteuid() == 0;
}

void attachConsole() {
    // No-op on FreeBSD - console is already available
}

const char* getBundlePath() {
    static thread_local char resolved[PATH_MAX];
    size_t len = sizeof(resolved);

    // Use sysctl to get the path of current executable
    const int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PATHNAME, -1};
    if (sysctl(mib, 4, resolved, &len, NULL, 0) == -1) {
        resolved[0] = '\0';
        return nullptr;
    }

    return resolved;
}

bool isElevatableBundle() {
    const char *path = getBundlePath();

    return path && access(path, F_OK) == 0;
}

// Internal helper to generate unique policy filename
static bool generatePolkitPolicyFilename(const char* appImagePath, char* buffer, size_t bufferSize) {
    if (!appImagePath || !buffer || bufferSize < 64) {
        return false;
    }

    // Generate a hash of the AppImage path to create a unique filename
    // This ensures each AppImage location gets its own policy file
    QByteArray pathBytes(appImagePath);
    QByteArray hash = QCryptographicHash::hash(pathBytes, QCryptographicHash::Md5).toHex();

    // Create filename: com.raspberrypi.rpi-imager.appimage-HASH.policy
    int written = std::snprintf(buffer, bufferSize,
        "com.raspberrypi.rpi-imager.appimage-%s.policy",
        hash.left(12).constData());  // Use first 12 chars of hash

    return written > 0 && static_cast<size_t>(written) < bufferSize;
}

// Internal helper to XML-escape a string for safe embedding in XML content
static QString xmlEscape(const QString& input) {
    QString result;
    result.reserve(input.size() + 16);  // Reserve a bit extra for escapes

    for (const QChar& c : input) {
        uint16_t codepoint = c.unicode();
        switch (codepoint) {
            case '&':  result += QStringLiteral("&amp;");  break;
            case '<':  result += QStringLiteral("&lt;");   break;
            case '>':  result += QStringLiteral("&gt;");   break;
            case '"':  result += QStringLiteral("&quot;"); break;
            case '\'': result += QStringLiteral("&apos;"); break;
            default:
                // Also escape control characters (except tab, newline, carriage return)
                if (codepoint < 0x20 && c != '\t' && c != '\n' && c != '\r') {
                    result += QString("&#x%1;").arg(codepoint, 0, 16);
                } else {
                    result += c;
                }
                break;
        }
    }
    return result;
}

// Polkit action directories in order of preference:
// - /etc/polkit-1/actions/ is the local override location (writable on immutable distros)
// - /usr/share/polkit-1/actions/ is the vendor location (read-only on immutable distros)
static const char* const POLKIT_ACTIONS_DIRS[] = {
    "/etc/polkit-1/actions",
    "/usr/share/polkit-1/actions",
    nullptr
};

// Internal helper to check if a polkit policy exists that authorizes pkexec
// to run the given binary path. Scans all .policy files in the standard polkit
// action directories for a matching exec.path annotation.
static bool hasPolkitPolicyForPath(const char* binaryPath) {
    if (!binaryPath) {
        return false;
    }

    // Build the search string: the exec.path annotation matching our binary
    QString escapedPath = xmlEscape(QString::fromUtf8(binaryPath));
    QByteArray searchString = QString("org.freedesktop.policykit.exec.path\">%1</annotate>")
        .arg(escapedPath).toUtf8();

    for (int i = 0; POLKIT_ACTIONS_DIRS[i]; i++) {
        QDir dir(POLKIT_ACTIONS_DIRS[i]);
        if (!dir.exists())
            continue;

        const QStringList policyFiles = dir.entryList(
            QStringList() << QStringLiteral("*.policy"), QDir::Files);
        for (const QString& filename : policyFiles) {
            QFile policyFile(dir.filePath(filename));
            if (!policyFile.open(QIODevice::ReadOnly))
                continue;

            QByteArray content = policyFile.readAll();
            policyFile.close();

            if (content.contains(searchString))
                return true;
        }
    }

    return false;
}

bool hasElevationPolicyInstalled() {
    const char* bundlePath = getBundlePath();
    if (!bundlePath) {
        return false;
    }
    return hasPolkitPolicyForPath(bundlePath);
}

// Internal helper to remove stale polkit policy files left behind when the
// AppImage was moved, renamed, or deleted. Called during policy installation
// (which runs as root) so we have write access to the polkit directories.
// Only touches files matching our naming convention (com.raspberrypi.rpi-imager.appimage-*.policy).
static void cleanupStalePolkitPolicies(const char* currentPath) {
    const QByteArray execPathTag("org.freedesktop.policykit.exec.path\">");
    const QByteArray closeTag("</annotate>");

    for (int i = 0; POLKIT_ACTIONS_DIRS[i]; i++) {
        QDir dir(POLKIT_ACTIONS_DIRS[i]);
        if (!dir.exists())
            continue;

        const QStringList policyFiles = dir.entryList(
            QStringList() << QStringLiteral("com.raspberrypi.rpi-imager.appimage-*.policy"),
            QDir::Files);

        for (const QString& filename : policyFiles) {
            QString fullPath = dir.filePath(filename);
            QFile file(fullPath);
            if (!file.open(QIODevice::ReadOnly))
                continue;

            QByteArray content = file.readAll();
            file.close();

            // Extract the exec.path value from the policy XML
            int tagStart = content.indexOf(execPathTag);
            if (tagStart < 0)
                continue;
            tagStart += execPathTag.size();
            int tagEnd = content.indexOf(closeTag, tagStart);
            if (tagEnd < 0)
                continue;

            QByteArray referencedPath = content.mid(tagStart, tagEnd - tagStart).trimmed();
            if (referencedPath.isEmpty())
                continue;

            // Keep the policy if it references the current AppImage path
            if (currentPath && referencedPath == currentPath)
                continue;

            // Remove if the referenced binary no longer exists
            struct stat st;
            if (stat(referencedPath.constData(), &st) != 0) {
                if (QFile::remove(fullPath)) {
                    std::fprintf(stderr, "Removed stale polkit policy: %s (target %s no longer exists)\n",
                                 qPrintable(fullPath), referencedPath.constData());
                }
            }
        }
    }
}

// Internal helper to install polkit policy for a specific path
static bool installPolkitPolicyForPath(const char* appImagePath) {
    if (!appImagePath || ::geteuid() != 0) {
        return false;
    }

    // Verify the AppImage path exists and is a regular file
    struct stat st;
    if (stat(appImagePath, &st) != 0 || !S_ISREG(st.st_mode)) {
        return false;
    }

    // Security: Validate the path doesn't contain suspicious characters
    // that could be used for XML injection attacks
    QString pathStr = QString::fromUtf8(appImagePath);

    // Reject paths with null bytes (could truncate strings in C code)
    if (pathStr.contains(QChar('\0'))) {
        // Log the path length and first portion for debugging (don't log full path - could be malicious)
        std::fprintf(stderr, "Security: Rejecting AppImage path with embedded null byte (length=%d, prefix=%.50s...)\n",
                     static_cast<int>(strlen(appImagePath)), appImagePath);
        return false;
    }

    // Clean up stale policy files from previous AppImage locations before installing the new one
    cleanupStalePolkitPolicies(appImagePath);

    char policyFilename[256];
    if (!generatePolkitPolicyFilename(appImagePath, policyFilename, sizeof(policyFilename))) {
        return false;
    }

    // Find a writable polkit actions directory
    // Prefer /etc/ (local overrides) over /usr/share/ (vendor, read-only on immutable distros)
    const char* targetDir = nullptr;
    for (int i = 0; POLKIT_ACTIONS_DIRS[i]; i++) {
        struct stat dirSt;
        if (stat(POLKIT_ACTIONS_DIRS[i], &dirSt) == 0 && S_ISDIR(dirSt.st_mode)) {
            targetDir = POLKIT_ACTIONS_DIRS[i];
            break;
        }
    }

    // If no directory exists, try to create the preferred one (with parent)
    if (!targetDir) {
        if (mkdir("/etc/polkit-1", 0755) != 0 && errno != EEXIST) {
            std::fprintf(stderr, "Failed to create /etc/polkit-1: %s\n", strerror(errno));
            return false;
        }
        if (mkdir("/etc/polkit-1/actions", 0755) == 0 || errno == EEXIST) {
            targetDir = "/etc/polkit-1/actions";
        } else {
            std::fprintf(stderr, "Failed to create /etc/polkit-1/actions: %s\n", strerror(errno));
            return false;
        }
    }

    char policyPath[512];
    std::snprintf(policyPath, sizeof(policyPath), "%s/%s", targetDir, policyFilename);

    // Generate unique action ID based on path hash
    QByteArray pathBytes(appImagePath);
    QByteArray hash = QCryptographicHash::hash(pathBytes, QCryptographicHash::Md5).toHex();
    QString actionId = QString("com.raspberrypi.rpi-imager.appimage.%1").arg(QString::fromUtf8(hash.left(12)));

    // Security: XML-escape the AppImage path to prevent XML injection attacks
    // An attacker-controlled path like "</annotate><evil>..." could break the XML
    QString escapedPath = xmlEscape(pathStr);

    // Create policy XML with escaped path
    QString policyContent = QString(
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<!DOCTYPE policyconfig PUBLIC\n"
        " \"-//freedesktop//DTD PolicyKit Policy Configuration 1.0//EN\"\n"
        " \"http://www.freedesktop.org/standards/PolicyKit/1.0/policyconfig.dtd\">\n"
        "<policyconfig>\n"
        "  <vendor>Raspberry Pi Ltd</vendor>\n"
        "  <vendor_url>https://www.raspberrypi.com/</vendor_url>\n"
        "  <action id=\"%1\">\n"
        "    <description>Run Raspberry Pi Imager</description>\n"
        "    <message>Authentication is required to run Raspberry Pi Imager</message>\n"
        "    <icon_name>rpi-imager</icon_name>\n"
        "    <defaults>\n"
        "      <allow_any>auth_admin</allow_any>\n"
        "      <allow_inactive>auth_admin</allow_inactive>\n"
        "      <allow_active>auth_admin_keep</allow_active>\n"
        "    </defaults>\n"
        "    <annotate key=\"org.freedesktop.policykit.exec.path\">%2</annotate>\n"
        "    <annotate key=\"org.freedesktop.policykit.exec.allow_gui\">true</annotate>\n"
        "  </action>\n"
        "</policyconfig>\n"
    ).arg(actionId, escapedPath);

    // Write policy file
    // O_CLOEXEC prevents fd leak to child processes after fork/exec
    int fd = open(policyPath, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) {
        int savedErrno = errno;
        std::fprintf(stderr, "Failed to create policy file %s: %s\n", policyPath, strerror(savedErrno));
        return false;
    }

    QByteArray contentBytes = policyContent.toUtf8();
    ssize_t written = write(fd, contentBytes.constData(), static_cast<size_t>(contentBytes.size()));

    if (written < 0 || static_cast<size_t>(written) != static_cast<size_t>(contentBytes.size())) {
        int savedErrno = errno;
        std::fprintf(stderr, "Failed to write policy file: %s\n", strerror(savedErrno));
        close(fd);
        unlink(policyPath);
        return false;
    }

    // fsync to ensure data is flushed to disk before close
    // This prevents data loss if system crashes immediately after
    // Also sync the parent directory to ensure the directory entry is persisted
    if (fsync(fd) != 0) {
        int savedErrno = errno;
        std::fprintf(stderr, "Failed to fsync policy file: %s\n", strerror(savedErrno));
        close(fd);
        unlink(policyPath);
        return false;
    }

    close(fd);

    // fsync parent directory to ensure the directory entry is persisted
    // This is often overlooked but necessary for full durability
    int dirFd = open(targetDir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dirFd >= 0) {
        fsync(dirFd);
        close(dirFd);
    }

    return true;
}

bool installElevationPolicy() {
    const char* bundlePath = getBundlePath();
    return bundlePath ? installPolkitPolicyForPath(bundlePath) : false;
}

bool launchDetached(const QString& program, const QStringList& arguments) {
    // Use double-fork pattern to create a fully detached process
    // that will outlive the parent and not become a zombie.
    //
    // Note: We do NOT call setsid() because that would create a new session
    // and break D-Bus/display connections needed for GUI applications.

    pid_t pid = fork();

    if (pid < 0) {
        qWarning() << "launchDetached: fork failed:" << strerror(errno);
        return false;
    }

    if (pid == 0) {
        // First child - fork again to fully detach
        pid_t pid2 = fork();

        if (pid2 < 0) {
            _exit(1);
        }

        if (pid2 > 0) {
            // First child exits, orphaning the grandchild (adopted by init)
            _exit(0);
        }

        // Grandchild - build argv and exec

        // Clear AppImage environment before running external tools
        // Even though FreeBSD does not have "appimages", the packaged image is
        // based on one and behaves similarly
        clearAppImageEnvironment();

        QByteArray programBytes = program.toUtf8();
        std::vector<QByteArray> argBytes;
        std::vector<char*> argv;

        argv.push_back(programBytes.data());
        for (const QString& arg : arguments) {
            argBytes.push_back(arg.toUtf8());
            argv.push_back(argBytes.back().data());
        }
        argv.push_back(nullptr);

        // Resolve to absolute path to prevent PATH hijack under elevated privileges.
        QByteArray resolvedProgram = programBytes;
        if (!programBytes.startsWith('/')) {
            static const char *searchDirs[] = {"/usr/bin/", "/bin/", "/usr/sbin/", "/sbin/", nullptr};
            for (const char **dir = searchDirs; *dir; ++dir) {
                QByteArray candidate = QByteArray(*dir) + programBytes;
                if (access(candidate.constData(), X_OK) == 0) {
                    resolvedProgram = candidate;
                    break;
                }
            }
        }
        execv(resolvedProgram.constData(), argv.data());
        _exit(127);  // exec failed
    }

    // Parent - wait for first child to exit
    int status;
    waitpid(pid, &status, 0);

    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

bool tryElevate(int argc, char** argv) {
    const char* bundlePath = getBundlePath();
    if (!bundlePath || hasElevatedPrivileges()) {
        return false;
    }

    if (access("/usr/bin/pkexec", X_OK) != 0 || !hasPolkitPolicyForPath(bundlePath)) {
        return false;
    }

    // --disable-internal-agent prevents pkexec from spawning its own polkit agent.
    // We rely on the desktop environment's agent (e.g., gnome-shell, kde-polkit)
    // to show the auth dialog. Without this flag, pkexec's built-in text-mode agent
    // can interfere with the GUI agent, causing duplicate prompts or hangs.
    char** newArgv = new char*[argc + 3];
    int newArgc = 0;

    newArgv[newArgc++] = strdup("/usr/bin/pkexec");
    newArgv[newArgc++] = strdup("--disable-internal-agent");
    newArgv[newArgc++] = strdup(bundlePath);

    for (int i = 1; i < argc; i++) {
        newArgv[newArgc++] = strdup(argv[i]);
    }
    newArgv[newArgc] = nullptr;

    pid_t pid = fork();

    if (pid < 0) {
        for (int i = 0; i < newArgc; i++) free(newArgv[i]);
        delete[] newArgv;
        return false;
    }

    if (pid == 0) {
        execv("/usr/bin/pkexec", newArgv);
        _exit(127);
    }

    int status;
    waitpid(pid, &status, 0);

    for (int i = 0; i < newArgc; i++) free(newArgv[i]);
    delete[] newArgv;

    if (WIFEXITED(status)) {
        int exitCode = WEXITSTATUS(status);
        if (exitCode == 0) {
            _exit(0);  // Elevated process completed successfully
        } else if (exitCode == 126 || exitCode == 127) {
            return false;  // User cancelled or auth failed
        } else {
            _exit(exitCode);
        }
    } else if (WIFSIGNALED(status)) {
        _exit(128 + WTERMSIG(status));
    }

    return false;
}

bool runElevatedPolicyInstaller() {
    const char* bundlePath = getBundlePath();
    if (!bundlePath) {
        return false;
    }

    // Use pkexec to run ourselves with --install-elevation-policy
    QStringList args;
    args << "--disable-internal-agent";
    args << QString::fromUtf8(bundlePath);
    args << "--install-elevation-policy";

    QProcess process;
    process.start("/usr/bin/pkexec", args);

    if (!process.waitForStarted(5000) || !process.waitForFinished(60000)) {
        process.kill();
        return false;
    }

    return process.exitCode() == 0;
}

void execElevated(const QStringList& extraArgs) {
    const char* bundlePath = getBundlePath();
    if (!bundlePath) {
        return;
    }

    // Build argument list for exec
    std::vector<std::string> argStrings;
    argStrings.push_back("pkexec");
    argStrings.push_back("--disable-internal-agent");
    argStrings.push_back(bundlePath);

    for (const QString& arg : extraArgs) {
        argStrings.push_back(arg.toStdString());
    }

    std::vector<char*> argv;
    for (auto& s : argStrings) {
        argv.push_back(const_cast<char*>(s.c_str()));
    }
    argv.push_back(nullptr);

    fflush(stdout);
    fflush(stderr);
    execv("/usr/bin/pkexec", argv.data());

    // Only reached if exec failed
    qWarning() << "Failed to exec pkexec:" << strerror(errno);
}

bool isScrollInverted(bool qtInvertedFlag) {
    // On Linux (and therefore on FreeBSD), Qt's inverted flag behavior varies
    // by desktop environment.  Most modern DEs (GNOME, KDE) correctly report
    // it, so we pass through.
    return qtInvertedFlag;
}

bool prefersReducedMotion() {
    // Use absolute paths for external commands — this function may run in an
    // elevated (root) context after pkexec.  Relative names would search PATH,
    // which could be user-controlled.

    // GNOME: org.gnome.desktop.interface enable-animations
    // This is the standard accessibility setting on GNOME-based desktops.
    {
        const QString bin = QStringLiteral("/usr/bin/gsettings");
        if (QFileInfo::exists(bin)) {
            QProcess gsettings;
            gsettings.start(bin, {"get", "org.gnome.desktop.interface", "enable-animations"});
            if (gsettings.waitForFinished(500)) {
                QString value = gsettings.readAllStandardOutput().trimmed();
                if (value == "false") {
                    return true;
                }
            }
        }
    }

    // KDE Plasma 5.67+: AnimationDurationFactor=0 disables animations
    // Prefer kreadconfig6 (Plasma 6), fallback to kreadconfig5 (Plasma 5).
    {
        QString bin = QStringLiteral("/usr/bin/kreadconfig6");
        if(!QFileInfo::exists(bin)) {
            bin = QStringLiteral("/usr/bin/kreadconfig5");
        }
        if (QFileInfo::exists(bin)) {
            QProcess kreadconfig;
            kreadconfig.start(bin, {"--group", "KDE", "--key", "AnimationDurationFactor"});
            if (kreadconfig.waitForFinished(500)) {
                QString value = kreadconfig.readAllStandardOutput().trimmed();
                bool ok = false;
                double factor = value.toDouble(&ok);
                if (ok && factor == 0.0) {
                    return true;
                }
            }
        }
    }

    // Fallback: GTK3 settings file — works on Raspberry Pi OS, XFCE, MATE,
    // LXDE, LXQt, and any other GTK-based desktop without GSettings.
    // Users can set gtk-enable-animations=0 in ~/.config/gtk-3.0/settings.ini
    {
        QString gtkSettingsPath = QDir::homePath() + "/.config/gtk-3.0/settings.ini";
        QFile gtkSettings(gtkSettingsPath);
        if (gtkSettings.open(QIODevice::ReadOnly | QIODevice::Text)) {
            while (!gtkSettings.atEnd()) {
                QString line = gtkSettings.readLine().trimmed();
                if (line.startsWith("gtk-enable-animations")) {
                    int eq = line.indexOf('=');
                    if (eq >= 0) {
                        QString value = line.mid(eq + 1).trimmed();
                        if (value == "0" || value == "false") {
                            return true;
                        }
                    }
                    break;
                }
            }
        }
    }

    return false;
}

QString getWriteDevicePath(const QString& devicePath) {
    return devicePath;
}

QString getEjectDevicePath(const QString& devicePath) {
    return devicePath;
}

static bool isDisk(const QString& device) {
    QByteArray deviceBytes = device.toUtf8();
    QString devicePath = g_device_path(deviceBytes.constData());

    struct gmesh devtree;
    struct gclass *geom_class;
    struct ggeom *geom;

    bool found = false;

    int error = geom_gettree(&devtree);
    if (error != 0) {
        qWarning() << "PlatformQuirks::isDisk: Failed to open GEOM device tree"
                   << ", &devtree=" << &devtree
                   << ", errno=" << error << " (" << std::strerror(error) << ")";
        return false;
    }

    LIST_FOREACH(geom_class, &devtree.lg_class, lg_class) {
        if (QString(geom_class->lg_name) == "DISK") {
            LIST_FOREACH(geom, &geom_class->lg_geom, lg_geom) {
                found = (g_device_path(geom->lg_name) == devicePath);

                if (found) {
                    break;
                }
            }

            break;
        }
    }

    return found;
}

DiskResult unmountDisk(const QString& device) {
    QByteArray deviceBytes = device.toUtf8();
    const char* devicePath = g_device_path(deviceBytes.constData());

    struct gmesh devtree;
    struct gclass *geom_class;
    struct ggeom *geom;
    struct gconsumer *consumer;
    struct gprovider *provider;

    bool foundDisk = false;
    bool foundPartition = false;

    if (devicePath == NULL) {
        qDebug() << "PlatformQuirks::unmountDisk: Device with name '"
                 << devicePath
                 << "' does not appear to exist in the geom hierarchy";
        return DiskResult::InvalidDrive;
    }

    std::vector<struct gprovider*> mountPartitions;

    int error = geom_gettree(&devtree);
    if (error != 0) {
        qWarning() << "PlatformQuirks::unmountDisk: Failed to open GEOM device tree"
                   << ", &devtree=" << &devtree
                   << ", errno=" << error << " (" << std::strerror(error) << ")";
        return DiskResult::Error;
    }
    LIST_FOREACH(geom_class, &devtree.lg_class, lg_class) {
        if (QString(geom_class->lg_name) != "PART") {
            continue;
        }

        LIST_FOREACH(geom, &geom_class->lg_geom, lg_geom) {
            if (QString(g_device_path(geom->lg_name)) == devicePath) {
                foundDisk = true; // The current geom object is the disk we seek
            }

            // Look through partitions and add them to the unmount list
            LIST_FOREACH(provider, &geom->lg_provider, lg_provider) {
                if (foundDisk) {
                    // Unmount every partition on the disk
                    mountPartitions.push_back(provider);
                } else if (QString(g_device_path(provider->lg_name)) == devicePath) {
                    // The current partition is the device we seek
                    foundPartition = true;
                    mountPartitions.push_back(provider);
                    break;
                }
            }

            if (foundDisk || foundPartition) {
                break;
            }
        }

        if (foundDisk || foundPartition) {
            break;
        }
    }
    geom_deletetree(&devtree);

    if (!foundDisk && !foundPartition) {
        qDebug() << "unmountDisk: Failed to find partition table associated with"
                 << devicePath;
        return DiskResult::InvalidDrive;
    }

    std::vector<struct gprovider*> failedUnmounts;

    for (const auto partition : mountPartitions) {
        char *unmountPath = g_device_path(partition->lg_name);
        if (unmount(unmountPath, 0) == -1) {
            qDebug() << "PlatformQuirks::unmountDisk: unmounted" << unmountPath << "(normal, first try)";
        } else if (unmount(unmountPath, 0) == -1) {
            qDebug() << "PlatformQuirks::unmountDisk: unmounted" << unmountPath << "(normal, second try)";
        } else if (unmount(unmountPath, MNT_FORCE) == -1) {
            qDebug() << "PlatformQuirks::unmountDisk: unmounted" << unmountPath << "(MNT_FORCE; may lead to data loss)";
        } else {
            failedUnmounts.push_back(partition);
            qWarning() << "PlatformQuirks::unmountDisk: Failed to unmount partition"
                       << ", partition name=" << partition->lg_name
                       << ", errno=" << errno << " (" << std::strerror(errno) << ")";
        }
    }

    if (failedUnmounts.size() == 0) {
        return DiskResult::Success;
    } else if (failedUnmounts.size() < mountPartitions.size()) {
        qWarning() << "PlatformQuirks::unmountDisk: Partial failure"
                   << ", total=" << mountPartitions.size()
                   << ", failed=" << failedUnmounts.size()
                   << ", failed list:";

        for (const auto failed : failedUnmounts) {
            qWarning() << " -" << g_device_path(failed->lg_name);
        }
    } else {
        qWarning() << "PlatformQuirks::unmountDisk: Failed to unmount all partitions"
                   << ", total=" << mountPartitions.size();
    }
    return DiskResult::Busy;
}

DiskResult ejectDisk(const QString& device) {
    return unmountDisk(device);
}

DiskResult refreshDiskView(const QString& device) {
    Q_UNUSED(device);
    return DiskResult::Success;
}

const char* findCACertBundle()
{
    return nullptr;
}

void clearAppImageEnvironment() {
    // AppImages set LD_LIBRARY_PATH and LD_PRELOAD to use bundled libraries.
    // External tools need system libraries instead, otherwise they may fail
    // due to symbol conflicts (e.g., PAM modules failing with "cannot open
    // session: Module is unknown", or KDE tools failing with Qt version
    // mismatches like "version `Qt_6.10' not found").
    //
    // This is safe because forked children running external tools don't need
    // our bundled libraries.
    unsetenv("LD_LIBRARY_PATH");
    unsetenv("LD_PRELOAD");
}

qreal detectTextScaleFactor()
{
    return 1.0;
}

qreal fontDpiCorrection()
{
    return 72.0 / 96.0;
}

} // namespace PlatformQuirks
