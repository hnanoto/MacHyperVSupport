//
//  HyperVShutdown.cpp
//  Hyper-V guest shutdown driver
//
//  Copyright © 2021-2022 Goldfish64. All rights reserved.
//

#include "HyperVShutdown.hpp"

OSDefineMetaClassAndStructors(HyperVShutdown, super);

static const VMBusICVersion shutdownVersions[] = {
  kHyperVShutdownVersionV3_2,
  kHyperVShutdownVersionV3_1,
  kHyperVShutdownVersionV3_0,
  kHyperVShutdownVersionV1_0
};

bool HyperVShutdown::start(IOService *provider) {
  if (HVCheckOffArg()) {
    HVSYSLOG("Disabling Hyper-V Guest Shutdown due to boot arg");
    return false;
  }

  if (!super::start(provider)) {
    HVSYSLOG("super::start() returned false");
    return false;
  }

  HVCheckDebugArgs();
  setICDebug(debugEnabled);

  HVDBGLOG("Initialized Hyper-V Guest Shutdown");
  return true;
}

void HyperVShutdown::stop(IOService *provider) {
  HVDBGLOG("Stopping Hyper-V Guest Shutdown");
  super::stop(provider);
}

void HyperVShutdown::handlePacket(VMBusPacketHeader *pktHeader, UInt32 pktHeaderLength, UInt8 *pktData, UInt32 pktDataLength) {
  VMBusICMessageShutdown *shutdownMsg = (VMBusICMessageShutdown*) pktData;

  bool doShutdown = false;
  switch (shutdownMsg->header.type) {
    case kVMBusICMessageTypeNegotiate:
      //
      // Determine supported protocol version and communicate back to Hyper-V.
      //
      if (!processNegotiationResponse(&shutdownMsg->negotiate, shutdownVersions, arrsize(shutdownVersions))) {
        HVSYSLOG("Failed to determine a supported Hyper-V Guest Shutdown version");
        shutdownMsg->header.status = kHyperVStatusFail;
      }
      break;

    case kVMBusICMessageTypeShutdown:
      //
      // Shutdown/restart request.
      //
      doShutdown = handleShutdown(&shutdownMsg->shutdown);
      break;

    default:
      HVDBGLOG("Unknown shutdown message type %u", shutdownMsg->header.type);
      shutdownMsg->header.status = kHyperVStatusFail;
      break;
  }

  //
  // Send response back to Hyper-V. The packet size will always be the same as the original inbound one.
  //
  shutdownMsg->header.flags = kVMBusICFlagTransaction | kVMBusICFlagResponse;
  _hvDevice->writeInbandPacket(shutdownMsg, pktDataLength, false);

  //
  // Shutdown machine if requested. This should not return.
  //
  if (doShutdown) {
    HVDBGLOG("Shutdown request received, notifying userspace");
    performShutdown(&shutdownMsg->shutdown, true);
  }
}

bool HyperVShutdown::handleShutdown(VMBusICMessageShutdownData *shutdownData) {
  bool result       = false;
  UInt32 packetSize = shutdownData->header.dataSize + sizeof (shutdownData->header);

  if (packetSize < __offsetof (VMBusICMessageShutdownData, reason)) {
    HVSYSLOG("Shutdown packet is invalid size (%u bytes)", packetSize);
    return false;
  }
  HVDBGLOG("Shutdown request received: flags 0x%X, reason 0x%X", shutdownData->flags, shutdownData->reason);

  //
  // Send message to userclients to see if we can shutdown.
  //
  result = _hvDevice->getHvController()->checkUserClient();
  if (result) {
    result = performShutdown(shutdownData, false);
    if (!result) {
      HVSYSLOG("Unable to request shutdown (invalid flags)");
    }
  } else {
    HVSYSLOG("Unable to request shutdown (shutdown daemon is not running)");
  }

  shutdownData->header.status = result ? kHyperVStatusSuccess : kHyperVStatusFail;
  return result;
}

bool HyperVShutdown::performShutdown(VMBusICMessageShutdownData *shutdownData, bool doShutdown) {
  switch (shutdownData->flags) {
    case kVMBusICShutdownFlagsShutdown:
    case kVMBusICShutdownFlagsShutdownForced:
      if (doShutdown) {
        HVDBGLOG("Performing shutdown");
        _hvDevice->getHvController()->notifyUserClient(kHyperVUserClientNotificationTypePerformShutdown, nullptr, 0);
      }
      break;

    case kVMBusICShutdownFlagsRestart:
    case kVMBusICShutdownFlagsRestartForced:
      if (doShutdown) {
        HVDBGLOG("Performing restart");
        _hvDevice->getHvController()->notifyUserClient(kHyperVUserClientNotificationTypePerformRestart, nullptr, 0);
      }
      break;

    default:
      HVSYSLOG("Invalid shutdown flags %u");
      return false;
  }

  return true;
}
