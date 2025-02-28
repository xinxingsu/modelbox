/*
 * Copyright 2021 The Modelbox Project Authors. All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */


#include "modelbox/device/cpu/cpu_memory.h"

#include <securec.h>

#include "modelbox/base/collector.h"
#include "modelbox/base/os.h"

namespace modelbox {
CpuMemory::CpuMemory(const std::shared_ptr<Device> &device,
                     const std::shared_ptr<DeviceMemoryManager> &mem_mgr,
                     std::shared_ptr<void> device_mem_ptr, size_t size)
    : DeviceMemory(device, mem_mgr, device_mem_ptr, size, true) {}

Status CpuMemory::ReadFrom(
    const std::shared_ptr<const DeviceMemory> &src_memory, size_t src_offset,
    size_t src_size, size_t dest_offset) {
  if (!CheckReadFromParam(src_memory, src_offset, src_size, dest_offset)) {
    MBLOG_ERROR << "Check read param failed";
    return STATUS_INVALID;
  }

  auto mem_mgr = src_memory->mem_mgr_;
  return mem_mgr->DeviceMemoryCopy(shared_from_this(), dest_offset, src_memory,
                                   src_offset, src_size,
                                   DeviceMemoryCopyKind::ToHost);
}

Status CpuMemory::Verify() const {
  auto mem_size = offset_ + capacity_;
  if (mem_size == 0) {
    return STATUS_OK;
  }

  auto magic_code = (uint64_t *)((uint8_t *)device_mem_ptr_.get() + mem_size);
  if (MEM_MAGIC_CODE != *magic_code) {
    MBLOG_ERROR << "Host memory verify failed, magic code wrong";
    return STATUS_FAULT;
  }

  return STATUS_OK;
}

CpuMemoryPool::CpuMemoryPool() {}

Status CpuMemoryPool::Init() {
  auto status = InitSlabCache();
  if (!status) {
    return {status, "init mempool failed."};
  }

  return STATUS_OK;
}

CpuMemoryPool::~CpuMemoryPool() {
  if (flush_timer_) {
    flush_timer_->Stop();
    flush_timer_ = nullptr;
  }
}

void CpuMemoryPool::OnTimer() {
  // TODO support config shrink time.
}

void *CpuMemoryPool::MemAlloc(size_t size) {
  auto cpu_mem_ptr = (uint8_t *)malloc(size);
  if (cpu_mem_ptr == nullptr) {
    MBLOG_ERROR << "cpu_mem_ptr is null";
  }
  return cpu_mem_ptr;
}

void CpuMemoryPool::MemFree(void *ptr) { free(ptr); }

CpuMemoryManager::CpuMemoryManager(const std::string &device_id)
    : DeviceMemoryManager(device_id) {
  mem_pool_ = std::make_shared<CpuMemoryPool>();
  mem_pool_->RegisterCollector("cpu");
}

CpuMemoryManager::~CpuMemoryManager() {
  mem_pool_->DestroySlabCache();
  mem_pool_->UnregisterCollector("cpu");
}

Status CpuMemoryManager::Init() { return mem_pool_->Init(); }

std::shared_ptr<DeviceMemory> CpuMemoryManager::MakeDeviceMemory(
    const std::shared_ptr<Device> &device, std::shared_ptr<void> mem_ptr,
    size_t size) {
  return std::make_shared<CpuMemory>(device, shared_from_this(), mem_ptr, size);
}

std::shared_ptr<void> CpuMemoryManager::AllocSharedPtr(size_t size,
                                                       uint32_t mem_flags) {
  auto mem_size = size + sizeof(DeviceMemory::MEM_MAGIC_CODE);
  auto cpu_mem_ptr = mem_pool_->AllocSharedPtr(mem_size);
  if (cpu_mem_ptr == nullptr) {
    MBLOG_ERROR << "Cpu malloc failed, size " << mem_size;
    return nullptr;
  }

  *((uint64_t *)((u_char *)cpu_mem_ptr.get() + size)) =
      DeviceMemory::MEM_MAGIC_CODE;
  return cpu_mem_ptr;
}

void *CpuMemoryManager::Malloc(size_t size, uint32_t mem_flags) {
  return mem_pool_->MemAlloc(size);
}

void CpuMemoryManager::Free(void *mem_ptr, uint32_t mem_flags) {
  mem_pool_->MemFree(mem_ptr);
}

Status CpuMemoryManager::Write(const void *host_data, size_t host_size,
                               void *device_buffer, size_t device_size) {
  return Copy(device_buffer, device_size, host_data, host_size);
}

Status CpuMemoryManager::Read(const void *device_data, size_t device_size,
                              void *host_buffer, size_t host_size) {
  return Copy(host_buffer, host_size, device_data, device_size);
}

Status CpuMemoryManager::Copy(void *dest, size_t dest_size,
                              const void *src_buffer, size_t src_size) {
  auto ret = memcpy_s(dest, dest_size, src_buffer, src_size);
  if (EOK != ret) {
    MBLOG_ERROR << "Cpu memcpy failed, ret " << ret << ", src size " << src_size
                << ", dest size " << dest_size;
    return STATUS_FAULT;
  }

  return STATUS_OK;
}

Status CpuMemoryManager::GetDeviceMemUsage(size_t *free, size_t *total) const {
  return os->GetMemoryUsage(free, total);
}

Status CpuMemoryManager::DeviceMemoryCopy(
    const std::shared_ptr<DeviceMemory> &dest_memory, size_t dest_offset,
    const std::shared_ptr<const DeviceMemory> &src_memory, size_t src_offset,
    size_t src_size, DeviceMemoryCopyKind copy_kind) {
  auto dest_ptr = dest_memory->GetPtr<uint8_t>().get() + dest_offset;
  auto src_ptr = src_memory->GetConstPtr<uint8_t>().get() + src_offset;
  auto ret = memcpy_s(dest_ptr, src_size, src_ptr, src_size);
  if (EOK != ret) {
    MBLOG_ERROR << "Cpu memcpy failed, ret " << ret << ", src size " << src_size
                << ", dest size " << src_size;
    return modelbox::STATUS_FAULT;
  }

  return STATUS_OK;
}

}  // namespace modelbox