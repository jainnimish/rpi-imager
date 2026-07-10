/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright (C) 2025 Raspberry Pi Ltd
 */

#ifndef FILE_OPERATIONS_FREEBSD_H_
#define FILE_OPERATIONS_FREEBSD_H_

#include "../unix/file_operations_unix.h"
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <vector>
#include <unordered_map>

namespace rpi_imager {

// FreeBSD implementation using POSIX file operations
class FreeBSDFileOperations : public UnixFileOperations {
 public:
  FreeBSDFileOperations();
  ~FreeBSDFileOperations() override;

  // Non-copyable, non-movable
  FreeBSDFileOperations(const FreeBSDFileOperations&) = delete;
  FreeBSDFileOperations& operator=(const FreeBSDFileOperations&) = delete;
  FreeBSDFileOperations(FreeBSDFileOperations&&) = delete;
  FreeBSDFileOperations& operator=(FreeBSDFileOperations&&) = delete;

  // ============= Async I/O API (FreeBSD: using aio) =============
  bool SetAsyncQueueDepth(int depth) override;
  bool IsAsyncIOSupported() const override { return true; }
  FileError AsyncWriteSequential(const std::uint8_t* data, std::size_t size,
                                  AsyncWriteCallback callback = nullptr) override;
  FileError WaitForPendingWrites() override;
  void CancelAsyncIO() override;

 private:
  int is_destr_;

  static bool IsBlockDevicePath(const std::string& path) override;
  FileError GetDeviceSize(std::uint64_t& size) override;
  DeviceIOLimits QueryPlatformDeviceIOLimits(const std::string& path) override;
  void ProcessCompletions(bool wait) override;
  FileError AttemptSyncFallback() override;
  bool DrainAndSwitchToSync(int timeoutSeconds) override;
};

} // namespace rpi_imager

#endif // FILE_OPERATIONS_FREEBSD_H_
