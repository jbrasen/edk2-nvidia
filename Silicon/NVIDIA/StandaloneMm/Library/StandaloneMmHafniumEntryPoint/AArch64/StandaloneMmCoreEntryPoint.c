/** @file
  Entry point to the Standalone MM Foundation when initialized during the SEC
  phase on ARM platforms

Copyright (c) 2021-2022, NVIDIA CORPORATION. All rights reserved.
Copyright (c) 2017 - 2021, Arm Ltd. All rights reserved.<BR>
SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#include <PiMm.h>

#include <Library/Arm/StandaloneMmCoreEntryPoint.h>

#include <PiPei.h>
#include <Guid/MmramMemoryReserve.h>
#include <Guid/MpInformation.h>

#include <Library/ArmMmuLib.h>
#include <Library/ArmSvcLib.h>
#include <Library/DebugLib.h>
#include <Library/HobLib.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/SerialPortLib.h>
#include <Library/PcdLib.h>
#include <Library/StandaloneMmOpteeDeviceMem.h>

#include <IndustryStandard/ArmStdSmc.h>
#include <IndustryStandard/ArmMmSvc.h>
#include <IndustryStandard/ArmFfaSvc.h>

#include <Include/libfdt.h>

#include "../SlabMmuOps/SlabMmuOps.h"
#include "StandaloneMmHafnium.h"

#define SPM_MAJOR_VER_MASK    0xFFFF0000
#define SPM_MINOR_VER_MASK    0x0000FFFF
#define SPM_MAJOR_VER_SHIFT   16
#define FFA_NOT_SUPPORTED     -1
#define FFA_MSG_WAIT_32       0x8400006B
#define FFA_ERROR_32          0x84000060
#define FFA_VMID_SHIFT        16
#define FFA_VMID_MASK         0xFFFF
#define DEFAULT_PAGE_SIZE     SIZE_4KB
#define MAX_MANIFEST_REGIONS  255
#define SP_PKG_HEADER_SIZE    0x18

#define ADDRESS_IN_RANGE(addr, min, max)  (((addr) > (min)) && ((addr) < (max)))

ARM_MEMORY_REGION_DESCRIPTOR        MemoryTable[MAX_MANIFEST_REGIONS+1];
PI_MM_ARM_TF_CPU_DRIVER_ENTRYPOINT  CpuDriverEntryPoint = NULL;
EFI_SECURE_PARTITION_BOOT_INFO      PayloadBootInfo;
STATIC STMM_COMM_BUFFERS            StmmCommBuffers;

STATIC CONST UINT32  mSpmMajorVer = SPM_MAJOR_VERSION;
STATIC CONST UINT32  mSpmMinorVer = SPM_MINOR_VERSION;

STATIC CONST UINT32  mSpmMajorVerFfa = SPM_MAJOR_VERSION_FFA;
STATIC CONST UINT32  mSpmMinorVerFfa = SPM_MINOR_VERSION_FFA;

/*
 * Helper function get a 32-bit property from the Manifest and accessing it in a way
 * that won't cause alignment issues if running with MMU disabled.
 */
STATIC
UINT64
FDTGetProperty32 (
  VOID        *DtbAddress,
  INT32       NodeOffset,
  CONST VOID  *PropertyName
  )
{
  CONST VOID  *Property;
  INT32       Length;
  UINT32      P32;

  Property = fdt_getprop (DtbAddress, NodeOffset, PropertyName, &Length);

  ASSERT (Property != NULL);
  ASSERT (Length == 4);

  CopyMem ((VOID *)&P32, (UINT32 *)Property, sizeof (UINT32));

  return SwapBytes32 (P32);
}

/*
 * Helper function get a 64-bit property from the Manifest and accessing it in a way
 * that won't cause alignment issues if running with MMU disabled.
 */
STATIC
UINT64
FDTGetProperty64 (
  VOID        *DtbAddress,
  INT32       NodeOffset,
  CONST VOID  *PropertyName
  )
{
  CONST VOID  *Property;
  INT32       Length;
  UINT64      P64;

  Property = fdt_getprop (DtbAddress, NodeOffset, PropertyName, &Length);

  ASSERT (Property != NULL);
  ASSERT (Length == 8);

  CopyMem ((VOID *)&P64, (UINT64 *)Property, sizeof (UINT64));

  return SwapBytes64 (P64);
}

/*
 * Quick sanity check of the partition manifest.
 *
 * @param  [in] DtbAddress           Address of the partition manifest.
 */
