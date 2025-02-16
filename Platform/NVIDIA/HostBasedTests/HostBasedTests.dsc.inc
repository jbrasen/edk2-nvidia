# Copyright (c) 2021-2022 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
#
# SPDX-License-Identifier: BSD-2-Clause-Patent

!include UnitTestFrameworkPkg/UnitTestFrameworkPkgHost.dsc.inc

[LibraryClasses.common.HOST_APPLICATION]
  UefiLib|MdePkg/Library/UefiLib/UefiLib.inf
  UefiRuntimeLib|MdePkg/Library/UefiRuntimeLib/UefiRuntimeLib.inf
  UefiRuntimeServicesTableLib|MdePkg/Library/UefiRuntimeServicesTableLib/UefiRuntimeServicesTableLib.inf
  UefiDriverEntryPoint|MdePkg/Library/UefiDriverEntryPoint/UefiDriverEntryPoint.inf
  UefiBootServicesTableLib|MdePkg/Library/UefiBootServicesTableLib/UefiBootServicesTableLib.inf
  DevicePathLib|MdePkg/Library/UefiDevicePathLib/UefiDevicePathLib.inf
  FlashStubLib|Silicon/NVIDIA/Library/HostBasedTestStubLib/FlashStubLib/FlashStubLib.inf

[BuildOptions.common.EDKII.HOST_APPLICATION]
!ifdef $(HOSTAPP_STATIC_LINK)
  # Statically link the host-based executable.  This allows us to easily use
  # alternate toolchains at build time and not introduce a runtime dependency.
  GCC:*_*_*_DLINK2_FLAGS = -static -s
!endif

[BuildOptions]
!ifdef $(HOSTAPP_GCC_COVERAGE)
  # Generate coverage data when tests are run.
  GCC:*_*_*_DLINK2_FLAGS = -fprofile-arcs
  GCC:*_*_*_CC_FLAGS = -fprofile-arcs -ftest-coverage
!endif
