/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright (C) 2025 Raspberry Pi Ltd
 */

#include "file_operations_freebsd.h"

#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/disk.h>
#include <errno.h>
#include <iostream>
#include <sstream>
#include <cstring>
#include <algorithm>
#include <thread>
#include <functional>
#include "../timeout_utils.h"

using rpi_imager::TimeoutResult;
using rpi_imager::TimeoutConfig;
using rpi_imager::runWithTimeout;
using rpi_imager::TimeoutDefaults::kSyncWriteTimeoutSeconds;
using rpi_imager::TimeoutDefaults::kSyncFsyncTimeoutSeconds;
using rpi_imager::TimeoutDefaults::kMinAsyncQueueDepth;
using rpi_imager::TimeoutDefaults::kHighLatencyThresholdMs;
using rpi_imager::TimeoutDefaults::kAsyncFirstCompletionTimeoutMs;

#include <fstream>

namespace rpi_imager {

// Forward declaration — defined at bottom of file, called from OpenDevice()
FileOperations::DeviceIOLimits QueryPlatformDeviceIOLimits(const std::string& path);

// Use the common logging function from file_operations.cpp
static void Log(const std::string& msg) {
    FileOperationsLog(msg);
}

FreeBSDFileOperations::FreeBSDFileOperations()
    : fd_(-1), last_error_code_(0), using_direct_io_(false), direct_io_attempted_(false),
      async_queue_depth_(1), pending_writes_(0), cancelled_(false), first_async_error_(FileError::kSuccess),
      async_write_offset_(0), next_write_id_(1) {  // Start at 1, 0 is reserved for cancel operations
}

bool FreeBSDFileOperations::IsBlockDevicePath(const std::string& path) {
    // Disk devices are generally prefixed by /dev/
    return (path.find("/dev/") == 0);
}

FileError FreeBSDFileOperations::OpenDevice(const std::string& path) {
    std::cout << "FreeBSDFileOperations::OpenDevice: "
              << "Opening FreeBSD device: " << path
              << std::endl;

    // Reset direct I/O tracking for new device
    direct_io_attempted_ = false;

    // Use O_DIRECT for block devices to bypass the page cache
    int flags = O_RDWR;
    bool isBlockDevice = IsBlockDevicePath(path);

    if (isBlockDevice) {
        flags |= O_DIRECT;
        using_direct_io_ = true;
        direct_io_attempted_ = true;
    }

    FileError result = OpenInternal(path.c_str(), flags);

    // If O_DIRECT fails, fall back to regular I/O
    if (result != FileError::kSuccess && isBlockDevice && using_direct_io_) {
        using_direct_io_ = false;
        result = OpenInternal(path.c_str(), O_RDWR);
    }

    // Reset async state for new file
    async_write_offset_ = 0;
    first_async_error_ = FileError::kSuccess;
    cancelled_.store(false);
    write_latency_stats_.reset();

    if (result == FileError::kSuccess) {
        device_io_limits_ = QueryPlatformDeviceIOLimits(current_path_);
        if (device_io_limits_.max_transfer_bytes > 0 || device_io_limits_.suggested_queue_depth > 0) {
            std::ostringstream oss;
            oss << "Device I/O limits: max_transfer=" << device_io_limits_.max_transfer_bytes
                << " bytes, suggested_queue_depth=" << device_io_limits_.suggested_queue_depth;
            Log(oss.str());
        }
    }

    return result;
}

FileError FreeBSDFileOperations::CreateTestFile(const std::string& path, std::uint64_t size) {
    // Create an empty file with 644 permissions
    FileError result = OpenInternal(path.c_str(),
                                    O_CREAT | O_RDWR | O_TRUNC,
                                    S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
    if (result != FileError::kSuccess) {
        return result;
    }

    // Extend file to desired size
    if (ftruncate(fd_, static_cast<off_t>(size)) != 0) {
        std::cerr << "FreeBSDFileOperations::CreateTestFile: "
                  << "Failed to increase file size to " << size
                  << ", errno=" << errno << " (" << std::strerror(errno) << ")"
                  << std::endl;
        Close();
        return FileError::kSizeError;
    }

    return FileError::kSuccess;
}

FileError FreeBSDFileOperations::WriteAtOffset(std::uint64_t offset,
                                               const std::uint8_t* data,
                                               std::size_t size) {
    if (!IsOpen()) {
        std::cerr << "FreeBSDFileOperations::WriteAtOffset: "
                  << "Device not open"
                  << std::endl;
        return FileError::kOpenError;
    }

    if (lseek(fd_, static_cast<off_t>(offset), SEEK_SET) == -1) {
        std::cerr << "FreeBSDFileOperations::WriteAtOffset: "
                  << "lseek failed"
                  << ", offset=" << offset
                  << ", errno=" << errno << " (" << std::strerror(errno) << ")"
                  << std::endl;
        return FileError::kSeekError;
    }

    std::size_t bytes_written = 0;
    while (bytes_written < size) {
        ssize_t result = write(fd_, data + bytes_written, size - bytes_written);
        if (result <= 0) {
            std::cerr << "FreeBSDFileOperations::WriteAtOffset: "
                      << "write failed"
                      << ", bytes_written=" << bytes_written
                      << ", errno=" << errno << " (" << std::strerror(errno) << ")"
                      << std::endl;
            return FileError::kWriteError;
        }
        bytes_written += static_cast<std::size_t>(result);
    }

    return FileError::kSuccess;
}

FileError FreeBSDFileOperations::GetSize(std::uint64_t& size) {
    if (!IsOpen()) {
        std::cerr << "FreeBSDFileOperations::GetSize: "
                  << "Device not open"
                  << std::endl;
        return FileError::kOpenError;
    }

    struct stat st;
    if (fstat(fd_, &st) != 0) {
        std::cerr << "FreeBSDFileOperations::GetSize: "
                  << "Failed to get file status"
                  << ", errno=" << errno << " (" << std::strerror(errno) << ")"
                  << std::endl;
        last_error_code_ = errno;
        return FileError::kSizeError;
    }

    if (S_ISBLK(st.st_mode) || S_ISCHR(st.st_mode)) {
        std::uint64_t device_size = 0;
        if (ioctl(fd_, DIOCGMEDIASIZE, &device_size) == -1) {
            std::cerr << "FreeBSDFileOperations::GetSize: "
                      << "Failed to get device size"
                      << ", errno=" << errno << " (" << std::strerror(errno) << ")"
                      << std::endl;
            last_error_code_ = errno;
            return FileError::kSizeError;
        }
        size = device_size;
    } else {
        size = static_cast<std::uint64_t>(st.st_size);
    }

    return FileError::kSuccess;
}

FileError FreeBSDFileOperations::Close() {
    WaitForPendingWrites();

    if (IsOpen()) {
        if (close(fd_) != 0) {
            std::cerr << "FreeBSDFileOperations::Close: "
                      << "Failed to close fd"
                      << ", errno=" << errno << " (" << std::strerror(errno) << ")"
                      << std::endl;
            fd_ = -1;
            using_direct_io_ = false;
            return FileError::kCloseError;
        }
        fd_ = -1;
    }

    current_path_.clear();
    using_direct_io_ = false;
    async_write_offset_ = 0;
    return FileError::kSuccess;
}

bool FreeBSDFileOperations::IsOpen() const {
    return fd_ >= 0;
}

FileError FreeBSDFileOperations::SetDirectIOEnabled(bool enabled) {
    if (!IsOpen() || current_path_.empty()) {
        std::cerr << "FreeBSDFileOpeartions::SetDirectIOEnabled: "
                  << "File not open"
                  << std::endl;
        return FileError::kOpenError;
    }

    if (using_direct_io_ == enabled) {
        return FileError::kSuccess;
    }

    // Save current position before reopening
    off_t currentPos = lseek(fd_, 0, SEEK_CUR);
    std::string savedPath = current_path_;

    close(fd_);
    fd_ = -1;

    int flags = O_RDWR;
    if (enabled && IsBlockDevicePath(savedPath)) {
        flags |= O_DIRECT;
    }

    FileError result = OpenInternal(savedPath.c_str(), flags);
    if (result != FileError::kSuccess) {
        if (enabled) {
            result = OpenInternal(savedPath.c_str(), O_RDWR);
            using_direct_io_ = false;
            Log("Failed to enable O_DIRECT, reopened without it");
        }
        if (result != FileError::kSuccess) {
            return result;
        }
    } else {
        using_direct_io_ = enabled;
    }

    if (currentPos > 0) {
        lseek(fd_, currentPos, SEEK_SET);
    }

    std::ostringstream oss;
    oss << "O_DIRECT " << (using_direct_io_ ? "enabled" : "disabled");
    Log(oss.str());

    return FileError::kSuccess;
}

FileError FreeBSDFileOperations::OpenInternal(const char* path, int flags, mode_t mode) {
    Close();

    fd_ = open(path, flags, mode);
    if (fd_ < 0) {
        last_error_code_ = errno;
        return FileError::kOpenError;
    }

    current_path_ = path;
    last_error_code_ = 0;
    return FileError::kSuccess;
}

FileError FreeBSDFileOperations::WriteSequential(const std::uint8_t* data, std::size_t size) {
    if (!IsOpen()) {
        std::cerr << "FreeBSDFileOperations::WriteSequential: "
                  << "File not open"
                  << std::endl;
        return FileError::kOpenError;
    }

    std::size_t bytes_written = 0;
    while (bytes_written < size) {
        ssize_t result = write(fd_, data + bytes_written, size - bytes_written);
        if (result <= 0) {
            if (result == 0 || errno != EINTR) {
                std::cerr << "FreeBSDFileOperations::WriteSequential: "
                          << "Failed to write to file"
                          << ", errno=" << errno << " (" << std::strerror(errno) << ")"
                          << std::endl;
                last_error_code_ = errno;
                return FileError::kWriteError;
            }
            continue;
        }
        bytes_written += static_cast<std::size_t>(result);
    }

    last_error_code_ = 0;

    // Update async_write_offset_ so Tell() returns correct position
    // This is needed because Seek() sets async_write_offset_, and Tell()
    // uses it if > 0. Without this update, Tell() would return a stale value.
    async_write_offset_ += size;

    return FileError::kSuccess;
}

FileError FreeBSDFileOperations::ReadSequential(std::uint8_t* data, std::size_t size, std::size_t& bytes_read) {
    if (!IsOpen()) {
        std::cerr << "FreeBSDFileOperations::ReadSequential: "
                  << "File not open"
                  << std::endl;
        return FileError::kOpenError;
    }

    ssize_t result = read(fd_, data, size);
    if (result < 0) {
        std::cerr << "FreeBSDFileOperations::ReadSequential: "
                  << "Failed to read from fd"
                  << ", errno=" << errno << " (" << std::strerror(errno) << ")"
                  << std::endl;
        bytes_read = 0;
        return FileError::kReadError;
    }

    bytes_read = static_cast<std::size_t>(result);
    return FileError::kSuccess;
}

FileError FreeBSDFileOperations::Seek(std::uint64_t position) {
    if (!IsOpen()) {
        std::cerr << "FreeBSDFileOperations::Seek: "
                  << "File not open"
                  << std::endl;
        return FileError::kOpenError;
    }

    // Wait for pending async writes before seeking
    WaitForPendingWrites();

    if (lseek(fd_, static_cast<off_t>(position), SEEK_SET) == -1) {
        std::cerr << "FreeBSDFileOperations::Seek: "
                  << "Seek failed"
                  << ", position=" << position
                  << ", errno=" << errno << " (" << std::strerror(errno) << ")"
                  << std::endl;
        return FileError::kSeekError;
    }

    // Also update async write offset
    async_write_offset_ = position;

    return FileError::kSuccess;
}

std::uint64_t FreeBSDFileOperations::Tell() const {
    if (!IsOpen()) {
        std::cerr << "FreeBSDFileOperations::Tell: "
                  << "File not open"
                  << std::endl;
        return 0;
    }

    if (async_write_offset_ > 0) {
        return async_write_offset_;
    }

    off_t pos = lseek(fd_, 0, SEEK_CUR);
    return (pos == -1) ? 0 : static_cast<std::uint64_t>(pos);
}

FileError FreeBSDFileOperations::ForceSync() {
    if (!IsOpen()) {
        std::cerr << "FreeBSDFileOperations::ForceSync: "
                  << "File not open"
                  << std::endl;
        return FileError::kOpenError;
    }

    WaitForPendingWrites();

    if (fsync(fd_) != 0) {
        std::cerr << "FreeBSDFileOperations::ForceSync: "
                  << "Sync failed"
                  << ", errno=" << errno << " (" << std::strerror(errno) << ")"
                  << std::endl;
        return FileError::kSyncError;
    }

    return FileError::kSuccess;
}

FileError FreeBSDFileOperations::Flush() {
    if (!IsOpen()) {
        std::cerr << "FreeBSDFileOperations::Flush: "
                  << "File not open"
                  << std::endl;
        return FileError::kOpenError;
    }

    WaitForPendingWrites();

    if (fdatasync(fd_) != 0) {
        std::cerr << "FreeBSDFileOperations::Flush: "
                  << "Sync failed"
                  << ", errno=" << errno << " (" << std::strerror(errno) << ")"
                  << std::endl;
        return FileError::kFlushError;
    }

    return FileError::kSuccess;
}

void FreeBSDFileOperations::PrepareForSequentialRead(std::uint64_t offset, std::uint64_t length) {
    if (!IsOpen()) {
        std::cerr << "FreeBSDFileOperations::PrepareForSequentialRead: "
                  << "File not open"
                  << std::endl;
        return;
    }

    // Invalidate cache and set up read-ahead
    int ret = posix_fadvise(fd_, static_cast<off_t>(offset), static_cast<off_t>(length), POSIX_FADV_DONTNEED);
    if (ret != 0) {
        std::ostringstream oss;
        oss << "Warning: posix_fadvise(DONTNEED) failed: " << ret;
        Log(oss.str());
    }

    ret = posix_fadvise(fd_, static_cast<off_t>(offset), static_cast<off_t>(length), POSIX_FADV_SEQUENTIAL);
    if (ret != 0) {
        std::ostringstream oss;
        oss << "Warning: posix_fadvise(SEQUENTIAL) failed: " << ret;
        Log(oss.str());
    }
}

int FreeBSDFileOperations::GetHandle() const {
    return fd_;
}

int FreeBSDFileOperations::GetLastErrorCode() const {
    return last_error_code_;
}

// ============= TODO: Async I/O Implementation (using aio) =============

bool FreeBSDFileOperations::SetAsyncQueueDepth(int depth) {
    return false;
}

FileError FreeBSDFileOperations::AsyncWriteSequential(const std::uint8_t* data, std::size_t size,
                                                      AsyncWriteCallback callback) {
    return FileError::kSuccess;
}

void FreeBSDFileOperations::PollAsyncCompletions() {
}

void FreeBSDFileOperations::CancelAsyncIO() {
}

FileError FreeBSDFileOperations::WaitForPendingWrites() {
    return FileError::kSuccess;
}

// GetAsyncIOStats() inherited from FileOperations base class

std::vector<FileOperations::PendingWriteInfo> FreeBSDFileOperations::GetPendingWritesSorted() const {
    std::vector<PendingWriteInfo> result;
    return result;
}

FileError FreeBSDFileOperations::AttemptSyncFallback() {
    return FileError::kSuccess;
}

bool FreeBSDFileOperations::DrainAndSwitchToSync(int stallTimeoutSeconds) {
    (void)stallTimeoutSeconds;
    sync_fallback_mode_ = true;
    return true;
}

void FreeBSDFileOperations::ReduceQueueDepthForRecovery(int newDepth) {
    (void)newDepth;
}

FileOperations::DeviceIOLimits QueryPlatformDeviceIOLimits(const std::string& path) {
    // TODO: Determine FreeBSD disk I/O parameters
    (void)path;
    return {};
}

// Platform-specific factory function implementation
std::unique_ptr<FileOperations> CreatePlatformFileOperations() {
  return std::make_unique<FreeBSDFileOperations>();
}

} // namespace rpi_imager