EFI_STATUS
CheckManifest (
  IN VOID  *DtbAddress
  )
{
  INT32  HeaderCheck  = -1;
  INT32  ParentOffset = 0;

  /* Check integrity of DTB */
  HeaderCheck = fdt_check_header ((VOID *)DtbAddress);
  if (HeaderCheck != 0) {
    DEBUG ((DEBUG_ERROR, "fdt_check_header failed, err=%d\r\n", HeaderCheck));
    return EFI_DEVICE_ERROR;
  }

  ParentOffset = fdt_path_offset (DtbAddress, "/");
  if (ParentOffset < 0) {
    DEBUG ((DEBUG_ERROR, "Failed to find root node\r\n"));
    return EFI_DEVICE_ERROR;
  }

  ParentOffset = fdt_path_offset (DtbAddress, "/memory-regions");
  if (ParentOffset < 0) {
    DEBUG ((DEBUG_ERROR, "Failed to find /memory-regions node\r\n"));
    return EFI_DEVICE_ERROR;
  }

  return EFI_SUCCESS;
}

/*
 * From the manifest load-address and entrypoint-offset, find the base address of the SP code.
 *
 * @param  [in] DtbAddress           Address of the partition manifest.
 */
UINT64
GetSpImageBase (
  IN VOID  *DtbAddress
  )
{
  INT32   ParentOffset = 0;
  UINT64  SpImageBase  = 0;

  ParentOffset = fdt_path_offset (DtbAddress, "/");

  SpImageBase = FDTGetProperty64 (DtbAddress, ParentOffset, "load-address") +
                FDTGetProperty32 (DtbAddress, ParentOffset, "entrypoint-offset");

  return SpImageBase;
}

/*
 * Get the device regions from the manifest and install a guided hob that
 * the other drivers can use.
 *
 * @param  [in] DtbAddress           Address of the partition manifest.
 * EFI_SUCCESS                       On Success
 * EFI_NOT_FOUND                     Device Regions not found.
 * OTHER                             On failure to install GuidHob
 */
STATIC
EFI_STATUS
GetDeviceMemRegions (
  IN VOID  *DtbAddress
  )
{
  INT32                 ParentOffset   = 0;
  INT32                 NodeOffset     = 0;
  INT32                 PrevNodeOffset = 0;
  CONST VOID            *NodeName;
  EFI_MM_DEVICE_REGION  *DeviceRegions;
  UINTN                 NumRegions;
  UINTN                 BufferSize;
  EFI_STATUS            Status;
  UINTN                 Index;

  NumRegions   = 0;
  Index        = 0;
  Status       = EFI_SUCCESS;
  ParentOffset = fdt_path_offset (DtbAddress, "/device-regions");
  if (ParentOffset < 0) {
    DEBUG ((DEBUG_ERROR, "Failed to find /device-regions node\r\n"));
    Status = EFI_NOT_FOUND;
    goto GetDeviceMemRegionsExit;
  }

  for (NodeOffset = fdt_first_subnode (DtbAddress, ParentOffset);
       NodeOffset > 0;
       NodeOffset = fdt_next_subnode (DtbAddress, PrevNodeOffset))
  {
    NumRegions++;
    PrevNodeOffset = NodeOffset;
  }

  if (NumRegions == 0) {
    goto GetDeviceMemRegionsExit;
  }

  BufferSize    = NumRegions * sizeof (EFI_MM_DEVICE_REGION);
  DeviceRegions = BuildGuidHob (&gEfiStandaloneMmDeviceMemoryRegions, BufferSize);

  for (NodeOffset = fdt_first_subnode (DtbAddress, ParentOffset);
       NodeOffset > 0;
       NodeOffset = fdt_next_subnode (DtbAddress, PrevNodeOffset))
  {
    NodeName                               = fdt_get_name (DtbAddress, NodeOffset, NULL);
    DeviceRegions[Index].DeviceRegionStart =
      FDTGetProperty64 (DtbAddress, NodeOffset, "base-address");
    DeviceRegions[Index].DeviceRegionSize = FDTGetProperty32 (
                                              DtbAddress,
                                              NodeOffset,
                                              "pages-count"
                                              )
                                            * DEFAULT_PAGE_SIZE;

    AsciiStrnCpyS (
      DeviceRegions[Index].DeviceRegionName,
      DEVICE_REGION_NAME_MAX_LEN,
      NodeName,
      AsciiStrLen (NodeName)
      );
    DEBUG ((
      DEBUG_INFO,
      "%a: Name %a Start 0x%lx Size %u\n",
      __FUNCTION__,
      DeviceRegions[Index].DeviceRegionName,
      DeviceRegions[Index].DeviceRegionStart,
      DeviceRegions[Index].DeviceRegionSize
      ));
    Index++;
    PrevNodeOffset = NodeOffset;
  }

GetDeviceMemRegionsExit:
  return Status;
}

/*
 * Gather additional information from the Manifest to populate the PayloadBootInfo structure.
 * The PayloadBootInfo.SpImageBase and PayloadBootInfo.SpImageSize fields must already be initialized.
 *
 * @param  [in] DtbAddress           Address of the partition manifest.
 * @param  [in] TotalSPMemorySize    Total memory allocated to the SP.
 */
