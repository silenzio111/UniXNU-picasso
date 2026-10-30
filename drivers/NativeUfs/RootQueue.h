// SPDX-License-Identifier: BSD-2-Clause-Patent
// Root queue takeover; shared with the host fault-injection harness.
#ifndef NATIVE_UFS_TRACE
#define NATIVE_UFS_TRACE(...) do { if (gRootDiagnosticEnabled) IOLog(__VA_ARGS__); } while (0)
#endif

static bool hostWaitRegister (unsigned offset, uint32_t mask, uint32_t value)
{
  uint64_t deadline;
  clock_interval_to_deadline (100, kMillisecondScale, &deadline);
  do {
    if ((read32 (gHost.hc, offset) & mask) == value) return true;
    IODelay (10);
  } while (mach_absolute_time () < deadline);
  return false;
}

static bool hostStopQueue (const char **why)
{
  // Confirm the firmware has no active transfer/task before changing its
  // request list or the storage stream's SMMU routing.
  uint32_t hce = read32 (gHost.hc, 0x34);
  uint32_t hcs = read32 (gHost.hc, 0x30);
  NATIVE_UFS_TRACE ("NATIVE_UFS_QUEUE stop hce=%x hcs=%x\n", hce, hcs);
  if (!(hce & 1) || (hcs & 0x0f) != 0x0f) {
    recordTransport (kIOReturnNotReady, 15);
    *why = "controller not ready for takeover";
    return false;
  }
  uint32_t doorbell = read32 (gHost.hc, 0x58);
  uint32_t task = read32 (gHost.hc, 0x78);
  NATIVE_UFS_TRACE ("NATIVE_UFS_QUEUE doorbell=%x task=%x\n", doorbell, task);
  if (doorbell || task) {
    recordTransport (kIOReturnBusy, 15);
    *why = "firmware request outstanding";
    return false;
  }
  uint32_t interrupt = read32 (gHost.hc, 0x20);
  NATIVE_UFS_TRACE ("NATIVE_UFS_QUEUE interrupt=%x\n", interrupt);
  if (interrupt & kUfsFatalStatus) {
    recordTransport (kIOReturnIOError, 15);
    *why = "firmware controller error";
    return false;
  }
  hostWrite (0x24, 0);
  hostWrite (0x60, 0);
  NATIVE_UFS_TRACE ("NATIVE_UFS_QUEUE stop_wait=begin\n");
  if (!hostWaitRegister (0x60, 1, 0)) {
    recordTransport (kIOReturnTimeout, 15);
    *why = "request list stop";
    return false;
  }
  NATIVE_UFS_TRACE ("NATIVE_UFS_QUEUE stop_wait=end\n");
  return true;
}

static bool hostStartQueue (const char **why)
{
  NATIVE_UFS_TRACE ("NATIVE_UFS_QUEUE start list=%llx\n", gHost.phys);
  hostWrite (0x50, static_cast<uint32_t> (gHost.phys));
  hostWrite (0x54, static_cast<uint32_t> (gHost.phys >> 32));
  if (read32 (gHost.hc, 0x50) != uint32_t (gHost.phys) ||
      read32 (gHost.hc, 0x54) != uint32_t (gHost.phys >> 32)) {
    recordTransport (kIOReturnIOError, 15);
    *why = "request list address";
    return false;
  }
  gRootDiagnostic.isBefore = read32 (gHost.hc, 0x20);
  NATIVE_UFS_TRACE ("NATIVE_UFS_QUEUE start interrupt=%x\n", gRootDiagnostic.isBefore);
  hostWrite (0x20, gRootDiagnostic.isBefore);
  hostWrite (0x60, 1);
  NATIVE_UFS_TRACE ("NATIVE_UFS_QUEUE start_wait=begin\n");
  if (!hostWaitRegister (0x60, 1, 1) || !hostWaitRegister (0x30, 3, 3)) {
    recordTransport (kIOReturnTimeout, 15);
    *why = "request list ready";
    return false;
  }
  NATIVE_UFS_TRACE ("NATIVE_UFS_QUEUE start_wait=end\n");
  return true;
}
