/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */

/*!
 * \file snps_ev_device_api.cc
 */

#include <tvm/runtime/registry.h>
#include <tvm/runtime/device_api.h>
#include <tvm/runtime/c_runtime_api.h>
#include "../workspace_pool.h"
#include "snps_ev_session.h"

namespace tvm {
namespace runtime {
/*!
 * \brief device API for uTVM snps_ev devices
 */
class SNPS_EVDeviceAPI final : public DeviceAPI {
 public:
  /*! \brief constructor */
  SNPS_EVDeviceAPI() { }

  void SetDevice(TVMContext ctx) final {}

  void GetAttr(TVMContext ctx, DeviceAttrKind kind, TVMRetValue* rv) final {
    if (kind == kExist) {
      *rv = 1;
    }
  }

  void* AllocDataSpace(TVMContext ctx,
                       size_t nbytes,
                       size_t alignment,
                       TVMType type_hint) final {
    ObjectPtr<SNPS_EVSession>& session = SNPS_EVSession::Current();
    void* data = session->AllocateInSection(SectionKind::kHeap, nbytes).cast_to<void*>();
    CHECK(data != nullptr) << "unable to allocate " << nbytes << " bytes on device heap";
    SNPS_EVDevSpace* dev_space = new SNPS_EVDevSpace();
    dev_space->data = data;
    dev_space->session = session;
    return static_cast<void*>(dev_space);
  }

  void FreeDataSpace(TVMContext ctx, void* ptr) final {
    SNPS_EVDevSpace* dev_space = static_cast<SNPS_EVDevSpace*>(ptr);
    dev_space->session->FreeInSection(
      SectionKind::kHeap, DevPtr(reinterpret_cast<std::uintptr_t>(dev_space->data)));
    delete dev_space;
  }

  void CopyDataFromTo(const void* from,
                      size_t from_offset,
                      void* to,
                      size_t to_offset,
                      size_t size,
                      TVMContext ctx_from,
                      TVMContext ctx_to,
                      TVMType type_hint,
                      TVMStreamHandle stream) final {
    std::tuple<int, int> type_from_to(ctx_from.device_type, ctx_to.device_type);
    if (type_from_to == std::make_tuple(kDLSNPS_EV, kDLSNPS_EV)) {
      // Copying from the device to the device.

      SNPS_EVDevSpace* from_space = static_cast<SNPS_EVDevSpace*>(const_cast<void*>(from));
      SNPS_EVDevSpace* to_space = static_cast<SNPS_EVDevSpace*>(const_cast<void*>(to));
      CHECK(from_space->session == to_space->session)
          << "attempt to copy data between different snps_ev sessions ("
          << from_space->session.get()
          << " != " << to_space->session.get() << ")";
      CHECK(ctx_from.device_id == ctx_to.device_id)
        << "can only copy between the same snps_ev device";
      ObjectPtr<SNPS_EVSession>& session = from_space->session;
      const std::shared_ptr<LowLevelDevice>& lld = session->low_level_device();

      DevPtr from_dev_addr = GetDevLoc(from_space, from_offset);
      DevPtr to_dev_addr = GetDevLoc(to_space, to_offset);

      std::vector<uint8_t> buffer(size);
      lld->Read(from_dev_addr, static_cast<void*>(buffer.data()), size);
      lld->Write(to_dev_addr, static_cast<void*>(buffer.data()), size);
    } else if (type_from_to == std::make_tuple(kDLSNPS_EV, kDLCPU)) {
      // Reading from the device.

      SNPS_EVDevSpace* from_space = static_cast<SNPS_EVDevSpace*>(const_cast<void*>(from));
      ObjectPtr<SNPS_EVSession>& session = from_space->session;
      const std::shared_ptr<LowLevelDevice>& lld = session->low_level_device();

      DevPtr from_dev_addr = GetDevLoc(from_space, from_offset);
      void* to_host_ptr = GetHostLoc(to, to_offset);
      lld->Read(from_dev_addr, to_host_ptr, size);
    } else if (type_from_to == std::make_tuple(kDLCPU, kDLSNPS_EV)) {
      // Writing to the device.

      SNPS_EVDevSpace* to_space = static_cast<SNPS_EVDevSpace*>(const_cast<void*>(to));
      ObjectPtr<SNPS_EVSession>& session = to_space->session;
      const std::shared_ptr<LowLevelDevice>& lld = session->low_level_device();

      void* from_host_ptr = GetHostLoc(from, from_offset);
      DevPtr to_dev_addr = GetDevLoc(to_space, to_offset);
      lld->Write(to_dev_addr, from_host_ptr, size);
    } else {
      LOG(FATAL) << "Expect copy from/to snps_ev device or between snps_ev device\n";
    }
  }

  void StreamSync(TVMContext ctx, TVMStreamHandle stream) final {
  }

  void* AllocWorkspace(TVMContext ctx, size_t size, TVMType type_hint) final {
    ObjectPtr<SNPS_EVSession>& session = SNPS_EVSession::Current();

    void* data = session->AllocateInSection(SectionKind::kWorkspace, size).cast_to<void*>();
    CHECK(data != nullptr) << "unable to allocate " << size << " bytes on device workspace";
    SNPS_EVDevSpace* dev_space = new SNPS_EVDevSpace();
    dev_space->data = data;
    dev_space->session = session;
    return static_cast<void*>(dev_space);
  }

  void FreeWorkspace(TVMContext ctx, void* data) final {
    SNPS_EVDevSpace* dev_space = static_cast<SNPS_EVDevSpace*>(data);
    ObjectPtr<SNPS_EVSession>& session = dev_space->session;
    session->FreeInSection(SectionKind::kWorkspace,
                           DevPtr(reinterpret_cast<std::uintptr_t>(dev_space->data)));
    delete dev_space;
  }

  /*!
   * \brief obtain a global singleton of SNPS_EVDeviceAPI
   * \return global shared pointer to SNPS_EVDeviceAPI
   */
  static const std::shared_ptr<SNPS_EVDeviceAPI>& Global() {
    static std::shared_ptr<SNPS_EVDeviceAPI> inst = std::make_shared<SNPS_EVDeviceAPI>();
    return inst;
  }

 private:
  DevPtr GetDevLoc(SNPS_EVDevSpace* dev_space, size_t offset) {
    return DevPtr(reinterpret_cast<std::uintptr_t>(dev_space->data) + offset);
  }

  void* GetHostLoc(const void* ptr, size_t offset) {
    return reinterpret_cast<void*>(reinterpret_cast<std::uintptr_t>(ptr) + offset);
  }
};

// register device that can be obtained from Python frontend
TVM_REGISTER_GLOBAL("device_api.snps_ev_dev")
.set_body([](TVMArgs args, TVMRetValue* rv) {
    DeviceAPI* ptr = SNPS_EVDeviceAPI::Global().get();
    *rv = static_cast<void*>(ptr);
    });
}  // namespace runtime
}  // namespace tvm