EFI_STATUS
GetAndPrintManifestinformation (
  IN VOID    *DtbAddress,
  IN UINT64  TotalSPMemorySize
  )
{
  INT32       ParentOffset   = 0;
  INT32       NodeOffset     = 0;
  INT32       PrevNodeOffset = 0;
  CONST VOID  *NodeName;
  UINT64      FfaRxBufferAddr, FfaTxBufferAddr;
  UINT32      FfaRxBufferSize, FfaTxBufferSize;
  UINT32      ReservedPagesSize;
  UINT64      LoadAddress;
  UINT64      SPMemoryLimit;
  UINT64      LowestRegion, HighestRegion;
  UINT64      RegionAddress;
  UINT32      RegionSize;

  ParentOffset = fdt_path_offset (DtbAddress, "/");

  LoadAddress                = FDTGetProperty64 (DtbAddress, ParentOffset, "load-address");
  PayloadBootInfo.SpMemBase  = LoadAddress;
  PayloadBootInfo.SpMemLimit = PayloadBootInfo.SpImageBase + PayloadBootInfo.SpImageSize;
  SPMemoryLimit              = PayloadBootInfo.SpMemBase + TotalSPMemorySize;
  ReservedPagesSize          = FDTGetProperty32 (DtbAddress, ParentOffset, "reserved-pages-count") * DEFAULT_PAGE_SIZE;
  LowestRegion               = SPMemoryLimit;
  HighestRegion              = PayloadBootInfo.SpMemBase;

  ParentOffset = fdt_path_offset (DtbAddress, "/memory-regions");
  if (ParentOffset < 0) {
    DEBUG ((DEBUG_ERROR, "Failed to find /memory-regions node\r\n"));
    return EFI_DEVICE_ERROR;
  }

  for (NodeOffset = fdt_first_subnode (DtbAddress, ParentOffset);
       NodeOffset > 0;
       NodeOffset = fdt_next_subnode (DtbAddress, PrevNodeOffset))
  {
    NodeName      = fdt_get_name (DtbAddress, NodeOffset, NULL);
    RegionAddress = FDTGetProperty64 (DtbAddress, NodeOffset, "base-address");
    RegionSize    = FDTGetProperty32 (DtbAddress, NodeOffset, "pages-count") * DEFAULT_PAGE_SIZE;
    if (ADDRESS_IN_RANGE (RegionAddress, LoadAddress, SPMemoryLimit)) {
      LowestRegion  = MIN (LowestRegion, RegionAddress);
      HighestRegion = MAX (HighestRegion, (RegionAddress+RegionSize));
    }

    /* For each known resource type, extract information */
    if (AsciiStrCmp (NodeName, "stmmns-memory") == 0) {
      PayloadBootInfo.SpNsCommBufBase = RegionAddress;
      PayloadBootInfo.SpNsCommBufSize = RegionSize;
      StmmCommBuffers.NsBufferAddr    = PayloadBootInfo.SpNsCommBufBase;
      StmmCommBuffers.NsBufferSize    = PayloadBootInfo.SpNsCommBufSize;
    } else if (AsciiStrCmp (NodeName, "rx-buffer") == 0) {
      FfaRxBufferAddr = RegionAddress;
      FfaRxBufferSize = RegionSize;
    } else if (AsciiStrCmp (NodeName, "tx-buffer") == 0) {
      FfaTxBufferAddr = RegionAddress;
      FfaTxBufferSize = RegionSize;
    } else if (AsciiStrCmp (NodeName, "stmmsec-memory") == 0) {
      StmmCommBuffers.SecBufferAddr = RegionAddress;
      StmmCommBuffers.SecBufferSize = RegionSize;
    }

    PrevNodeOffset = NodeOffset;
  }

  /* Find the free memory in the SP space to use as driver heap */
 #ifdef HEAP_HIGH_REGION
  PayloadBootInfo.SpHeapBase = HighestRegion;
  PayloadBootInfo.SpHeapSize = SPMemoryLimit - PayloadBootInfo.SpHeapBase;
 #else
  PayloadBootInfo.SpHeapBase = PayloadBootInfo.SpMemLimit + ReservedPagesSize;
  PayloadBootInfo.SpHeapSize = LowestRegion - PayloadBootInfo.SpHeapBase;
 #endif
  DEBUG ((DEBUG_ERROR, "SPMEMBASE 0x%x RESERVED 0x%x SIZE 0x%x\n", PayloadBootInfo.SpHeapBase, ReservedPagesSize, PayloadBootInfo.SpHeapSize));

  /* Some StMM regions are not needed or don't apply to an UP migratable partition */
  PayloadBootInfo.SpSharedBufBase = 0;
  PayloadBootInfo.SpSharedBufSize = 0;
  PayloadBootInfo.SpStackBase     = 0;
  PayloadBootInfo.SpPcpuStackSize = 0;
  PayloadBootInfo.NumCpus         = 0;

  PayloadBootInfo.NumSpMemRegions = 6;

  DEBUG ((DEBUG_ERROR, "SP mem base       = 0x%llx \n", PayloadBootInfo.SpMemBase));
  DEBUG ((DEBUG_ERROR, "  SP image base   = 0x%llx \n", PayloadBootInfo.SpImageBase));
  DEBUG ((DEBUG_ERROR, "  SP image size   = 0x%llx \n", PayloadBootInfo.SpImageSize));
  DEBUG ((DEBUG_ERROR, "SP mem limit      = 0x%llx \n", PayloadBootInfo.SpMemLimit));
  DEBUG ((DEBUG_ERROR, "Core-Heap limit   = 0x%llx \n", PayloadBootInfo.SpMemLimit + ReservedPagesSize));
  DEBUG ((DEBUG_ERROR, "FFA rx buf base   = 0x%llx \n", FfaRxBufferAddr));
  DEBUG ((DEBUG_ERROR, "FFA rx buf size   = 0x%llx \n", FfaRxBufferSize));
  DEBUG ((DEBUG_ERROR, "FFA tx buf base   = 0x%llx \n", FfaTxBufferAddr));
  DEBUG ((DEBUG_ERROR, "FFA tx buf size   = 0x%llx \n", FfaTxBufferSize));
  DEBUG ((DEBUG_ERROR, "Driver-Heap base  = 0x%llx \n", PayloadBootInfo.SpHeapBase));
  DEBUG ((DEBUG_ERROR, "Driver-Heap size  = 0x%llx \n", PayloadBootInfo.SpHeapSize));
  DEBUG ((DEBUG_ERROR, "SP real mem limit = 0x%llx \n", SPMemoryLimit));

  DEBUG ((DEBUG_ERROR, "Shared Buffers:\n"));
  DEBUG ((DEBUG_ERROR, "SP NS buf base    = 0x%llx \n", StmmCommBuffers.NsBufferAddr));
  DEBUG ((DEBUG_ERROR, "SP NS buf size    = 0x%llx \n", StmmCommBuffers.NsBufferSize));
  DEBUG ((DEBUG_ERROR, "SP Sec buf base   = 0x%llx \n", StmmCommBuffers.SecBufferAddr));
  DEBUG ((DEBUG_ERROR, "SP Sec buf size   = 0x%llx \n", StmmCommBuffers.SecBufferSize));

  /* Core will take all the memory from SpMemBase to CoreHeapLimit and should not reach the first memory-region */
  ASSERT ((PayloadBootInfo.SpMemLimit + ReservedPagesSize) <= FfaRxBufferAddr);

  return EFI_SUCCESS;
}

