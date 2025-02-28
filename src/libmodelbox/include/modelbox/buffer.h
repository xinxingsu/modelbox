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

#ifndef MODELBOX_BUFFER_H_
#define MODELBOX_BUFFER_H_

#include <modelbox/base/any.h>
#include <modelbox/base/device.h>
#include <modelbox/base/log.h>
#include <modelbox/base/utils.h>
#include <modelbox/buffer_type.h>
#include <modelbox/stream.h>

#include <algorithm>
#include <atomic>
#include <memory>

namespace modelbox {
class SessionContext;
class Buffer;
class BufferList;

/**
 * @brief Meta of buffer
 */
class BufferMeta {
 public:
  BufferMeta();

  /**
   * @brief Copy constructor
   */
  BufferMeta(const BufferMeta& other);
  virtual ~BufferMeta();

  /**
   * @brief Save current flowuint error to buffer
   * @param e flowunit error
   * @return reference of current buffer meta
   */
  BufferMeta& SetError(const std::shared_ptr<FlowUnitError>& e);
  BufferMeta& SetStreamMetaContent(const std::string& key,
                                   const std::shared_ptr<void>& content);

  const std::shared_ptr<FlowUnitError>& GetError() const;

  /**
   * @brief Copy meta from anoter buffer meta
   * @param buf_meta other buffer meta
   * @param is_override override existing meta key
   * @return copy result
   */
  Status CopyMeta(const std::shared_ptr<BufferMeta> buf_meta,
                  bool is_override = false);

  /**
   * @brief Set meta key pair
   * @param key meta key
   * @param value meta value
   */
  template <typename T>
  void Set(const std::string& key, T&& value) {
    custom_meta_.Set(key, value);
  }

  /**
   * @brief Get value of key
   * @param key meta key
   * @param value meta value
   * @return whether the key exists
   */
  template <typename T>
  bool Get(const std::string& key, T&& value) {
    return custom_meta_.Get(key, value);
  }

  /**
   * @brief Get value of key return tuple
   * @param key meta key
   * @return meta tuple
   */
  std::tuple<Any*, bool> Get(const std::string& key) {
    return custom_meta_.Get(key);
  }

  /**
   * @brief Copy meta
   * @param other other meta
   * @return reference of current buffer meta
   */
  BufferMeta& operator=(const BufferMeta& other);

  /**
   * @brief Deep copy meta
   * @param other other meta
   * @return reference of current buffer meta
   */
  BufferMeta& DeepCopy(const BufferMeta& other);

 private:
  Collection custom_meta_;
  std::shared_ptr<FlowUnitError> error_;
};

/**
 * @brief Buffer type enum
 */
enum class BufferEnumType {
  /// RAW data
  RAW = 0,
  /// Image
  IMG = 1,
  /// String
  STR = 2,
};

/**
 * @brief Data buffer, the basic unit of data processing in the flow
 */
class Buffer : public std::enable_shared_from_this<Buffer> {
 public:
  /**
   * @brief New data buffer
   */
  Buffer();

  /**
   * @brief Create a new buffer related with specific device
   * @param device related device
   * @param dev_mem_flags Flags to create device memory
   */
  Buffer(const std::shared_ptr<Device>& device, uint32_t dev_mem_flags = 0);

  /**
   * @brief Create a new buffer related with specific device memory
   * @param dev_mem related device memory
   */
  Buffer(const std::shared_ptr<DeviceMemory>& dev_mem);

  /**
   * @brief Copy constructor, copy meta from other buffer
   * @param buffer other buffer
   */
  Buffer(const Buffer& buffer);

  virtual ~Buffer() = default;

  /**
   * @brief Create a buffer, apply for memory
   * @param size memory size
   * @return create result
   */
  virtual Status Build(size_t size);

  /**
   * @brief Create a buffer from existing memory
   * @param data data pointer
   * @param data_size data size
   * @param func data free function
   * @return create result
   */
  virtual Status Build(void* data, size_t data_size, DeleteFunction func);

  /**
   * @brief Create a buffer from existing host memory
   * @param data data pointer to host memory
   * @param data_size data size
   * @param func data free function
   * @return create result
   */
  virtual Status BuildFromHost(void* data, size_t data_size,
                               DeleteFunction func = nullptr);

