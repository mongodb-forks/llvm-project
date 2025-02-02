//===- RPC.h - Interface for remote procedure calls from the GPU ----------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "RPC.h"

#include "Shared/Debug.h"

#include "PluginInterface.h"

// TODO: This should be included unconditionally and cleaned up.
#if defined(LIBOMPTARGET_RPC_SUPPORT)
#include "include/llvm-libc-types/rpc_opcodes_t.h"
#include "llvmlibc_rpc_server.h"
#include "shared/rpc.h"
#endif

using namespace llvm;
using namespace omp;
using namespace target;

RPCServerTy::RPCServerTy(plugin::GenericPluginTy &Plugin)
    : Buffers(Plugin.getNumDevices()) {}

llvm::Expected<bool>
RPCServerTy::isDeviceUsingRPC(plugin::GenericDeviceTy &Device,
                              plugin::GenericGlobalHandlerTy &Handler,
                              plugin::DeviceImageTy &Image) {
#ifdef LIBOMPTARGET_RPC_SUPPORT
  return Handler.isSymbolInImage(Device, Image, "__llvm_libc_rpc_client");
#else
  return false;
#endif
}

Error RPCServerTy::initDevice(plugin::GenericDeviceTy &Device,
                              plugin::GenericGlobalHandlerTy &Handler,
                              plugin::DeviceImageTy &Image) {
#ifdef LIBOMPTARGET_RPC_SUPPORT
  uint64_t NumPorts =
      std::min(Device.requestedRPCPortCount(), rpc::MAX_PORT_COUNT);
  void *RPCBuffer = Device.allocate(
      rpc::Server::allocation_size(Device.getWarpSize(), NumPorts), nullptr,
      TARGET_ALLOC_HOST);
  if (!RPCBuffer)
    return plugin::Plugin::error(
        "Failed to initialize RPC server for device %d", Device.getDeviceId());

  // Get the address of the RPC client from the device.
  void *ClientPtr;
  plugin::GlobalTy ClientGlobal("__llvm_libc_rpc_client", sizeof(void *));
  if (auto Err =
          Handler.getGlobalMetadataFromDevice(Device, Image, ClientGlobal))
    return Err;

  if (auto Err = Device.dataRetrieve(&ClientPtr, ClientGlobal.getPtr(),
                                     sizeof(void *), nullptr))
    return Err;

  rpc::Client client(NumPorts, RPCBuffer);
  if (auto Err =
          Device.dataSubmit(ClientPtr, &client, sizeof(rpc::Client), nullptr))
    return Err;
  Buffers[Device.getDeviceId()] = RPCBuffer;

  return Error::success();

#endif
  return Error::success();
}

Error RPCServerTy::runServer(plugin::GenericDeviceTy &Device) {
#ifdef LIBOMPTARGET_RPC_SUPPORT
  uint64_t NumPorts =
      std::min(Device.requestedRPCPortCount(), rpc::MAX_PORT_COUNT);
  rpc::Server Server(NumPorts, Buffers[Device.getDeviceId()]);

  auto port = Server.try_open(Device.getWarpSize());
  if (!port)
    return Error::success();

  int Status = rpc::SUCCESS;
  switch (port->get_opcode()) {
  case RPC_MALLOC: {
    port->recv_and_send([&](rpc::Buffer *Buffer, uint32_t) {
      Buffer->data[0] = reinterpret_cast<uintptr_t>(Device.allocate(
          Buffer->data[0], nullptr, TARGET_ALLOC_DEVICE_NON_BLOCKING));
    });
    break;
  }
  case RPC_FREE: {
    port->recv([&](rpc::Buffer *Buffer, uint32_t) {
      Device.free(reinterpret_cast<void *>(Buffer->data[0]),
                  TARGET_ALLOC_DEVICE_NON_BLOCKING);
    });
    break;
  }
  default:
    // Let the `libc` library handle any other unhandled opcodes.
    Status = libc_handle_rpc_port(&*port, Device.getWarpSize());
    break;
  }
  port->close();

  if (Status != rpc::SUCCESS)
    return createStringError("RPC server given invalid opcode!");

  return Error::success();
#endif
  return Error::success();
}

Error RPCServerTy::deinitDevice(plugin::GenericDeviceTy &Device) {
#ifdef LIBOMPTARGET_RPC_SUPPORT
  Device.free(Buffers[Device.getDeviceId()], TARGET_ALLOC_HOST);
  return Error::success();
#endif
  return Error::success();
}