/**
 * A loop to delegated events.
 *
 * @param  [in] EventCompleteSvcArgs   Pointer to the event completion arguments.
 *
 */
VOID
EFIAPI
DelegatedEventLoop (
  IN ARM_SVC_ARGS  *EventCompleteSvcArgs
  )
{
  EFI_STATUS  Status;
  UINTN       SvcStatus;
  BOOLEAN     FfaEnabled;
  UINT16      SenderPartId;
  UINT16      ReceiverPartId;

  while (TRUE) {
    ArmCallSvc (EventCompleteSvcArgs);

    DEBUG ((DEBUG_INFO, "Received delegated event\n"));
    DEBUG ((DEBUG_INFO, "X0 :  0x%x\n", (UINT32)EventCompleteSvcArgs->Arg0));
    DEBUG ((DEBUG_INFO, "X1 :  0x%x\n", (UINT32)EventCompleteSvcArgs->Arg1));
    DEBUG ((DEBUG_INFO, "X2 :  0x%x\n", (UINT32)EventCompleteSvcArgs->Arg2));
    DEBUG ((DEBUG_INFO, "X3 :  0x%x\n", (UINT32)EventCompleteSvcArgs->Arg3));
    DEBUG ((DEBUG_INFO, "X4 :  0x%x\n", (UINT32)EventCompleteSvcArgs->Arg4));
    DEBUG ((DEBUG_INFO, "X5 :  0x%x\n", (UINT32)EventCompleteSvcArgs->Arg5));
    DEBUG ((DEBUG_INFO, "X6 :  0x%x\n", (UINT32)EventCompleteSvcArgs->Arg6));
    DEBUG ((DEBUG_INFO, "X7 :  0x%x\n", (UINT32)EventCompleteSvcArgs->Arg7));

    SenderPartId   = EventCompleteSvcArgs->Arg1 >> FFA_VMID_SHIFT;
    ReceiverPartId = EventCompleteSvcArgs->Arg1 & FFA_VMID_MASK;

    FfaEnabled = FeaturePcdGet (PcdFfaEnable);
    if (FfaEnabled) {
      switch (EventCompleteSvcArgs->Arg3) {
        case STMM_GET_NS_BUFFER:
          EventCompleteSvcArgs->Arg5 = StmmCommBuffers.NsBufferAddr;
          EventCompleteSvcArgs->Arg6 = StmmCommBuffers.NsBufferSize;
          Status                     = EFI_SUCCESS;
          break;
        case ARM_SMC_ID_MM_COMMUNICATE_AARCH64:
          Status = CpuDriverEntryPoint (
                     EventCompleteSvcArgs->Arg0,
                     EventCompleteSvcArgs->Arg6,
                     EventCompleteSvcArgs->Arg5
                     );
          if (EFI_ERROR (Status)) {
            DEBUG ((
              DEBUG_ERROR,
              "Failed delegated event 0x%x, Status 0x%x\n",
              EventCompleteSvcArgs->Arg3,
              Status
              ));
          }

          break;
        default:
          Status = EFI_UNSUPPORTED;
          break;
      }
    } else {
      Status = CpuDriverEntryPoint (
                 EventCompleteSvcArgs->Arg0,
                 EventCompleteSvcArgs->Arg3,
                 EventCompleteSvcArgs->Arg1
                 );
      if (EFI_ERROR (Status)) {
        DEBUG ((
          DEBUG_ERROR,
          "Failed delegated event 0x%x, Status 0x%x\n",
          EventCompleteSvcArgs->Arg0,
          Status
          ));
      }
    }

    switch (Status) {
      case EFI_SUCCESS:
        SvcStatus = ARM_SVC_SPM_RET_SUCCESS;
        break;
      case EFI_INVALID_PARAMETER:
        SvcStatus = ARM_SVC_SPM_RET_INVALID_PARAMS;
        break;
      case EFI_ACCESS_DENIED:
        SvcStatus = ARM_SVC_SPM_RET_DENIED;
        break;
      case EFI_OUT_OF_RESOURCES:
        SvcStatus = ARM_SVC_SPM_RET_NO_MEMORY;
        break;
      case EFI_UNSUPPORTED:
        SvcStatus = ARM_SVC_SPM_RET_NOT_SUPPORTED;
        break;
      default:
        SvcStatus = ARM_SVC_SPM_RET_NOT_SUPPORTED;
        break;
    }

    if (FfaEnabled) {
      EventCompleteSvcArgs->Arg0 = ARM_SVC_ID_FFA_MSG_SEND_DIRECT_RESP;
      EventCompleteSvcArgs->Arg1 = ReceiverPartId << FFA_VMID_SHIFT | SenderPartId;
      EventCompleteSvcArgs->Arg2 = 0;
      EventCompleteSvcArgs->Arg3 = ARM_SVC_ID_SP_EVENT_COMPLETE;
      EventCompleteSvcArgs->Arg4 = SvcStatus;
    } else {
      EventCompleteSvcArgs->Arg0 = ARM_SVC_ID_SP_EVENT_COMPLETE;
      EventCompleteSvcArgs->Arg1 = SvcStatus;
    }
  }
}

