/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright (C) 2025 Raspberry Pi Ltd
 */

#include "file_operations_freebsd.h"

#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/vnode.h>
#include <sys/file.h>
#include <sys/mount.h>
#include <sys/namei.h>
#include <errno.h>
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

// io_uring support (Linux 5.1+)
#ifdef HAVE_LIBURING
#include <liburing.h>
#endif

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
      async_write_offset_(0), io_uring_available_(false), ring_(nullptr), next_write_id_(1) {  // Start at 1, 0 is reserved for cancel operations
}

bool FreeBSDFileOperations::IsBlockDevicePath(const std::string& path) {
    return false;
}

FreeBSDFileOperations::~FreeBSDFileOperations() {
}

#ifdef HAVE_LIBURING
bool FreeBSDFileOperations::InitIOUring() {
    return false;
}

void FreeBSDFileOperations::CleanupIOUring() {
}

void FreeBSDFileOperations::ProcessCompletions(bool wait) {
}
#else
// Stubs when liburing is not available
bool FreeBSDFileOperations::InitIOUring() { return false; }
void FreeBSDFileOperations::CleanupIOUring() { pending_callbacks_.clear(); }
void FreeBSDFileOperations::ProcessCompletions(bool) {}
#endif

FileError FreeBSDFileOperations::OpenDevice(const std::string& path) {
    FileError result;
    return result;
}

FileError FreeBSDFileOperations::CreateTestFile(const std::string& path, std::uint64_t size) {
    return FileError::kSuccess;
}

FileError FreeBSDFileOperations::WriteAtOffset(
    std::uint64_t offset,
    const std::uint8_t* data,
    std::size_t size) {
    return FileError::kSuccess;
}

FileError FreeBSDFileOperations::GetSize(std::uint64_t& size) {
    return FileError::kSuccess;
}

bool FreeBSDFileOperations::IsOpen() const {
    return false;
}

FileError FreeBSDFileOperations::SetDirectIOEnabled(bool enabled) {
    return FileError::kSuccess;
}

FileError FreeBSDFileOperations::OpenInternal(const char* path, int flags, mode_t mode) {
    return FileError::kSuccess;
}

FileError FreeBSDFileOperations::WriteSequential(const std::uint8_t* data, std::size_t size) {
    return FileError::kSuccess;
}

FileError FreeBSDFileOperations::ReadSequential(std::uint8_t* data, std::size_t size, std::size_t& bytes_read) {
    return FileError::kSuccess;
}

FileError FreeBSDFileOperations::Seek(std::uint64_t position) {
    return FileError::kSuccess;
}

std::uint64_t FreeBSDFileOperations::Tell() const {
    return 0;
}

FileError FreeBSDFileOperations::ForceSync() {
    return FileError::kSuccess;
}

FileError FreeBSDFileOperations::Flush() {
    return FileError::kSuccess;
}

void FreeBSDFileOperations::PrepareForSequentialRead(std::uint64_t offset, std::uint64_t length) {
}

int FreeBSDFileOperations::GetHandle() const {
    return 0;
}

int FreeBSDFileOperations::GetLastErrorCode() const {
    return 0;
}

// ============= Async I/O Implementation (using io_uring) =============

bool FreeBSDFileOperations::SetAsyncQueueDepth(int depth) {
    return false;
}

FileError FreeBSDFileOperations::AsyncWriteSequential(const std::uint8_t* data, std::size_t size,
                                                     AsyncWriteCallback callback) {
    return FileError::kSuccess;
}

void FreeBSDFileOperations::PollAsyncCompletions() {
  // Intentionally a no-op on FreeBSD.
  //
  // Unlike Windows IOCP, io_uring's CQ is not thread-safe for multiple
  // consumers. The extract thread is the sole CQ consumer — it polls via
  // ProcessCompletions() inside AsyncWriteSequential() and
  // WaitForPendingWrites(). External callers (watchdog timer, download
  // thread's _updateBottleneckState) must not touch the CQ directly, as
  // concurrent peek/cqe_seen calls cause double-processing or skipped
  // completions.
  //
  // This is safe because the extract thread's blocking wait uses 100ms
  // timeouts with cancellation checks, so completions are always drained
  // promptly without external prodding.
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
    return false;
}

void FreeBSDFileOperations::ReduceQueueDepthForRecovery(int newDepth) {
}

// Query device I/O limits from sysfs without requiring an open file descriptor.
// Returns zero-initialized struct if the device path isn't a block device or sysfs is unavailable.
FileOperations::DeviceIOLimits QueryPlatformDeviceIOLimits(const std::string& path) {
    FileOperations::DeviceIOLimits limits;
    return limits;
}

// Platform-specific factory function implementation
std::unique_ptr<FileOperations> CreatePlatformFileOperations() {
  return std::make_unique<FreeBSDFileOperations>();
}

} // namespace rpi_imager