  /**
   * @brief Get buffer mutable data pointer, if data is immutable, null pointer
   * will return
   * @return mutable buffer data pointer
   */
  virtual void* MutableData();

  /**
   * @brief Get buffer data pointer, buffer pointer is readonly
   * @return const buffer data pointer
   */
  virtual const void* ConstData() const;

  /**
   * @brief Is there an error
   * @return whether has an error
   */
  virtual bool HasError() const;

  /**
   * @brief Save error
   * @param error flowuint error
   * @return buffer reference
   */
  virtual Buffer& SetError(const std::shared_ptr<FlowUnitError>& error);

  /**
   * @brief Get flowunit error
   * @return flowunit error
   */
  virtual const std::shared_ptr<FlowUnitError>& GetError() const;

  /**
   * @brief Get buffer size in bytes
   * @return buffer size in tyes
   */
  virtual size_t GetBytes() const;

  /**
   * @brief Copy meta from other buffer.
   * @param buf other buffer.
   * @param is_override may override existing meta.
   * @return copy result
   */
  virtual Status CopyMeta(const std::shared_ptr<Buffer> buf,
                          bool is_override = false);

  /**
   * @brief Set meta key to buffer
   * @param key meta key
   * @param value meta value
   */
  template <typename T>
  void Set(const std::string& key, T&& value) {
    meta_->Set(key, value);
  }

  /**
   * @brief Get meta key from the buffer
   * @param key meta key
   * @param value meta value
   * @return whether the key exists
   */
  template <typename T>
  bool Get(const std::string& key, T&& value) {
    return meta_->Get(key, value);
  }

  /**
   * @brief Get value of key return tuple
   * @param key meta key
   * @return meta tuple
   */
  std::tuple<Any*, bool> Get(const std::string& key) { return meta_->Get(key); }

  /**
   * @brief Get meta key from the buffer, when the key does not exist, return to
   * the default value
   * @param key meta key
   * @param value meta value
   * @param default_value if key does not exist, return this value
   */
  template <typename T, typename U>
  void Get(const std::string& key, T&& value, const U& default_value) {
    auto ret = meta_->Get(key, value);
    if (!ret) {
      value = default_value;
    }
  }

  /**
   * @brief Get the device object related to the buffer
   * @return related device of the buffer
   */
  std::shared_ptr<Device> GetDevice() const;

  /**
   * @brief Copy buffer, only copy object, share same data memory
   * @return related device of the buffer
   */
  virtual std::shared_ptr<Buffer> Copy() const;

  /**
   * @brief Copy buffer, include meta and memory
   * @return related device of the buffer
   */
  virtual std::shared_ptr<Buffer> DeepCopy() const;

  /**
   * @brief Copy buffer, include meta and memory
   * @param dest_device copy data to other device
   * @return related device of the buffer
   */
  virtual std::shared_ptr<Buffer> CopyTo(
      const std::shared_ptr<Device>& dest_device) const;

  /**
   * @brief Get buffer type
   * @return buffer type
   */
  BufferEnumType GetBufferType() const;

  /**
   * @brief Set buffer type
   * @param type type to set
   */
  void SetGetBufferType(BufferEnumType type);

  /**
   * @brief Get device memory of buffer
   * @return device memory
   */
  std::shared_ptr<DeviceMemory> GetDeviceMemory() const;

 protected:
  /**
   * @brief Deep copy buffer
   * @return device memory
   */
  Status DeepCopy(const Buffer& other);

  /**
   * @brief Set data mutable
   * @return set result
   */
  Status SetBufferMutable(bool is_mutable);

 private:
  friend class BufferList;
  /// @brief Buffer meta
  std::shared_ptr<BufferMeta> meta_;

  /// @brief Buffer device memory
  std::shared_ptr<DeviceMemory> dev_mem_;

  uint32_t dev_mem_flags_{0};

  /// @brief Buffer type
  BufferEnumType type_{BufferEnumType::RAW};
};

}  // namespace modelbox

#endif  // MODELBOX_BUFFER_H_