/**
  Query the SPM version, check compatibility and return success if compatible.

  @retval EFI_SUCCESS       SPM versions compatible.
  @retval EFI_UNSUPPORTED   SPM versions not compatible.
**/
STATIC
EFI_STATUS
GetSpmVersion (
  VOID
  )
{
  EFI_STATUS    Status;
  UINT16        CalleeSpmMajorVer;
  UINT16        CallerSpmMajorVer;
  UINT16        CalleeSpmMinorVer;
  UINT16        CallerSpmMinorVer;
  UINT32        SpmVersion;
  ARM_SVC_ARGS  SpmVersionArgs;

  if (FeaturePcdGet (PcdFfaEnable)) {
    SpmVersionArgs.Arg0  = ARM_SVC_ID_FFA_VERSION_AARCH32;
    SpmVersionArgs.Arg1  = mSpmMajorVerFfa << SPM_MAJOR_VER_SHIFT;
    SpmVersionArgs.Arg1 |= mSpmMinorVerFfa;
    CallerSpmMajorVer    = mSpmMajorVerFfa;
    CallerSpmMinorVer    = mSpmMinorVerFfa;
  } else {
    SpmVersionArgs.Arg0 = ARM_SVC_ID_SPM_VERSION_AARCH32;
    CallerSpmMajorVer   = mSpmMajorVer;
    CallerSpmMinorVer   = mSpmMinorVer;
  }

  ArmCallSvc (&SpmVersionArgs);

  SpmVersion = SpmVersionArgs.Arg0;
  if (SpmVersion == FFA_NOT_SUPPORTED) {
    return EFI_UNSUPPORTED;
  }

  CalleeSpmMajorVer = ((SpmVersion & SPM_MAJOR_VER_MASK) >> SPM_MAJOR_VER_SHIFT);
  CalleeSpmMinorVer = ((SpmVersion & SPM_MINOR_VER_MASK) >> 0);

  // Different major revision values indicate possibly incompatible functions.
  // For two revisions, A and B, for which the major revision values are
  // identical, if the minor revision value of revision B is greater than
  // the minor revision value of revision A, then every function in
  // revision A must work in a compatible way with revision B.
  // However, it is possible for revision B to have a higher
  // function count than revision A.
  if ((CalleeSpmMajorVer == CallerSpmMajorVer) &&
      (CalleeSpmMinorVer >= CallerSpmMinorVer))
  {
    DEBUG ((
      DEBUG_INFO,
      "SPM Version: Major=0x%x, Minor=0x%x\n",
      CalleeSpmMajorVer,
      CalleeSpmMinorVer
      ));
    Status = EFI_SUCCESS;
  } else {
    DEBUG ((
      DEBUG_INFO,
      "Incompatible SPM Versions.\n Callee Version: Major=0x%x, Minor=0x%x.\n Caller: Major=0x%x, Minor>=0x%x.\n",
      CalleeSpmMajorVer,
      CalleeSpmMinorVer,
      CallerSpmMajorVer,
      CallerSpmMinorVer
      ));
    Status = EFI_UNSUPPORTED;
  }

  return Status;
}

/**
 * Initialize parameters to be sent via SVC call.
 *
 * @param[out]     InitMmFoundationSvcArgs  Args structure
 * @param[out]     Ret                      Return Code
 *
 */
STATIC
VOID
InitArmSvcArgs (
  OUT ARM_SVC_ARGS  *InitMmFoundationSvcArgs,
  OUT INT32         *Ret
  )
{
  DEBUG ((DEBUG_ERROR, "%a: Ret %u\n", __FUNCTION__, *Ret));
  if (*Ret == 0) {
    InitMmFoundationSvcArgs->Arg0 = FFA_MSG_WAIT_32;
  } else {
    InitMmFoundationSvcArgs->Arg0 = FFA_ERROR_32;
  }

  InitMmFoundationSvcArgs->Arg1 = 0;
  InitMmFoundationSvcArgs->Arg2 = *Ret;
  InitMmFoundationSvcArgs->Arg3 = 0;
  InitMmFoundationSvcArgs->Arg4 = 0;
}

/**
 * Generate a table that contains all the memory regions that need to be mapped as stage-1 translations.
 * For DRAM, simply use the base of the SP (calculated as DTBAddress - sizeof(sp_pkg_header)) and use the total
 * SP Memory size as given by Hafnium.
 * For Devices, parse the Manifest looking for entries in the /device-regions node.
 *
 * Considering that parsing the Manifest in this function is done with caches disabled, it can be quite time consuming,
 * so in special development platforms, a "fast" mode can be used where all of the MMIO space is mapped (limited to
 * socket 0) instead of relying on the Manifest. In this case, access control to MMIO will still be ensured by stage-2
 * translations.
 *
 * @param[in]          TotalSPMemorySize  Total SP Memory size as given by Hafnium
 * @param[in]          DTBAddress         Address of the DTB as found by the entry point loader
 **/
STATIC
VOID
ConfigureStage1Translations (
  IN UINT64  TotalSPMemorySize,
  IN VOID    *DTBAddress
  )
{
  UINT64      Stage1EntriesAddress, Stage1EntriesPages;
  UINT64      RegionAddress, RegionSize;
  UINT64      NsBufferAddress, NsBufferSize;
  EFI_STATUS  Status;
  UINT16      NumRegions     = 0;
  INT32       ParentOffset   = 0;
  INT32       NodeOffset     = 0;
  INT32       PrevNodeOffset = 0;

 #ifdef FAST_STAGE1_SETUP
  /*In "Fast" mode, simply allocate the MMIO range of socket 0. That's sufficient for FPGA-based testing. */
  MemoryTable[NumRegions].PhysicalBase = 0;
  MemoryTable[NumRegions].VirtualBase  = 0;
  MemoryTable[NumRegions].Length       = 0x80000000;
  MemoryTable[NumRegions].Attributes   = ARM_MEMORY_REGION_ATTRIBUTE_DEVICE;
  NumRegions++;

 #else
  /* Loop over all the device-regions of the manifest. This is time-consuming with caches disabled. */
  ParentOffset = fdt_path_offset (DTBAddress, "/device-regions");
  if (ParentOffset < 0) {
    DEBUG ((DEBUG_ERROR, "Failed to find /device-regions node\r\n"));
    ASSERT (0);
  }

  for (NodeOffset = fdt_first_subnode (DTBAddress, ParentOffset); NodeOffset > 0;
       NodeOffset = fdt_next_subnode (DTBAddress, PrevNodeOffset))
  {
    RegionAddress = PAGE_ALIGN (FDTGetProperty64 (DTBAddress, NodeOffset, "base-address"), DEFAULT_PAGE_SIZE);
    RegionSize    = FDTGetProperty32 (DTBAddress, NodeOffset, "pages-count") * DEFAULT_PAGE_SIZE;

    MemoryTable[NumRegions].PhysicalBase = RegionAddress;
    MemoryTable[NumRegions].VirtualBase  = RegionAddress;
    MemoryTable[NumRegions].Length       = RegionSize;
    MemoryTable[NumRegions].Attributes   = ARM_MEMORY_REGION_ATTRIBUTE_DEVICE;
    NumRegions++;
    ASSERT (NumRegions < MAX_MANIFEST_REGIONS);

    PrevNodeOffset = NodeOffset;
  }

 #endif

  /* Single section for the whole SP memory */
  MemoryTable[NumRegions].PhysicalBase = PAGE_ALIGN ((UINT64)DTBAddress, DEFAULT_PAGE_SIZE);
  MemoryTable[NumRegions].VirtualBase  = MemoryTable[NumRegions].PhysicalBase;
  MemoryTable[NumRegions].Length       = TotalSPMemorySize;
  MemoryTable[NumRegions].Attributes   = ARM_MEMORY_REGION_ATTRIBUTE_WRITE_BACK;
  NumRegions++;

  /* Loop over all the memory-regions of the manifest. This is time-consuming with caches disabled. */
  ParentOffset = fdt_path_offset (DTBAddress, "/memory-regions");
  if (ParentOffset < 0) {
    DEBUG ((DEBUG_ERROR, "Failed to find /memory-regions node\r\n"));
    ASSERT (0);
  }

  NsBufferAddress = 0;
  NsBufferSize    = 0;
  for (NodeOffset = fdt_first_subnode (DTBAddress, ParentOffset); NodeOffset > 0;
       NodeOffset = fdt_next_subnode (DTBAddress, PrevNodeOffset))
  {
    INT32       Length;
    CONST VOID  *NodeName;

    NodeName = fdt_get_name (DTBAddress, NodeOffset, NULL);
    if (NodeName == NULL) {
      PrevNodeOffset = NodeOffset;
      continue;
    }

    if (fdt_getprop (DTBAddress, NodeOffset, "nv-non-secure-memory", &Length) != NULL) {
      NsBufferAddress = PAGE_ALIGN (FDTGetProperty64 (DTBAddress, NodeOffset, "base-address"), DEFAULT_PAGE_SIZE);
      NsBufferSize    = FDTGetProperty32 (DTBAddress, NodeOffset, "pages-count") * DEFAULT_PAGE_SIZE;
      /* NS Buffer */
      MemoryTable[NumRegions].PhysicalBase = NsBufferAddress;
      MemoryTable[NumRegions].VirtualBase  = NsBufferAddress;
      MemoryTable[NumRegions].Length       = NsBufferSize;
      MemoryTable[NumRegions].Attributes   = ARM_MEMORY_REGION_ATTRIBUTE_NONSECURE_UNCACHED_UNBUFFERED;
      NumRegions++;
    }

    if (fdt_getprop (DTBAddress, NodeOffset, "nv-sp-shared-buffer-id", &Length) != NULL) {
      RegionAddress = PAGE_ALIGN (FDTGetProperty64 (DTBAddress, NodeOffset, "base-address"), DEFAULT_PAGE_SIZE);
      RegionSize    = FDTGetProperty32 (DTBAddress, NodeOffset, "pages-count") * DEFAULT_PAGE_SIZE;
      /* Secure Buffer */
      MemoryTable[NumRegions].PhysicalBase = RegionAddress;
      MemoryTable[NumRegions].VirtualBase  = RegionAddress;
      MemoryTable[NumRegions].Length       = RegionSize;
      MemoryTable[NumRegions].Attributes   = ARM_MEMORY_REGION_ATTRIBUTE_WRITE_BACK;
      NumRegions++;
    }

    if (AsciiStrStr (NodeName, "stage1-entries") != NULL) {
      Stage1EntriesAddress = PAGE_ALIGN (FDTGetProperty64 (DTBAddress, NodeOffset, "base-address"), DEFAULT_PAGE_SIZE);
      Stage1EntriesPages   = FDTGetProperty32 (DTBAddress, NodeOffset, "pages-count");
      DEBUG ((DEBUG_ERROR, "Stage-1 base      = 0x%llx \n", Stage1EntriesAddress));
      DEBUG ((DEBUG_ERROR, "Stage-1 size      = 0x%llx \n", Stage1EntriesPages*DEFAULT_PAGE_SIZE));
      SlabArmSetEntriesSlab (Stage1EntriesAddress, Stage1EntriesPages);
    }

    PrevNodeOffset = NodeOffset;
  }

  ASSERT (NsBufferAddress != 0);
  ASSERT (NsBufferSize != 0);

  /* Last entry must be all 0 */
  MemoryTable[NumRegions].PhysicalBase = 0;
  MemoryTable[NumRegions].VirtualBase  = 0;
  MemoryTable[NumRegions].Length       = 0;
  MemoryTable[NumRegions].Attributes   = 0;

  Status = SlabArmConfigureMmu (MemoryTable, NULL, NULL);
  ASSERT (Status == EFI_SUCCESS);
}

/**
 * The C entry point of the partition.
 *
 * @param  [in] TotalSPMemorySize    Total memory allocated to the SP.
 * @param  [in] DtbAddress           Address of the partition manifest.
 */
VOID
EFIAPI
_ModuleEntryPointC (
  IN UINT64  TotalSPMemorySize,
  IN VOID    *DTBAddress
  )
{
  PE_COFF_LOADER_IMAGE_CONTEXT  ImageContext;
  ARM_SVC_ARGS                  InitMmFoundationSvcArgs;
  EFI_STATUS                    Status;
  INT32                         Ret;
  UINT32                        SectionHeaderOffset;
  UINT16                        NumberOfSections;
  VOID                          *HobStart;
  VOID                          *TeData;
  UINTN                         TeDataSize;
  EFI_PHYSICAL_ADDRESS          ImageBase;

  DEBUG ((DEBUG_ERROR, "EntryPoint: MemorySize=0x%x DTB@0x%x\n", TotalSPMemorySize, DTBAddress));

  ConfigureStage1Translations (TotalSPMemorySize, DTBAddress);

  ZeroMem ((void *)&PayloadBootInfo, sizeof (EFI_SECURE_PARTITION_BOOT_INFO));
  ZeroMem ((void *)&StmmCommBuffers, sizeof (STMM_COMM_BUFFERS));

  /* Check Manifest */
  Status = CheckManifest (DTBAddress);
  if (EFI_ERROR (Status)) {
    goto finish;
  }

  // Get Secure Partition Manager Version Information
  Status = GetSpmVersion ();
  if (EFI_ERROR (Status)) {
    goto finish;
  }

  /* Locate PE/COFF File information for the Standalone MM core module */
  PayloadBootInfo.SpImageBase = GetSpImageBase (DTBAddress);
  Status                      = LocateStandaloneMmCorePeCoffData (
                                  (EFI_FIRMWARE_VOLUME_HEADER *)PayloadBootInfo.SpImageBase,
                                  &TeData,
                                  &TeDataSize
                                  );
  if (EFI_ERROR (Status)) {
    goto finish;
  }

  /* Obtain the PE/COFF Section information for the Standalone MM core module */
  Status = GetStandaloneMmCorePeCoffSections (
             TeData,
             &ImageContext,
             &ImageBase,
             &SectionHeaderOffset,
             &NumberOfSections
             );

  if (EFI_ERROR (Status)) {
    goto finish;
  }

  PayloadBootInfo.SpImageSize = ImageContext.ImageSize;

  /*
   * ImageBase may deviate from ImageContext.ImageAddress if we are dealing
   * with a TE image, in which case the latter points to the actual offset
   * of the image, whereas ImageBase refers to the address where the image
   * would start if the stripped PE headers were still in place. In either
   * case, we need to fix up ImageBase so it refers to the actual current
   * load address.
   */
  ImageBase += (UINTN)TeData - ImageContext.ImageAddress;

  /*
   * Update the memory access permissions of individual sections in the
   * Standalone MM core module
   */
  Status = UpdateMmFoundationPeCoffPermissions (
             &ImageContext,
             ImageBase,
             SectionHeaderOffset,
             NumberOfSections,
             ArmSetMemoryRegionNoExec,
             ArmSetMemoryRegionReadOnly,
             ArmClearMemoryRegionReadOnly
             );

  if (EFI_ERROR (Status)) {
    goto finish;
  }

  if (ImageContext.ImageAddress != (UINTN)TeData) {
    ImageContext.ImageAddress = (UINTN)TeData;
    ArmSetMemoryRegionNoExec (ImageBase, SIZE_4KB);
    ArmClearMemoryRegionReadOnly (ImageBase, SIZE_4KB);

    Status = PeCoffLoaderRelocateImage (&ImageContext);
    ASSERT_EFI_ERROR (Status);
  }

  /* Create Hoblist based upon boot information passed by Manifest */
  Status = GetAndPrintManifestinformation (DTBAddress, TotalSPMemorySize);
  if (EFI_ERROR (Status)) {
    Status = EFI_UNSUPPORTED;
    goto finish;
  }

  HobStart = CreateHobListFromBootInfo (&CpuDriverEntryPoint, &PayloadBootInfo);
  Status   = GetDeviceMemRegions (DTBAddress);
  if (EFI_ERROR (Status)) {
    // Not ideal, but not fatal, so continue.
    DEBUG ((
      DEBUG_ERROR,
      "%a: Failed to install Device Regions Hob %r\n",
      __FUNCTION__,
      Status
      ));
  }

  StmmCommBuffers.DTBAddress = (PHYSICAL_ADDRESS)DTBAddress;

  /* Call the MM Core entry point */
  ProcessModuleEntryPointList (HobStart);

  DEBUG ((DEBUG_INFO, "Shared Cpu Driver EP 0x%lx\n", (UINT64)CpuDriverEntryPoint));

finish:
  if (Status == RETURN_UNSUPPORTED) {
    Ret = -1;
  } else if (Status == RETURN_INVALID_PARAMETER) {
    Ret = -2;
  } else if (Status == EFI_NOT_FOUND) {
    Ret = -7;
  } else {
    Ret = 0;
  }

  ZeroMem (&InitMmFoundationSvcArgs, sizeof (InitMmFoundationSvcArgs));
  InitArmSvcArgs (&InitMmFoundationSvcArgs, &Ret);
  DelegatedEventLoop (&InitMmFoundationSvcArgs);
}
