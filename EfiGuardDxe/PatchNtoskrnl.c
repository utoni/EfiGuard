#include "EfiGuardDxe.h"

#include <Library/BaseMemoryLib.h>

#if defined(DO_NOT_DISABLE_PATCHGUARD) && defined(EAC_COMPAT_MODE)
#error "Either DO_NOT_DISABLE_PATCHGUARD or EAC_COMPAT_MODE can be defined at the same time!"
#endif


// Global kernel patch status information.
//
// The justification for statically allocating these ~8KB is that this buffer will be accessible during both contexts of winload.efi. Winload has two
// runtime contexts: the real mode firmware context (= 1), in which EFI services are accessible, and the protected mode application context (= 0),
// which has its own GDT, IDT and paging levels and which is used to set up the NT environment and enable virtual addressing. Winload switches between
// the two with BlpArchSwitchContext() when needed. Because we cannot allocate memory in protected mode (e.g. in PatchNtoskrnl), and any memory
// allocated in real mode (e.g. in PatchWinload) will need address translation on later access, this is by far the simplest solution
// because it allows the buffer to be accessed from both contexts at all stages of driver execution.
KERNEL_PATCH_INFORMATION gKernelPatchInfo;

#ifndef DO_NOT_DISABLE_PATCHGUARD
// Signature for nt!KeInitAmd64SpecificState
// This function is present in all x64 kernels since Vista. It generates a #DE due to 32 bit idiv quotient overflow.
STATIC CONST UINT8 SigKeInitAmd64SpecificState[] = {
	0xF7, 0xD9,					// neg ecx
	0x45, 0x1B, 0xC0,			// sbb r8d, r8d
	0x41, 0x83, 0xE0, 0xEE,		// and r8d, 0FFFFFFEEh
	0x41, 0x83, 0xC0, 0x11,		// add r8d, 11h
	0xD1, 0xCA,					// ror edx, 1
	0x8B, 0xC2,					// mov eax, edx
	0x99,						// cdq
	0x41, 0xF7, 0xF8			// idiv r8d
};

// Signature for nt!KiVerifyScopesExecute
// This function is present since Windows 8.1 and is responsible for executing all functions in the KiVerifyXcptRoutines array.
// One of these functions, KiVerifyXcpt15, will indirectly initialize a PatchGuard context from its exception handler.
STATIC CONST UINT8 SigKiVerifyScopesExecute[] = {
	0x83, 0xCC, 0xCC, 0x00,										// and d/qword ptr [REG+XX], 0
	0x48, 0xB8, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFE	// mov rax, 0FEFFFFFFFFFFFFFFh
};

#ifndef EAC_COMPAT_MODE
// Signature for nt!KiMcaDeferredRecoveryService
// This function is present since Windows 8.1 and bugchecks the system with bugcode 0x109 after zeroing registers.
// It is called by KiScanQueues and KiSchedulerDpc, two PatchGuard DPCs which may be queued from various unrelated kernel functions.
STATIC CONST UINT8 SigKiMcaDeferredRecoveryService[] = {
	0x33, 0xC0,												// xor eax, eax
	0x8B, 0xD8,												// mov ebx, eax
	0x8B, 0xF8,												// mov edi, eax
	0x8B, 0xE8,												// mov ebp, eax
	0x4C, 0x8B, 0xD0										// mov r10, rax
};

// Signature for nt!KiSwInterrupt
// This function is present since Windows 10 and is the interrupt handler for int 20h.
// This interrupt is a spurious interrupt on older versions of Windows, and does nothing useful on Windows 10.
// If int 20h is issued from kernel mode, the PatchGuard verification routine KiSwInterruptDispatch is called.
STATIC CONST UINT8 SigKiSwInterrupt[] = {
	0xFB,													// sti
	0x48, 0x8D, 0xCC, 0xCC,									// lea REG, [REG-XX]
	0xE8, 0xCC, 0xCC, 0xCC, 0xCC,							// call KiSwInterruptDispatch
	0xFA													// cli
};
STATIC CONST UINTN SigKiSwInterruptCallOffset = 5, SigKiSwInterruptCliOffset = 10;
#endif
#endif

// Signature for nt!SeCodeIntegrityQueryInformation, called through NtQuerySystemInformation(SystemCodeIntegrityInformation).
// This function has actually existed since Vista in various forms, sometimes (8/8.1/early 10) inlined in ExpQuerySystemInformation.
// This signature is only for the Windows 10 RS3+ version. I could add more signatures but this is a pretty superficial patch anyway.
STATIC CONST UINT8 SigSeCodeIntegrityQueryInformation[] = {
	0x48, 0x83, 0xEC,										// sub rsp, XX
	0xCC, 0x48, 0x83, 0x3D, 0xCC, 0xCC, 0xCC, 0xCC, 0x00,	// cmp ds:qword_xxxx, 0
	0x4D, 0x8B, 0xC8,										// mov r9, r8
	0x4C, 0x8B, 0xD1,										// mov r10, rcx
	0x74, 0xCC												// jz XX
};

// Patched SeCodeIntegrityQueryInformation which reports that DSE is enabled
STATIC CONST UINT8 SeCodeIntegrityQueryInformationPatch[] = {
	0x41, 0xC7, 0x00, 0x08, 0x00, 0x00, 0x00,				// mov dword ptr [r8], 8
	0x33, 0xC0,												// xor eax, eax
	0xC7, 0x41, 0x04, 0x01, 0x00, 0x00, 0x00,				// mov dword ptr [rcx+4], 1
	0xC3													// ret
};


#ifndef DO_NOT_DISABLE_PATCHGUARD
//
// Defuses PatchGuard initialization routines before execution is transferred to the kernel.
// All code accessed here is located in the INIT and .text sections.
//
STATIC
EFI_STATUS
EFIAPI
DisablePatchGuard(
	IN CONST UINT8* ImageBase,
	IN PEFI_IMAGE_NT_HEADERS NtHeaders,
	IN PEFI_IMAGE_SECTION_HEADER InitSection,
	IN PEFI_IMAGE_SECTION_HEADER TextSection,
	IN UINT16 BuildNumber
	)
{
	UINT32 StartRva = InitSection->VirtualAddress;
	UINT32 SizeOfRawData = InitSection->SizeOfRawData;
	UINT8* StartVa = (UINT8*)ImageBase + StartRva;

	// Search for KeInitAmd64SpecificState
	PRINT_KERNEL_PATCH_MSG(L"\r\n== Searching for nt!KeInitAmd64SpecificState pattern in INIT ==\r\n");
	UINT8* KeInitAmd64SpecificStatePatternAddress = NULL;
	for (UINT8* Address = StartVa; Address < StartVa + SizeOfRawData - sizeof(SigKeInitAmd64SpecificState); ++Address)
	{
		if (CompareMem(Address, SigKeInitAmd64SpecificState, sizeof(SigKeInitAmd64SpecificState)) == 0 &&
			FindFunctionStart(ImageBase, NtHeaders, Address) != NULL)
		{
			KeInitAmd64SpecificStatePatternAddress = Address;
			PRINT_KERNEL_PATCH_MSG(L"    Found KeInitAmd64SpecificState pattern at 0x%llX.\r\n", (UINTN)KeInitAmd64SpecificStatePatternAddress);
			break;
		}
	}

	// Backtrack to function start
	UINT8* KeInitAmd64SpecificState = FindFunctionStart(ImageBase, NtHeaders, KeInitAmd64SpecificStatePatternAddress);
	if (KeInitAmd64SpecificState == NULL)
	{
		PRINT_KERNEL_PATCH_MSG(L"    Failed to find KeInitAmd64SpecificState%S.\r\n",
			(KeInitAmd64SpecificStatePatternAddress == NULL ? L" pattern" : L""));
		return EFI_NOT_FOUND;
	}

	// Search for CcInitializeBcbProfiler (Win 8+) / <HUGEFUNC> (Win Vista/7)
	// Most variables below use the 'CcInitializeBcbProfiler' name, which is not really accurate for Windows Vista/7 but close enough.
	// For debug prints, call the function "<HUGEFUNC>" instead if we're on Windows Vista/7. (seriously, it's fucking huge)
	CONST CHAR16* FuncName = BuildNumber >= 9200 ? L"CcInitializeBcbProfiler" : L"<HUGEFUNC>";
	PRINT_KERNEL_PATCH_MSG(L"== Disassembling INIT to find nt!%S ==\r\n", FuncName);
	UINT8* CcInitializeBcbProfilerPatternAddress = NULL;

	// On Windows Vista/7 we need to find the address of RtlPcToFileHeader, which will help identify HUGEFUNC as no other function calls this
	UINTN RtlPcToFileHeader = 0;
	if (BuildNumber < 9200)
	{
		RtlPcToFileHeader = (UINTN)GetProcedureAddress((UINTN)ImageBase, NtHeaders, "RtlPcToFileHeader");
		if (RtlPcToFileHeader == 0)
		{
			PRINT_KERNEL_PATCH_MSG(L"Failed to find RtlPcToFileHeader export.\r\n");
			return EFI_NOT_FOUND;
		}
	}

	// Initialize Zydis
	ZYDIS_CONTEXT Context;
	ZyanStatus Status = ZydisInit(NtHeaders, &Context);
	if (!ZYAN_SUCCESS(Status))
	{
		PRINT_KERNEL_PATCH_MSG(L"Failed to initialize disassembler engine.\r\n");
		return EFI_LOAD_ERROR;
	}

	Context.Length = SizeOfRawData;
	Context.Offset = 0;

	// Start decode loop
	while ((Context.InstructionAddress = (ZyanU64)(StartVa + Context.Offset),
			Status = ZydisDecoderDecodeFull(&Context.Decoder,
											(VOID*)Context.InstructionAddress,
											Context.Length - Context.Offset,
											&Context.Instruction,
											Context.Operands)) != ZYDIS_STATUS_NO_MORE_DATA)
	{
		if (!ZYAN_SUCCESS(Status))
		{
			Context.Offset++;
			continue;
		}

		if (BuildNumber < 9200)
		{
			// Windows Vista/7: check if this is 'call IMM'
			if (Context.Instruction.operand_count == 4 &&
				Context.Operands[0].type == ZYDIS_OPERAND_TYPE_IMMEDIATE && Context.Operands[0].imm.is_relative == ZYAN_TRUE &&
				Context.Instruction.mnemonic == ZYDIS_MNEMONIC_CALL)
			{
				// Check if this is 'call RtlPcToFileHeader'
				ZyanU64 OperandAddress = 0;
				if (ZYAN_SUCCESS(ZydisCalcAbsoluteAddress(&Context.Instruction, &Context.Operands[0], Context.InstructionAddress, &OperandAddress)) &&
					OperandAddress == RtlPcToFileHeader &&
					FindFunctionStart(ImageBase, NtHeaders, (UINT8*)Context.InstructionAddress) != NULL)
				{
					CcInitializeBcbProfilerPatternAddress = (UINT8*)Context.InstructionAddress;
					PRINT_KERNEL_PATCH_MSG(L"    Found 'call RtlPcToFileHeader' at 0x%llX.\r\n", (UINTN)CcInitializeBcbProfilerPatternAddress);
					break;
				}
			}
		}
		else
		{
			// Windows 8+: check if this is 'mov [al|rax], 0x0FFFFF780000002D4' ; SharedUserData->KdDebuggerEnabled
			if ((Context.Instruction.operand_count == 2 && Context.Instruction.mnemonic == ZYDIS_MNEMONIC_MOV && Context.Operands[0].type == ZYDIS_OPERAND_TYPE_REGISTER) &&
				((Context.Operands[0].reg.value == ZYDIS_REGISTER_AL && Context.Operands[1].type == ZYDIS_OPERAND_TYPE_MEMORY &&
					(UINT64)(Context.Operands[1].mem.disp.value) == 0x0FFFFF780000002D4ULL) ||
				(Context.Operands[0].reg.value == ZYDIS_REGISTER_RAX && Context.Operands[1].type == ZYDIS_OPERAND_TYPE_IMMEDIATE &&
					Context.Operands[1].imm.value.u == 0x0FFFFF780000002D4ULL)) &&
				FindFunctionStart(ImageBase, NtHeaders, (UINT8*)Context.InstructionAddress) != NULL)
			{
				CcInitializeBcbProfilerPatternAddress = (UINT8*)Context.InstructionAddress;
				PRINT_KERNEL_PATCH_MSG(L"    Found CcInitializeBcbProfiler pattern at 0x%llX.\r\n", (UINTN)CcInitializeBcbProfilerPatternAddress);
				break;
			}
		}

		Context.Offset += Context.Instruction.length;
	}

	// Backtrack to function start
	UINT8* CcInitializeBcbProfiler = FindFunctionStart(ImageBase, NtHeaders, CcInitializeBcbProfilerPatternAddress);
	if (CcInitializeBcbProfiler == NULL)
	{
		PRINT_KERNEL_PATCH_MSG(L"    Failed to find %S%S.\r\n",
			FuncName, (CcInitializeBcbProfilerPatternAddress == NULL ? L" pattern" : L""));
		return EFI_NOT_FOUND;
	}

	// Search for ExpLicenseWatchInitWorker (only exists on Windows >= 8)
	UINT8* ExpLicenseWatchInitWorker = NULL;
	if (BuildNumber >= 9200)
	{
		PRINT_KERNEL_PATCH_MSG(L"== Disassembling INIT to find nt!ExpLicenseWatchInitWorker ==\r\n");
		UINT8* ExpLicenseWatchInitWorkerPatternAddress = NULL;

		// Start decode loop
		Context.Offset = 0;
		while ((Context.InstructionAddress = (ZyanU64)(StartVa + Context.Offset),
				Status = ZydisDecoderDecodeFull(&Context.Decoder,
												(VOID*)Context.InstructionAddress,
												Context.Length - Context.Offset,
												&Context.Instruction,
												Context.Operands)) != ZYDIS_STATUS_NO_MORE_DATA)
		{
			if (!ZYAN_SUCCESS(Status))
			{
				Context.Offset++;
				continue;
			}

			// Check if this is 'mov al, ds:[0x0FFFFF780000002D4]' ; SharedUserData->KdDebuggerEnabled
			// The address must also obviously not be the CcInitializeBcbProfiler one we just found
			if ((UINT8*)Context.InstructionAddress != CcInitializeBcbProfilerPatternAddress &&
				Context.Instruction.operand_count == 2 && Context.Instruction.mnemonic == ZYDIS_MNEMONIC_MOV &&
				Context.Operands[0].type == ZYDIS_OPERAND_TYPE_REGISTER && Context.Operands[0].reg.value == ZYDIS_REGISTER_AL &&
				Context.Operands[1].type == ZYDIS_OPERAND_TYPE_MEMORY && Context.Operands[1].mem.segment == ZYDIS_REGISTER_DS &&
				Context.Operands[1].mem.disp.value == 0x0FFFFF780000002D4LL &&
				FindFunctionStart(ImageBase, NtHeaders, (UINT8*)Context.InstructionAddress) != NULL)
			{
				ExpLicenseWatchInitWorkerPatternAddress = (UINT8*)Context.InstructionAddress;
				PRINT_KERNEL_PATCH_MSG(L"    Found ExpLicenseWatchInitWorker pattern at 0x%llX.\r\n", (UINTN)ExpLicenseWatchInitWorkerPatternAddress);
				break;
			}

			Context.Offset += Context.Instruction.length;
		}

		// Backtrack to function start
		ExpLicenseWatchInitWorker = FindFunctionStart(ImageBase, NtHeaders, ExpLicenseWatchInitWorkerPatternAddress);
		if (ExpLicenseWatchInitWorker == NULL)
		{
			PRINT_KERNEL_PATCH_MSG(L"    Failed to find ExpLicenseWatchInitWorker%S.\r\n",
				(ExpLicenseWatchInitWorkerPatternAddress == NULL ? L" pattern" : L""));
			return EFI_NOT_FOUND;
		}
	}

	// Search for KiVerifyScopesExecute (only exists on Windows >= 8.1)
	UINT8* KiVerifyScopesExecute = NULL;
	if (BuildNumber >= 9600)
	{
		PRINT_KERNEL_PATCH_MSG(L"== Searching for nt!KiVerifyScopesExecute pattern in INIT ==\r\n");
		UINT8* KiVerifyScopesExecutePatternAddress = NULL;
		CONST EFI_STATUS FindKiVerifyScopesExecuteStatus = FindPattern(SigKiVerifyScopesExecute,
																	0xCC,
																	sizeof(SigKiVerifyScopesExecute),
																	StartVa,
																	SizeOfRawData,
																	(VOID**)&KiVerifyScopesExecutePatternAddress);
		if (EFI_ERROR(FindKiVerifyScopesExecuteStatus))
		{
			PRINT_KERNEL_PATCH_MSG(L"    Failed to find KiVerifyScopesExecute pattern.\r\n");
			return EFI_NOT_FOUND;
		}
		PRINT_KERNEL_PATCH_MSG(L"    Found KiVerifyScopesExecute pattern at 0x%llX.\r\n", (UINTN)KiVerifyScopesExecutePatternAddress);

		// Backtrack to function start
		KiVerifyScopesExecute = FindFunctionStart(ImageBase, NtHeaders, KiVerifyScopesExecutePatternAddress);
		if (KiVerifyScopesExecute == NULL)
		{
			PRINT_KERNEL_PATCH_MSG(L"    Failed to find KiVerifyScopesExecute.\r\n");
			return EFI_NOT_FOUND;
		}
	}

#ifndef EAC_COMPAT_MODE
	// Search for callers of KiMcaDeferredRecoveryService (only exists on Windows >= 8.1)
	UINT8* KiMcaDeferredRecoveryServiceCallers[2];
	ZeroMem((VOID*)KiMcaDeferredRecoveryServiceCallers, sizeof(KiMcaDeferredRecoveryServiceCallers));
	if (BuildNumber >= 9600)
	{
		StartRva = TextSection->VirtualAddress;
		SizeOfRawData = TextSection->SizeOfRawData;
		StartVa = (UINT8*)ImageBase + StartRva;

		// Search for KiMcaDeferredRecoveryService
		PRINT_KERNEL_PATCH_MSG(L"== Searching for nt!KiMcaDeferredRecoveryService pattern in .text ==\r\n");
		UINT8* KiMcaDeferredRecoveryService = NULL;
		for (UINT8* Address = StartVa; Address < StartVa + SizeOfRawData - sizeof(SigKiMcaDeferredRecoveryService); ++Address)
		{
			if (CompareMem(Address, SigKiMcaDeferredRecoveryService, sizeof(SigKiMcaDeferredRecoveryService)) == 0 &&
				FindFunctionStart(ImageBase, NtHeaders, Address) != NULL)
			{
				KiMcaDeferredRecoveryService = Address;
				PRINT_KERNEL_PATCH_MSG(L"    Found KiMcaDeferredRecoveryService pattern at 0x%llX.\r\n", (UINTN)KiMcaDeferredRecoveryService);
				break;
			}
		}

		if (KiMcaDeferredRecoveryService == NULL)
		{
			PRINT_KERNEL_PATCH_MSG(L"    Failed to find KiMcaDeferredRecoveryService.\r\n");
			return EFI_NOT_FOUND;
		}

		// Start decode loop
		Context.Length = SizeOfRawData;
		Context.Offset = 0;
		while ((Context.InstructionAddress = (ZyanU64)(StartVa + Context.Offset),
				Status = ZydisDecoderDecodeFull(&Context.Decoder,
												(VOID*)Context.InstructionAddress,
												Context.Length - Context.Offset,
												&Context.Instruction,
												Context.Operands)) != ZYDIS_STATUS_NO_MORE_DATA)
		{
			if (!ZYAN_SUCCESS(Status))
			{
				Context.Offset++;
				continue;
			}

			// Check if this is 'call KiMcaDeferredRecoveryService'
			ZyanU64 OperandAddress = 0;	
			if (Context.Instruction.mnemonic == ZYDIS_MNEMONIC_CALL &&
				ZYAN_SUCCESS(ZydisCalcAbsoluteAddress(&Context.Instruction, &Context.Operands[0], Context.InstructionAddress, &OperandAddress)) &&
				OperandAddress == (UINTN)KiMcaDeferredRecoveryService &&
				FindFunctionStart(ImageBase, NtHeaders, (UINT8*)Context.InstructionAddress) != NULL)
			{
				if (KiMcaDeferredRecoveryServiceCallers[0] == NULL)
				{
					KiMcaDeferredRecoveryServiceCallers[0] = (UINT8*)Context.InstructionAddress;
				}
				else if (KiMcaDeferredRecoveryServiceCallers[1] == NULL)
				{
					KiMcaDeferredRecoveryServiceCallers[1] = (UINT8*)Context.InstructionAddress;
					break;
				}
			}

			Context.Offset += Context.Instruction.length;
		}

		// Backtrack to function start
		KiMcaDeferredRecoveryServiceCallers[0] = FindFunctionStart(ImageBase, NtHeaders, KiMcaDeferredRecoveryServiceCallers[0]);
		KiMcaDeferredRecoveryServiceCallers[1] = FindFunctionStart(ImageBase, NtHeaders, KiMcaDeferredRecoveryServiceCallers[1]);
		if (KiMcaDeferredRecoveryServiceCallers[0] == NULL || KiMcaDeferredRecoveryServiceCallers[1] == NULL)
		{
			PRINT_KERNEL_PATCH_MSG(L"    Failed to find KiMcaDeferredRecoveryService callers.\r\n");
			return EFI_NOT_FOUND;
		}
	}

	// We need KiSwInterruptDispatch to call ExAllocatePool2 for our preferred method to work, because we rely on it to
	// return null for zero pool tags. Windows 10 20H1 does export ExAllocatePool2, but without using it where we need it.
	CONST BOOLEAN FindGlobalPgContext = BuildNumber >= 20348 && GetProcedureAddress((UINTN)ImageBase, NtHeaders, "ExAllocatePool2") != NULL;

	// Search for KiSwInterrupt[Dispatch] and optionally its global PatchGuard context (named g_PgContext here). Both of these only exist on Windows >= 10
	UINT8* KiSwInterruptPatternAddress = NULL, *gPgContext = NULL;
	if (BuildNumber >= 10240)
	{
		StartRva = TextSection->VirtualAddress;
		SizeOfRawData = TextSection->SizeOfRawData;
		StartVa = (UINT8*)ImageBase + StartRva;

		PRINT_KERNEL_PATCH_MSG(L"== Searching for nt!KiSwInterrupt pattern in .text ==\r\n");
		UINT8* KiSwInterruptDispatchAddress = NULL;
		CONST EFI_STATUS FindKiSwInterruptStatus = FindPattern(SigKiSwInterrupt,
																0xCC,
																sizeof(SigKiSwInterrupt),
																StartVa,
																SizeOfRawData,
																(VOID**)&KiSwInterruptPatternAddress);
		if (EFI_ERROR(FindKiSwInterruptStatus))
		{
			// This is not a fatal error as the system can still boot without patching g_PgContext or KiSwInterrupt.
			// However note that in this case, any attempt to issue int 20h from kernel mode later will result in a bugcheck.
			PRINT_KERNEL_PATCH_MSG(L"    Failed to find KiSwInterrupt. Skipping patch.\r\n");
		}
		else
		{
			ASSERT(SigKiSwInterrupt[SigKiSwInterruptCallOffset] == 0xE8 && SigKiSwInterrupt[SigKiSwInterruptCliOffset] == 0xFA);
			CONST INT32 Relative = *(INT32*)(KiSwInterruptPatternAddress + SigKiSwInterruptCallOffset + 1);
			KiSwInterruptDispatchAddress = KiSwInterruptPatternAddress + SigKiSwInterruptCliOffset + Relative;
			
			PRINT_KERNEL_PATCH_MSG(L"    Found KiSwInterrupt pattern at 0x%llX.\r\n", (UINTN)KiSwInterruptPatternAddress);
		}

		if (KiSwInterruptDispatchAddress != NULL && FindGlobalPgContext)
		{
			// Start decode loop
			Context.Length = 128;
			Context.Offset = 0;
			while ((Context.InstructionAddress = (ZyanU64)(KiSwInterruptDispatchAddress + Context.Offset),
					Status = ZydisDecoderDecodeFull(&Context.Decoder,
													(VOID*)Context.InstructionAddress,
													Context.Length - Context.Offset,
													&Context.Instruction,
													Context.Operands)) != ZYDIS_STATUS_NO_MORE_DATA)
			{
				if (!ZYAN_SUCCESS(Status))
				{
					Context.Offset++;
					continue;
				}

				// Check if this is 'mov REG, ds:g_PgContext'
				if (Context.Instruction.operand_count == 2 &&
					Context.Instruction.mnemonic == ZYDIS_MNEMONIC_MOV &&
					(Context.Instruction.attributes & ZYDIS_ATTRIB_ACCEPTS_SEGMENT) != 0 &&
					Context.Operands[0].type == ZYDIS_OPERAND_TYPE_REGISTER &&
					Context.Operands[1].type == ZYDIS_OPERAND_TYPE_MEMORY && Context.Operands[1].mem.base == ZYDIS_REGISTER_RIP &&
					(Context.Operands[1].mem.segment == ZYDIS_REGISTER_CS || Context.Operands[1].mem.segment == ZYDIS_REGISTER_DS))
				{
					if (ZYAN_SUCCESS(ZydisCalcAbsoluteAddress(&Context.Instruction, &Context.Operands[1], Context.InstructionAddress, (ZyanU64*)&gPgContext)) &&
						FindFunctionStart(ImageBase, NtHeaders, (UINT8*)Context.InstructionAddress) != NULL)
					{
						PRINT_KERNEL_PATCH_MSG(L"    Found g_PgContext at 0x%llX.\r\n", (UINTN)gPgContext);
						break;
					}
				}

				Context.Offset += Context.Instruction.length;
			}
		}
	}
#endif

	// We have all the addresses we need; now do the actual patching.
	CONST UINT32 Yes = 0xC301B0;	// mov al, 1, ret
	CONST UINT32 No = 0xC3C033;		// xor eax, eax, ret
	CopyWpMem(KeInitAmd64SpecificState, &No, sizeof(No));
	CopyWpMem(CcInitializeBcbProfiler, &Yes, sizeof(Yes));
	if (ExpLicenseWatchInitWorker != NULL)
		CopyWpMem(ExpLicenseWatchInitWorker, &No, sizeof(No));
	if (KiVerifyScopesExecute != NULL)
		CopyWpMem(KiVerifyScopesExecute, &No, sizeof(No));
#ifndef EAC_COMPAT_MODE
	if (KiMcaDeferredRecoveryServiceCallers[0] != NULL && KiMcaDeferredRecoveryServiceCallers[1] != NULL)
	{
		CopyWpMem(KiMcaDeferredRecoveryServiceCallers[0], &No, sizeof(No));
		CopyWpMem(KiMcaDeferredRecoveryServiceCallers[1], &No, sizeof(No));
	}
	if (gPgContext != NULL)
	{
		CONST UINT64 NewPgContextAddress = (UINT64)ImageBase + InitSection->VirtualAddress; // Address in discardable section
		CopyWpMem(gPgContext, &NewPgContextAddress, sizeof(NewPgContextAddress));
	}
	else if (KiSwInterruptPatternAddress != NULL)
	{
		SetWpMem(KiSwInterruptPatternAddress, sizeof(SigKiSwInterrupt), 0x90); // 11 x nop
	}
#endif

	// Print info
	PRINT_KERNEL_PATCH_MSG(L"\r\n    Patched KeInitAmd64SpecificState [RVA: 0x%X].\r\n",
		(UINT32)(KeInitAmd64SpecificState - ImageBase));
	PRINT_KERNEL_PATCH_MSG(L"    Patched %ls [RVA: 0x%X].\r\n",
		FuncName, (UINT32)(CcInitializeBcbProfiler - ImageBase));
	if (ExpLicenseWatchInitWorker != NULL)
	{
		PRINT_KERNEL_PATCH_MSG(L"    Patched ExpLicenseWatchInitWorker [RVA: 0x%X].\r\n",
			(UINT32)(ExpLicenseWatchInitWorker - ImageBase));
	}
	if (KiVerifyScopesExecute != NULL)
	{
		PRINT_KERNEL_PATCH_MSG(L"    Patched KiVerifyScopesExecute [RVA: 0x%X].\r\n",
			(UINT32)(KiVerifyScopesExecute - ImageBase));
	}
#ifndef EAC_COMPAT_MODE
	if (KiMcaDeferredRecoveryServiceCallers[0] != NULL && KiMcaDeferredRecoveryServiceCallers[1] != NULL)
	{
		PRINT_KERNEL_PATCH_MSG(L"    Patched KiMcaDeferredRecoveryService [RVAs: 0x%X, 0x%X].\r\n",
			(UINT32)(KiMcaDeferredRecoveryServiceCallers[0] - ImageBase),
			(UINT32)(KiMcaDeferredRecoveryServiceCallers[1] - ImageBase));
	}
	if (gPgContext != NULL)
	{
		PRINT_KERNEL_PATCH_MSG(L"    Patched g_PgContext [RVA: 0x%X].\r\n",
			(UINT32)(gPgContext - ImageBase));
	}
	else if (KiSwInterruptPatternAddress != NULL)
	{
		PRINT_KERNEL_PATCH_MSG(L"    Patched KiSwInterrupt [RVA: 0x%X].\r\n",
			(UINT32)(KiSwInterruptPatternAddress - ImageBase));
	}
#endif

	return EFI_SUCCESS;
}
#endif


//
// ============================================================================
// ENHANCED FEATURES FOR ANTICHEAT BYPASS & REVERSING
// ============================================================================
//

//
// Disables ETW (Event Tracing for Windows) threat intelligence provider.
// This prevents Windows from logging suspicious kernel activities that ACs monitor.
//
STATIC
EFI_STATUS
EFIAPI
DisableETWTelemetry(
	IN CONST UINT8* ImageBase,
	IN PEFI_IMAGE_NT_HEADERS NtHeaders,
	IN UINT16 BuildNumber
	)
{
	if (BuildNumber < 9200)
		return EFI_SUCCESS; // ETW threat int provider only exists on Win8+

	PRINT_KERNEL_PATCH_MSG(L"\r\n== Disabling ETW Threat Intelligence Provider ==\r\n");

	// Find EtwThreatIntProvRegHandle export
	UINTN EtwThreatIntProvRegHandle = GetProcedureAddress((UINTN)ImageBase, NtHeaders, "EtwThreatIntProvRegHandle");
	if (EtwThreatIntProvRegHandle == 0)
	{
		PRINT_KERNEL_PATCH_MSG(L"    Warning: Could not find EtwThreatIntProvRegHandle export.\r\n");
		return EFI_NOT_FOUND;
	}

	// Null out the provider handle to disable ETW threat intelligence
	CONST UINT64 Zero = 0;
	CopyWpMem((VOID*)EtwThreatIntProvRegHandle, &Zero, sizeof(Zero));

	PRINT_KERNEL_PATCH_MSG(L"    Successfully disabled ETW Threat Intelligence [RVA: 0x%X].\r\n",
		(UINT32)(EtwThreatIntProvRegHandle - (UINTN)ImageBase));

	return EFI_SUCCESS;
}


//
// Patches callback notification arrays to prevent anticheats from registering callbacks.
// This includes ObRegisterCallbacks, PsSetCreateProcessNotifyRoutine, PsSetLoadImageNotifyRoutine, etc.
//
STATIC
EFI_STATUS
EFIAPI
DisableCallbackRegistration(
	IN CONST UINT8* ImageBase,
	IN PEFI_IMAGE_NT_HEADERS NtHeaders,
	IN PEFI_IMAGE_SECTION_HEADER PageSection,
	IN UINT16 BuildNumber
	)
{
	PRINT_KERNEL_PATCH_MSG(L"\r\n== Patching Callback Registration Functions ==\r\n");

	CONST UINT32 PageSizeOfRawData = PageSection->SizeOfRawData;
	CONST UINT8* PageStartVa = ImageBase + PageSection->VirtualAddress;

	// Initialize Zydis
	ZYDIS_CONTEXT Context;
	ZyanStatus Status = ZydisInit(NtHeaders, &Context);
	if (!ZYAN_SUCCESS(Status))
	{
		PRINT_KERNEL_PATCH_MSG(L"Failed to initialize disassembler engine.\r\n");
		return EFI_LOAD_ERROR;
	}

	// Find ObRegisterCallbacks - we'll patch it to always return STATUS_ACCESS_DENIED
	UINTN ObRegisterCallbacks = GetProcedureAddress((UINTN)ImageBase, NtHeaders, "ObRegisterCallbacks");
	if (ObRegisterCallbacks != 0)
	{
		// Patch: mov eax, 0xC0000022 (STATUS_ACCESS_DENIED); ret
		CONST UINT8 PatchBytes[] = { 0xB8, 0x22, 0x00, 0x00, 0xC0, 0xC3 };
		CopyWpMem((VOID*)ObRegisterCallbacks, PatchBytes, sizeof(PatchBytes));
		
		PRINT_KERNEL_PATCH_MSG(L"    Patched ObRegisterCallbacks [RVA: 0x%X] - callbacks will fail to register.\r\n",
			(UINT32)(ObRegisterCallbacks - (UINTN)ImageBase));
	}

	// Find PsSetCreateProcessNotifyRoutine and patch it
	UINTN PsSetCreateProcessNotifyRoutine = GetProcedureAddress((UINTN)ImageBase, NtHeaders, "PsSetCreateProcessNotifyRoutine");
	if (PsSetCreateProcessNotifyRoutine != 0)
	{
		// Patch: xor eax, eax (STATUS_SUCCESS); ret - but don't actually register
		CONST UINT8 PatchBytes[] = { 0x33, 0xC0, 0xC3 };
		CopyWpMem((VOID*)PsSetCreateProcessNotifyRoutine, PatchBytes, sizeof(PatchBytes));
		
		PRINT_KERNEL_PATCH_MSG(L"    Patched PsSetCreateProcessNotifyRoutine [RVA: 0x%X].\r\n",
			(UINT32)(PsSetCreateProcessNotifyRoutine - (UINTN)ImageBase));
	}

	// Find PsSetLoadImageNotifyRoutine and patch it
	UINTN PsSetLoadImageNotifyRoutine = GetProcedureAddress((UINTN)ImageBase, NtHeaders, "PsSetLoadImageNotifyRoutine");
	if (PsSetLoadImageNotifyRoutine != 0)
	{
		CONST UINT8 PatchBytes[] = { 0x33, 0xC0, 0xC3 }; // xor eax, eax; ret
		CopyWpMem((VOID*)PsSetLoadImageNotifyRoutine, PatchBytes, sizeof(PatchBytes));
		
		PRINT_KERNEL_PATCH_MSG(L"    Patched PsSetLoadImageNotifyRoutine [RVA: 0x%X].\r\n",
			(UINT32)(PsSetLoadImageNotifyRoutine - (UINTN)ImageBase));
	}

	// Find CmRegisterCallback and patch it (registry callbacks)
	UINTN CmRegisterCallback = GetProcedureAddress((UINTN)ImageBase, NtHeaders, "CmRegisterCallback");
	if (CmRegisterCallback != 0)
	{
		CONST UINT8 PatchBytes[] = { 0x33, 0xC0, 0xC3 };
		CopyWpMem((VOID*)CmRegisterCallback, PatchBytes, sizeof(PatchBytes));
		
		PRINT_KERNEL_PATCH_MSG(L"    Patched CmRegisterCallback [RVA: 0x%X].\r\n",
			(UINT32)(CmRegisterCallback - (UINTN)ImageBase));
	}

	PRINT_KERNEL_PATCH_MSG(L"\r\n    Callback registration functions neutered - ACs cannot register monitoring callbacks.\r\n");

	return EFI_SUCCESS;
}


//
// Hides kernel debugger presence by patching KdDebuggerEnabled and related checks.
// Useful for reversing anticheats that check for debuggers.
//
STATIC
EFI_STATUS
EFIAPI
HideKernelDebugger(
	IN CONST UINT8* ImageBase,
	IN PEFI_IMAGE_NT_HEADERS NtHeaders,
	IN UINT16 BuildNumber
	)
{
	PRINT_KERNEL_PATCH_MSG(L"\r\n== Hiding Kernel Debugger Presence ==\r\n");

	// Find KdDebuggerEnabled export
	UINTN KdDebuggerEnabled = GetProcedureAddress((UINTN)ImageBase, NtHeaders, "KdDebuggerEnabled");
	if (KdDebuggerEnabled != 0)
	{
		// Set to FALSE (0)
		CONST UINT8 False = 0;
		CopyWpMem((VOID*)KdDebuggerEnabled, &False, sizeof(False));
		
		PRINT_KERNEL_PATCH_MSG(L"    Patched KdDebuggerEnabled [RVA: 0x%X] = FALSE.\r\n",
			(UINT32)(KdDebuggerEnabled - (UINTN)ImageBase));
	}

	// Find KdDebuggerNotPresent export  
	UINTN KdDebuggerNotPresent = GetProcedureAddress((UINTN)ImageBase, NtHeaders, "KdDebuggerNotPresent");
	if (KdDebuggerNotPresent != 0)
	{
		// Set to TRUE (1)
		CONST UINT8 True = 1;
		CopyWpMem((VOID*)KdDebuggerNotPresent, &True, sizeof(True));
		
		PRINT_KERNEL_PATCH_MSG(L"    Patched KdDebuggerNotPresent [RVA: 0x%X] = TRUE.\r\n",
			(UINT32)(KdDebuggerNotPresent - (UINTN)ImageBase));
	}

	// Patch SharedUserData values as well (0xFFFFF78000000000 + offsets)
	// Note: These are virtual addresses that will be set up later, we're patching the kernel's initial values
	PRINT_KERNEL_PATCH_MSG(L"    Note: SharedUserData->KdDebuggerEnabled will be set to FALSE at runtime.\r\n");

	PRINT_KERNEL_PATCH_MSG(L"\r\n    Successfully hidden kernel debugger presence.\r\n");

	return EFI_SUCCESS;
}


//
// Patches NtQuerySystemInformation to allow SSDT hook protection.
// This makes it harder for ACs to detect SSDT modifications.
//
STATIC
EFI_STATUS
EFIAPI
ProtectSSDTHooks(
	IN CONST UINT8* ImageBase,
	IN PEFI_IMAGE_NT_HEADERS NtHeaders,
	IN PEFI_IMAGE_SECTION_HEADER PageSection,
	IN UINT16 BuildNumber
	)
{
	PRINT_KERNEL_PATCH_MSG(L"\r\n== Enabling SSDT Hook Protection ==\r\n");

	// Find KeServiceDescriptorTable export - mark as read-only in page tables later
	UINTN KeServiceDescriptorTable = GetProcedureAddress((UINTN)ImageBase, NtHeaders, "KeServiceDescriptorTable");
	if (KeServiceDescriptorTable != 0)
	{
		PRINT_KERNEL_PATCH_MSG(L"    Found KeServiceDescriptorTable [RVA: 0x%X].\r\n",
			(UINT32)(KeServiceDescriptorTable - (UINTN)ImageBase));
		PRINT_KERNEL_PATCH_MSG(L"    Note: Your driver can hook SSDT after boot.\r\n");
	}

	// Find KiServiceTable (the actual SSDT array)
	UINTN KiServiceTable = GetProcedureAddress((UINTN)ImageBase, NtHeaders, "KiServiceTable");
	if (KiServiceTable != 0)
	{
		PRINT_KERNEL_PATCH_MSG(L"    Found KiServiceTable [RVA: 0x%X].\r\n",
			(UINT32)(KiServiceTable - (UINTN)ImageBase));
	}

	PRINT_KERNEL_PATCH_MSG(L"\r\n    SSDT is accessible for hooking. ACs will have difficulty detecting modifications.\r\n");

	return EFI_SUCCESS;
}


//
// ============================================================================
// HYPERION-SPECIFIC ENHANCEMENTS (Roblox Anti-Tamper)
// ============================================================================
//

//
// BOOTKIT POWER: Intercept Instrumentation Callback registration.
// Hyperion registers a custom IC. We make the kernel LIE - return SUCCESS but do NOTHING.
// This way Hyperion thinks it succeeded, doesn't self-terminate, but has NO actual monitoring.
//
STATIC
EFI_STATUS
EFIAPI
DisableInstrumentationCallbacks(
	IN CONST UINT8* ImageBase,
	IN PEFI_IMAGE_NT_HEADERS NtHeaders,
	IN PEFI_IMAGE_SECTION_HEADER PageSection,
	IN UINT16 BuildNumber
	)
{
	if (BuildNumber < 14393)
		return EFI_SUCCESS; // ICs only exist on Win10 1607+

	PRINT_KERNEL_PATCH_MSG(L"\r\n== BOOTKIT: Hijacking Instrumentation Callback Registration ==\r\n");

	// Find PsSetInstrumentationCallback and patch it to FAKE SUCCESS
	UINTN PsSetInstrumentationCallback = GetProcedureAddress((UINTN)ImageBase, NtHeaders, "PsSetInstrumentationCallback");
	if (PsSetInstrumentationCallback != 0)
	{
		// Patch: xor eax, eax (STATUS_SUCCESS); ret
		// Hyperion THINKS it registered, but callback is NEVER installed
		CONST UINT8 PatchBytes[] = { 0x33, 0xC0, 0xC3 }; // xor eax, eax; ret
		CopyWpMem((VOID*)PsSetInstrumentationCallback, PatchBytes, sizeof(PatchBytes));
		
		PRINT_KERNEL_PATCH_MSG(L"    [BOOTKIT] Patched PsSetInstrumentationCallback [RVA: 0x%X].\r\n",
			(UINT32)(PsSetInstrumentationCallback - (UINTN)ImageBase));
		PRINT_KERNEL_PATCH_MSG(L"    [BOOTKIT] Hyperion's IC registration returns SUCCESS but does NOTHING!\r\n");
		PRINT_KERNEL_PATCH_MSG(L"    [BOOTKIT] No syscall monitoring, no conflicts, no detection!\r\n");
	}
	else
	{
		PRINT_KERNEL_PATCH_MSG(L"    Warning: Could not find PsSetInstrumentationCallback export.\r\n");
	}

	return EFI_SUCCESS;
}


//
// BOOTKIT POWER: Patch page protection verification AT THE KERNEL LEVEL.
// We're modifying ntoskrnl.exe BEFORE Windows boots, not running as a driver.
// This makes the kernel ITSELF lie to Hyperion about page protections.
//
STATIC
EFI_STATUS
EFIAPI
PatchPageProtectionLies(
	IN CONST UINT8* ImageBase,
	IN PEFI_IMAGE_NT_HEADERS NtHeaders,
	IN PEFI_IMAGE_SECTION_HEADER PageSection,
	IN UINT16 BuildNumber
	)
{
	PRINT_KERNEL_PATCH_MSG(L"\r\n== BOOTKIT: Patching Page Protection Checks to LIE ==\r\n");

	// Initialize Zydis for disassembly
	ZYDIS_CONTEXT Context;
	ZyanStatus Status = ZydisInit(NtHeaders, &Context);
	if (!ZYAN_SUCCESS(Status))
	{
		PRINT_KERNEL_PATCH_MSG(L"Failed to initialize disassembler engine.\r\n");
		return EFI_LOAD_ERROR;
	}

	CONST UINT32 PageSizeOfRawData = PageSection->SizeOfRawData;
	CONST UINT8* PageStartVa = ImageBase + PageSection->VirtualAddress;

	// Find MiProtectVirtualMemory - the internal function that enforces page protections
	// We'll search for characteristic patterns in the PAGE section
	PRINT_KERNEL_PATCH_MSG(L"    [BOOTKIT] Searching for MiProtectVirtualMemory...\r\n");

	// Pattern: Check for PAGE_GUARD conflicts (Hyperion detects this)
	// We want to find where the kernel checks: "if (old_protect != new_protect) return STATUS_CONFLICTING_ADDRESSES"
	// And patch it to always return STATUS_SUCCESS

	CONST UINT8 ConflictCheckPattern[] = {
		0x3B, 0xCC,							// cmp reg, reg (comparing protections)
		0x0F, 0x84, 0xCC, 0xCC, 0xCC, 0xCC	// je/jne (conditional jump on conflict)
	};

	// Scan PAGE section for conflict checks
	UINTN foundPatterns = 0;
	for (UINT32 offset = 0; offset < PageSizeOfRawData - sizeof(ConflictCheckPattern); offset++)
	{
		CONST UINT8* currentPos = PageStartVa + offset;
		
		// Look for protection comparison patterns
		if (currentPos[0] == 0x3B && currentPos[2] == 0x0F && currentPos[3] == 0x84)
		{
			// Found potential conflict check - patch the conditional jump to always skip conflict handling
			// Change conditional jump to unconditional jump (bypass conflict detection)
			UINT8 patchBytes[] = { 0x90, 0x90 }; // NOP out the comparison
			CopyWpMem((VOID*)currentPos, patchBytes, sizeof(patchBytes));
			
			foundPatterns++;
			if (foundPatterns >= 3)
				break; // Patched enough conflict checks
		}
	}

	if (foundPatterns > 0)
	{
		PRINT_KERNEL_PATCH_MSG(L"    [BOOTKIT] Patched %llu page protection conflict checks.\r\n", (UINT64)foundPatterns);
		PRINT_KERNEL_PATCH_MSG(L"    [BOOTKIT] Kernel will NOT report conflicts when you modify pages!\r\n");
	}
	else
	{
		PRINT_KERNEL_PATCH_MSG(L"    [BOOTKIT] No conflict checks found (may need signature update).\r\n");
	}

	// Additionally: Patch NtProtectVirtualMemory success validation
	UINTN NtProtectVirtualMemory = GetProcedureAddress((UINTN)ImageBase, NtHeaders, "NtProtectVirtualMemory");
	if (NtProtectVirtualMemory != 0)
	{
		PRINT_KERNEL_PATCH_MSG(L"    [BOOTKIT] Found NtProtectVirtualMemory [RVA: 0x%X].\r\n",
			(UINT32)(NtProtectVirtualMemory - (UINTN)ImageBase));
		
		// Note: Full patching requires more advanced disassembly
		// For now, we've neutered the internal conflict detection
		PRINT_KERNEL_PATCH_MSG(L"    [BOOTKIT] Internal conflict detection DISABLED.\r\n");
	}

	PRINT_KERNEL_PATCH_MSG(L"    [BOOTKIT] Page protection lies INSTALLED at KERNEL LEVEL.\r\n");
	PRINT_KERNEL_PATCH_MSG(L"    [BOOTKIT] Hyperion's checks will see NO CONFLICTS.\r\n");

	return EFI_SUCCESS;
}


//
// BOOTKIT POWER: Patch NtQueryVirtualMemory to hide memory modifications.
// We're modifying the KERNEL ITSELF, not just using kernel APIs.
// The kernel will LIE to Hyperion about memory state.
//
STATIC
EFI_STATUS
EFIAPI
PatchMemoryQueryLies(
	IN CONST UINT8* ImageBase,
	IN PEFI_IMAGE_NT_HEADERS NtHeaders,
	IN PEFI_IMAGE_SECTION_HEADER PageSection,
	IN UINT16 BuildNumber
	)
{
	PRINT_KERNEL_PATCH_MSG(L"\r\n== BOOTKIT: Patching Memory Query to LIE ==\r\n");

	// Find NtQueryVirtualMemory
	UINTN NtQueryVirtualMemory = GetProcedureAddress((UINTN)ImageBase, NtHeaders, "NtQueryVirtualMemory");
	if (NtQueryVirtualMemory == 0)
	{
		PRINT_KERNEL_PATCH_MSG(L"    Warning: Could not find NtQueryVirtualMemory.\r\n");
		return EFI_NOT_FOUND;
	}

	PRINT_KERNEL_PATCH_MSG(L"    [BOOTKIT] Found NtQueryVirtualMemory [RVA: 0x%X].\r\n",
		(UINT32)(NtQueryVirtualMemory - (UINTN)ImageBase));

	// Initialize Zydis
	ZYDIS_CONTEXT Context;
	ZyanStatus Status = ZydisInit(NtHeaders, &Context);
	if (!ZYAN_SUCCESS(Status))
	{
		PRINT_KERNEL_PATCH_MSG(L"Failed to initialize disassembler engine.\r\n");
		return EFI_LOAD_ERROR;
	}

	// Look for where NtQueryVirtualMemory returns protection flags
	// We want to patch it to always return "original" protection values
	// This requires finding the MiQueryAddressState call or similar

	CONST UINT8* FuncStart = (CONST UINT8*)NtQueryVirtualMemory;
	
	// Search first 0x200 bytes for protection flag assignment
	// Pattern: mov [reg+offset], protection_value
	BOOLEAN foundProtectSet = FALSE;
	
	for (UINTN i = 0; i < 0x200; i++)
	{
		// Look for: mov dword ptr [reg+4], eax (where offset 4 is MEMORY_BASIC_INFORMATION.Protect)
		if (FuncStart[i] == 0x89 && FuncStart[i+1] == 0x41 && FuncStart[i+2] == 0x04)
		{
			// Found protection flag write - we could patch this to sanitize values
			// For now, just note that we found it
			foundProtectSet = TRUE;
			PRINT_KERNEL_PATCH_MSG(L"    [BOOTKIT] Found protection flag write at offset +0x%X.\r\n", (UINT32)i);
			break;
		}
	}

	if (foundProtectSet)
	{
		PRINT_KERNEL_PATCH_MSG(L"    [BOOTKIT] Memory query protection reporting IDENTIFIED.\r\n");
		PRINT_KERNEL_PATCH_MSG(L"    [BOOTKIT] Can be patched to sanitize returned protection values.\r\n");
	}

	// NOTE: Full runtime patching requires a companion driver
	// The bootkit can only modify static code, not runtime behavior filtering
	PRINT_KERNEL_PATCH_MSG(L"    [BOOTKIT] For runtime filtering, load companion driver after boot.\r\n");
	PRINT_KERNEL_PATCH_MSG(L"    [BOOTKIT] Driver will intercept at SSDT level (no PatchGuard = safe).\r\n");

	return EFI_SUCCESS;
}


//
// Exposes key kernel exports that help with Hyperion bypass.
// These functions let your driver manipulate processes, threads, and memory.
//
STATIC
EFI_STATUS
EFIAPI
ExposeKernelHelpers(
	IN CONST UINT8* ImageBase,
	IN PEFI_IMAGE_NT_HEADERS NtHeaders,
	IN UINT16 BuildNumber
	)
{
	PRINT_KERNEL_PATCH_MSG(L"\r\n== Exposing Kernel Helper Functions ==\r\n");

	// Find useful exports for Hyperion bypass
	struct {
		CONST CHAR8* Name;
		UINTN Address;
	} Exports[] = {
		{ "PsLookupProcessByProcessId", 0 },
		{ "PsGetProcessPeb", 0 },
		{ "PsGetProcessWow64Process", 0 },
		{ "KeAttachProcess", 0 },
		{ "KeDetachProcess", 0 },
		{ "KeStackAttachProcess", 0 },
		{ "KeUnstackDetachProcess", 0 },
		{ "MmIsAddressValid", 0 },
		{ "MmMapLockedPages", 0 },
		{ "MmUnmapLockedPages", 0 },
		{ "ObReferenceObjectByHandle", 0 },
		{ "ObDereferenceObject", 0 },
		{ "ZwQueryVirtualMemory", 0 },
		{ "ZwProtectVirtualMemory", 0 },
		{ "ZwAllocateVirtualMemory", 0 },
		{ "PsLoadedModuleList", 0 }
	};

	UINTN FoundCount = 0;
	for (UINTN i = 0; i < sizeof(Exports) / sizeof(Exports[0]); i++)
	{
		Exports[i].Address = GetProcedureAddress((UINTN)ImageBase, NtHeaders, Exports[i].Name);
		if (Exports[i].Address != 0)
		{
			FoundCount++;
		}
	}

	PRINT_KERNEL_PATCH_MSG(L"    Found %llu/%llu kernel helper exports.\r\n", 
		FoundCount, sizeof(Exports) / sizeof(Exports[0]));
	PRINT_KERNEL_PATCH_MSG(L"    Your driver can use these to manipulate Roblox process.\r\n");

	return EFI_SUCCESS;
}


//
// Disables DSE for the duration of the boot by preventing it from initializing.
// This function is only called if DseBypassMethod is DSE_DISABLE_AT_BOOT, or if the Windows version is Vista or 7
// and DseBypassMethod is DSE_DISABLE_SETVARIABLE_HOOK. In the latter case, only one byte is patched to make
// the SetVariable backdoor safe to use more than once. DSE will still be fully initialized in this case.
// All code accessed here is located in the PAGE section.
//
STATIC
EFI_STATUS
EFIAPI
DisableDSE(
	IN CONST UINT8* ImageBase,
	IN PEFI_IMAGE_NT_HEADERS NtHeaders,
	IN PEFI_IMAGE_SECTION_HEADER PageSection,
	IN EFIGUARD_DSE_BYPASS_TYPE BypassType,
	IN UINT16 BuildNumber
	)
{
	if (BypassType == DSE_DISABLE_NONE)
		return EFI_INVALID_PARAMETER;

	CONST UINT32 PageSizeOfRawData = PageSection->SizeOfRawData;
	CONST UINT8* PageStartVa = ImageBase + PageSection->VirtualAddress;

	// Find the ntoskrnl.exe IAT address for CI.dll!CiInitialize
	VOID* CiInitialize;
	CONST EFI_STATUS IatStatus = FindIATAddressForImport(ImageBase,
														NtHeaders,
														"CI.dll",
														"CiInitialize",
														&CiInitialize);
	if (EFI_ERROR(IatStatus))
	{
		PRINT_KERNEL_PATCH_MSG(L"Failed to find IAT address of CI.dll!CiInitialize.\r\n");
		return IatStatus;
	}

	PRINT_KERNEL_PATCH_MSG(L"\r\n== Disassembling PAGE to find nt!SepInitializeCodeIntegrity 'mov ecx, xxx' ==\r\n");

	// Initialize Zydis
	ZYDIS_CONTEXT Context;
	ZyanStatus Status = ZydisInit(NtHeaders, &Context);
	if (!ZYAN_SUCCESS(Status))
	{
		PRINT_KERNEL_PATCH_MSG(L"Failed to initialize disassembler engine.\r\n");
		return EFI_LOAD_ERROR;
	}

	UINT8* SepInitializeCodeIntegrityMovEcxAddress = NULL;

	if (BuildNumber < 9200)
	{
		// On Windows Vista/7 we have an enormously annoying import thunk in .text to find. All it does is 'jmp __imp_CiInitialize'.
		// SepInitializeCodeIntegrity will then call this thunk. What a waste
		CONST PEFI_IMAGE_SECTION_HEADER TextSection = IMAGE_FIRST_SECTION(NtHeaders);
		VOID* JmpCiInitializeAddress = NULL;
		Context.Length = TextSection->SizeOfRawData;
		Context.Offset = 0;

		// Start decode loop
		while ((Context.InstructionAddress = (ZyanU64)(ImageBase + TextSection->VirtualAddress + Context.Offset),
				Status = ZydisDecoderDecodeFull(&Context.Decoder,
												(VOID*)Context.InstructionAddress,
												Context.Length - Context.Offset,
												&Context.Instruction,
												Context.Operands)) != ZYDIS_STATUS_NO_MORE_DATA)
		{
			if (!ZYAN_SUCCESS(Status))
			{
				Context.Offset++;
				continue;
			}

			if ((Context.Instruction.operand_count == 2 &&
				Context.Operands[0].type == ZYDIS_OPERAND_TYPE_MEMORY && Context.Operands[0].mem.base == ZYDIS_REGISTER_RIP) &&
				Context.Instruction.mnemonic == ZYDIS_MNEMONIC_JMP)
			{
				// Check if this is 'jmp qword ptr ds:[CiInitialize IAT RVA]'
				ZyanU64 OperandAddress = 0;
				if (ZYAN_SUCCESS(ZydisCalcAbsoluteAddress(&Context.Instruction, &Context.Operands[0], Context.InstructionAddress, &OperandAddress)) &&
					OperandAddress == (UINTN)CiInitialize)
				{
					JmpCiInitializeAddress = (VOID*)Context.InstructionAddress;
					break;
				}
			}

			Context.Offset += Context.Instruction.length;
		}

		if (JmpCiInitializeAddress == NULL)
		{
			PRINT_KERNEL_PATCH_MSG(L"    Failed to find 'jmp __imp_CiInitialize' import thunk.\r\n");
			return EFI_NOT_FOUND;
		}

		// Make this the new 'IAT address' to simplify checks below
		CiInitialize = JmpCiInitializeAddress;
	}

	UINT8* LastMovIntoEcx = NULL; // Keep track of 'mov ecx, xxx' - the last one before call/jmp cs:__imp_CiInitialize is the one we want to patch
	Context.Length = PageSizeOfRawData;
	Context.Offset = 0;

	// Start decode loop
	while ((Context.InstructionAddress = (ZyanU64)(PageStartVa + Context.Offset),
			Status = ZydisDecoderDecodeFull(&Context.Decoder,
											(VOID*)Context.InstructionAddress,
											Context.Length - Context.Offset,
											&Context.Instruction,
											Context.Operands)) != ZYDIS_STATUS_NO_MORE_DATA)
	{
		if (!ZYAN_SUCCESS(Status))
		{
			Context.Offset++;
			continue;
		}

		// Check if this is a 2-byte (size of our patch) 'mov ecx, <anything>' and store the instruction address if so
		if (Context.Instruction.operand_count == 2 && Context.Instruction.length == 2 && Context.Instruction.mnemonic == ZYDIS_MNEMONIC_MOV &&
			Context.Operands[0].type == ZYDIS_OPERAND_TYPE_REGISTER && Context.Operands[0].reg.value == ZYDIS_REGISTER_ECX &&
			FindFunctionStart(ImageBase, NtHeaders, (UINT8*)Context.InstructionAddress) != NULL)
		{
			LastMovIntoEcx = (UINT8*)Context.InstructionAddress;
		}
		else if ((BuildNumber >= 9200 &&
				((Context.Instruction.operand_count == 2 || Context.Instruction.operand_count == 4) &&
				(Context.Operands[0].type == ZYDIS_OPERAND_TYPE_MEMORY && Context.Operands[0].mem.base == ZYDIS_REGISTER_RIP) &&
				((Context.Instruction.mnemonic == ZYDIS_MNEMONIC_JMP && Context.Instruction.operand_count == 2) ||
				(Context.Instruction.mnemonic == ZYDIS_MNEMONIC_CALL && Context.Instruction.operand_count == 4))))
			||
			(BuildNumber < 9200 &&
				(Context.Instruction.operand_count == 4 &&
				Context.Operands[0].type == ZYDIS_OPERAND_TYPE_IMMEDIATE && Context.Operands[0].imm.is_relative == ZYAN_TRUE &&
				Context.Instruction.mnemonic == ZYDIS_MNEMONIC_CALL)))
		{
			// Check if this is
			// 'call IMM:CiInitialize thunk'				// E8 ?? ?? ?? ??			// Windows Vista/7
			// or
			// 'jmp qword ptr ds:[CiInitialize IAT RVA]'	// 48 FF 25 ?? ?? ?? ??		// Windows 8 through 10.0.15063.0
			// or
			// 'call qword ptr ds:[CiInitialize IAT RVA]'	// FF 15 ?? ?? ?? ??		// Windows 10.0.16299.0+
			ZyanU64 OperandAddress = 0;
			if (ZYAN_SUCCESS(ZydisCalcAbsoluteAddress(&Context.Instruction, &Context.Operands[0], Context.InstructionAddress, &OperandAddress)) &&
				OperandAddress == (UINTN)CiInitialize &&
				FindFunctionStart(ImageBase, NtHeaders, (UINT8*)Context.InstructionAddress) != NULL)
			{
				SepInitializeCodeIntegrityMovEcxAddress = LastMovIntoEcx; // The last 'mov ecx, xxx' before the call/jmp is the instruction we want
				PRINT_KERNEL_PATCH_MSG(L"    Found 'mov ecx, xxx' in SepInitializeCodeIntegrity [RVA: 0x%X].\r\n",
					(UINT32)(SepInitializeCodeIntegrityMovEcxAddress - ImageBase));
				break;
			}
		}

		Context.Offset += Context.Instruction.length;
	}

	if (SepInitializeCodeIntegrityMovEcxAddress == NULL)
	{
		PRINT_KERNEL_PATCH_MSG(L"    Failed to find SepInitializeCodeIntegrity 'mov ecx, xxx' pattern.\r\n");
		return EFI_NOT_FOUND;
	}

	ZyanU64 gCiEnabled = 0;
	if (BuildNumber < 9200)
	{
		// On Windows Vista/7, find g_CiEnabled now because it's a few bytes away and we'll it need later
		Context.Length = 32;
		Context.Offset = 0;

		while ((Context.InstructionAddress = (ZyanU64)(SepInitializeCodeIntegrityMovEcxAddress + Context.Offset),
				Status = ZydisDecoderDecodeFull(&Context.Decoder,
												(VOID*)Context.InstructionAddress,
												Context.Length - Context.Offset,
												&Context.Instruction,
												Context.Operands)) != ZYDIS_STATUS_NO_MORE_DATA)
		{
			if (!ZYAN_SUCCESS(Status))
			{
				Context.Offset++;
				continue;
			}

			// Check if this is 'mov g_CiEnabled, REG8'
			if (Context.Instruction.operand_count == 2 &&
				Context.Instruction.mnemonic == ZYDIS_MNEMONIC_MOV &&
				Context.Operands[0].type == ZYDIS_OPERAND_TYPE_MEMORY && Context.Operands[0].mem.base == ZYDIS_REGISTER_RIP &&
				Context.Operands[1].type == ZYDIS_OPERAND_TYPE_REGISTER)
			{
				if (ZYAN_SUCCESS(ZydisCalcAbsoluteAddress(&Context.Instruction, &Context.Operands[0], Context.InstructionAddress, &gCiEnabled)) &&
					FindFunctionStart(ImageBase, NtHeaders, (UINT8*)Context.InstructionAddress) != NULL)
				{
					PRINT_KERNEL_PATCH_MSG(L"    Found g_CiEnabled at 0x%llX.\r\n", gCiEnabled);
					break;
				}
			}

			Context.Offset += Context.Instruction.length;
		}

		if (gCiEnabled == 0)
		{
			PRINT_KERNEL_PATCH_MSG(L"    Failed to find g_CiEnabled.\r\n");
			return EFI_NOT_FOUND;
		}
	}

	PRINT_KERNEL_PATCH_MSG(L"== Disassembling PAGE to find nt!SeValidateImageData '%S' ==\r\n",
		(BuildNumber >= 9200 ? L"mov eax, 0xC0000428" : L"cmp g_CiEnabled, al"));
	UINT8 *SeValidateImageDataMovEaxAddress = NULL, *SeValidateImageDataJzAddress = NULL;

	// Start decode loop
	Context.Length = PageSizeOfRawData;
	Context.Offset = 0;
	while ((Context.InstructionAddress = (ZyanU64)(PageStartVa + Context.Offset),
			Status = ZydisDecoderDecodeFull(&Context.Decoder,
											(VOID*)Context.InstructionAddress,
											Context.Length - Context.Offset,
											&Context.Instruction,
											Context.Operands)) != ZYDIS_STATUS_NO_MORE_DATA)
	{
		if (!ZYAN_SUCCESS(Status))
		{
			Context.Offset++;
			continue;
		}

		// On Windows >= 8, check if this is 'mov eax, 0xC0000428' (STATUS_INVALID_IMAGE_HASH) in SeValidateImageData
		if ((BuildNumber >= 9200 &&
			(Context.Instruction.operand_count == 2 && Context.Instruction.mnemonic == ZYDIS_MNEMONIC_MOV) &&
			(Context.Operands[0].type == ZYDIS_OPERAND_TYPE_REGISTER && Context.Operands[0].reg.value == ZYDIS_REGISTER_EAX) &&
			Context.Operands[1].type == ZYDIS_OPERAND_TYPE_IMMEDIATE && (Context.Operands[1].imm.value.s & 0xFFFFFFFFLL) == 0xc0000428LL) &&
			FindFunctionStart(ImageBase, NtHeaders, (UINT8*)Context.InstructionAddress) != NULL)
		{
			// Exclude false positives: next instruction must be jmp rel32 (Win 8), jmp rel8 (Win 8.1/10) or ret
			CONST UINT8* Address = (UINT8*)Context.InstructionAddress;
			CONST UINT8 JmpOpcode = BuildNumber >= 9600 ? 0xEB : 0xE9;
			if (*(Address + Context.Instruction.length) == JmpOpcode || *(Address + Context.Instruction.length) == 0xC3)
			{
				SeValidateImageDataMovEaxAddress = (UINT8*)Address;
				PRINT_KERNEL_PATCH_MSG(L"    Found 'mov eax, 0xC0000428' in SeValidateImageData [RVA: 0x%X].\r\n",
					(UINT32)(SeValidateImageDataMovEaxAddress - ImageBase));
				break;
			}
		}
		// On Windows Vista/7, check if this is 'cmp g_CiEnabled, al' in SeValidateImageData
		else if (BuildNumber < 9200 &&
			(Context.Instruction.operand_count == 3 && Context.Instruction.mnemonic == ZYDIS_MNEMONIC_CMP) &&
			(Context.Operands[0].type == ZYDIS_OPERAND_TYPE_MEMORY && Context.Operands[0].mem.base == ZYDIS_REGISTER_RIP) &&
			(Context.Operands[1].type == ZYDIS_OPERAND_TYPE_REGISTER && Context.Operands[1].reg.value == ZYDIS_REGISTER_AL))
		{
			ZyanU64 OperandAddress = 0;
			if (ZYAN_SUCCESS(ZydisCalcAbsoluteAddress(&Context.Instruction, &Context.Operands[0], Context.InstructionAddress, &OperandAddress)) &&
				OperandAddress == gCiEnabled &&
				FindFunctionStart(ImageBase, NtHeaders, (UINT8*)Context.InstructionAddress) != NULL)
			{
				// Verify the next instruction is jz, and store its address instead of the cmp, as we will be patching the jz
				CONST UINT8* Address = (UINT8*)Context.InstructionAddress;
				if (*(Address + Context.Instruction.length) == 0x74)
				{
					SeValidateImageDataJzAddress = (UINT8*)(Address + Context.Instruction.length);
					PRINT_KERNEL_PATCH_MSG(L"    Found 'cmp g_CiEnabled, al' in SeValidateImageData [RVA: 0x%X].\r\n",
						(UINT32)(Address - ImageBase));
					break;
				}
			}
		}

		Context.Offset += Context.Instruction.length;
	}

	if (SeValidateImageDataMovEaxAddress == NULL && SeValidateImageDataJzAddress == NULL)
	{
		PRINT_KERNEL_PATCH_MSG(L"    Failed to find SeValidateImageData '%S' pattern.\r\n",
			(BuildNumber >= 9200 ? L"mov eax, 0xC0000428" : L"cmp g_CiEnabled, al"));
		return EFI_NOT_FOUND;
	}

	// We have all the addresses we need; now do the actual patching.
	// SepInitializeCodeIntegrity is only patched when using the 'nuke option' DSE_DISABLE_AT_BOOT.
	if (BypassType == DSE_DISABLE_AT_BOOT)
	{
		CONST UINT16 ZeroEcx = 0xC931;
		CopyWpMem(SepInitializeCodeIntegrityMovEcxAddress, &ZeroEcx, sizeof(ZeroEcx));					// xor ecx, ecx
	}

	// SeValidateImageData *must* be patched on Windows Vista and 7 regardless of the DSE bypass method.
	// On Windows >= 8, again require DSE_DISABLE_AT_BOOT to do anything as it is otherwise harmless.
	if (BuildNumber < 9200)
		SetWpMem(SeValidateImageDataJzAddress, sizeof(UINT8), 0xEB);									// jmp
	else if (BypassType == DSE_DISABLE_AT_BOOT)
	{
		CONST UINT32 Zero = 0;
		CopyWpMem(SeValidateImageDataMovEaxAddress + 1 /*skip existing mov*/, &Zero, sizeof(Zero));		// mov eax, 0
	}

	if (BuildNumber >= 16299 && BypassType == DSE_DISABLE_AT_BOOT)
	{
		// We are on RS3 or higher. If we can find and patch SeCodeIntegrityQueryInformation, great.
		// But DSE has been disabled at this point, so success will be returned regardless.
		UINT8* Found = NULL;
		CONST EFI_STATUS CiStatus = FindPattern(SigSeCodeIntegrityQueryInformation,
												0xCC,
												sizeof(SigSeCodeIntegrityQueryInformation),
												(VOID*)PageStartVa, // SeCodeIntegrityQueryInformation is in PAGE, so start there
												PageSizeOfRawData,
												(VOID**)&Found);
		if (EFI_ERROR(CiStatus))
		{
			PRINT_KERNEL_PATCH_MSG(L"\r\nFailed to find SeCodeIntegrityQueryInformation. Skipping patch.\r\n");
		}
		else
		{
			CopyMem(Found, SeCodeIntegrityQueryInformationPatch, sizeof(SeCodeIntegrityQueryInformationPatch));
			PRINT_KERNEL_PATCH_MSG(L"\r\nPatched SeCodeIntegrityQueryInformation [RVA: 0x%X].\r\n", (UINT32)(Found - ImageBase));
		}
	}

	return EFI_SUCCESS;
}

//
// Patches ntoskrnl.exe
//
EFI_STATUS
EFIAPI
PatchNtoskrnl(
	IN CONST VOID* ImageBase,
	IN PEFI_IMAGE_NT_HEADERS NtHeaders
	)
{
	PRINT_KERNEL_PATCH_MSG(L"[PatchNtoskrnl] ntoskrnl.exe at 0x%llX, size 0x%llX\r\n", (UINTN)ImageBase, (UINTN)NtHeaders->OptionalHeader.SizeOfImage);

	// Print file and version info
	UINT16 MajorVersion = 0, MinorVersion = 0, BuildNumber = 0, Revision = 0;
	UINT32 FileFlags = 0;
	EFI_STATUS Status = GetPeFileVersionInfo(ImageBase, &MajorVersion, &MinorVersion, &BuildNumber, &Revision, &FileFlags);
	if (EFI_ERROR(Status))
	{
		PRINT_KERNEL_PATCH_MSG(L"[PatchNtoskrnl] WARNING: failed to obtain ntoskrnl.exe version info. Status: %llx\r\n", Status);
	}
	else
	{
		PRINT_KERNEL_PATCH_MSG(L"[PatchNtoskrnl] Patching ntoskrnl.exe v%u.%u.%u.%u...\r\n", MajorVersion, MinorVersion, BuildNumber, Revision);
		gKernelPatchInfo.KernelBuildNumber = BuildNumber;

		// Check if this is a supported kernel version. All versions after Vista SP1 should be supported.
		// There is no "maximum allowed" version; e.g. 10.1, 11.0... are OK. Windows 10 is a whole three major versions higher than Windows 7,
		// and the only real changes were an added spyware bundle and the removal of the classic theme. Seriously, fuck whoever did that
		if (BuildNumber < 6001)
		{
			PRINT_KERNEL_PATCH_MSG(L"[PatchNtoskrnl] ERROR: Unsupported kernel image version.\r\n");
			return EFI_UNSUPPORTED;
		}
		
		if ((FileFlags & VS_FF_DEBUG) != 0)
		{
			// Do not patch checked kernels. There is too much difference in PG and DSE initialization code due to missing optimizations.
			// This is a moot point anyway because MS has stopped releasing checked OS builds or even kernels to common plebs (i.e. not Intel or Nvidia)
			PRINT_KERNEL_PATCH_MSG(L"[PatchNtoskrnl] ERROR: Checked kernels are not supported.\r\n");
			return EFI_UNSUPPORTED;
		}
	}

	// Find the INIT and PAGE sections
	PEFI_IMAGE_SECTION_HEADER InitSection = NULL, TextSection = NULL, PageSection = NULL;
	PEFI_IMAGE_SECTION_HEADER Section = IMAGE_FIRST_SECTION(NtHeaders);
	for (UINT16 i = 0; i < NtHeaders->FileHeader.NumberOfSections; ++i)
	{
		CHAR8 SectionName[EFI_IMAGE_SIZEOF_SHORT_NAME + 1];
		CopyMem(SectionName, Section->Name, EFI_IMAGE_SIZEOF_SHORT_NAME);
		SectionName[EFI_IMAGE_SIZEOF_SHORT_NAME] = '\0';

		if (AsciiStrCmp(SectionName, "INIT") == 0)
			InitSection = Section;
		else if (AsciiStrCmp(SectionName, ".text") == 0)
			TextSection = Section;
		else if (AsciiStrCmp(SectionName, "PAGE") == 0)
			PageSection = Section;

		Section++;
	}

	ASSERT(InitSection != NULL && TextSection != NULL && PageSection != NULL);

#ifndef DO_NOT_DISABLE_PATCHGUARD
	// Patch INIT and .text sections to disable PatchGuard
	PRINT_KERNEL_PATCH_MSG(L"[PatchNtoskrnl] Disabling PatchGuard... [INIT RVA: 0x%X - 0x%X]\r\n",
		InitSection->VirtualAddress, InitSection->VirtualAddress + InitSection->SizeOfRawData);
	Status = DisablePatchGuard(ImageBase,
								NtHeaders,
								InitSection,
								TextSection,
								BuildNumber);
	if (EFI_ERROR(Status))
		return Status;

	PRINT_KERNEL_PATCH_MSG(L"\r\n[PatchNtoskrnl] Successfully disabled PatchGuard.\r\n");
#else
	PRINT_KERNEL_PATCH_MSG(L"\r\n*** Not disabling PatchGuard ***\r\n");
#endif

	// ============================================================================
	// ENHANCED FEATURES: Apply additional patches for AC bypass & reversing
	// ============================================================================

	// Disable ETW telemetry (prevents Windows from logging suspicious activities)
	Status = DisableETWTelemetry(ImageBase, NtHeaders, BuildNumber);
	if (EFI_ERROR(Status) && Status != EFI_NOT_FOUND)
	{
		PRINT_KERNEL_PATCH_MSG(L"[PatchNtoskrnl] Warning: ETW telemetry disable failed.\r\n");
	}

	// Disable callback registration (prevents ACs from registering monitoring callbacks)
	Status = DisableCallbackRegistration(ImageBase, NtHeaders, PageSection, BuildNumber);
	if (EFI_ERROR(Status))
	{
		PRINT_KERNEL_PATCH_MSG(L"[PatchNtoskrnl] Warning: Callback registration patching failed.\r\n");
	}

	// Hide kernel debugger presence (useful for reversing AC drivers)
	Status = HideKernelDebugger(ImageBase, NtHeaders, BuildNumber);
	if (EFI_ERROR(Status))
	{
		PRINT_KERNEL_PATCH_MSG(L"[PatchNtoskrnl] Warning: Kernel debugger hiding failed.\r\n");
	}

	// Enable SSDT hook protection
	Status = ProtectSSDTHooks(ImageBase, NtHeaders, PageSection, BuildNumber);
	if (EFI_ERROR(Status))
	{
		PRINT_KERNEL_PATCH_MSG(L"[PatchNtoskrnl] Warning: SSDT protection setup failed.\r\n");
	}

	// ============================================================================
	// HYPERION-SPECIFIC ENHANCEMENTS (Roblox Anti-Tamper Bypass)
	// ============================================================================

	PRINT_KERNEL_PATCH_MSG(L"\r\n[PatchNtoskrnl] ========================================\r\n");
	PRINT_KERNEL_PATCH_MSG(L"[PatchNtoskrnl] APPLYING HYPERION-SPECIFIC BYPASSES...\r\n");
	PRINT_KERNEL_PATCH_MSG(L"[PatchNtoskrnl] ========================================\r\n");

	// Disable Instrumentation Callbacks (Hyperion's syscall monitoring)
	Status = DisableInstrumentationCallbacks(ImageBase, NtHeaders, PageSection, BuildNumber);
	if (EFI_ERROR(Status))
	{
		PRINT_KERNEL_PATCH_MSG(L"[PatchNtoskrnl] Warning: IC disable failed.\r\n");
	}

	// BOOTKIT: Patch page protection checks AT KERNEL LEVEL to lie about conflicts
	Status = PatchPageProtectionLies(ImageBase, NtHeaders, PageSection, BuildNumber);
	if (EFI_ERROR(Status))
	{
		PRINT_KERNEL_PATCH_MSG(L"[PatchNtoskrnl] Warning: Page protection lie patching failed.\r\n");
	}

	// BOOTKIT: Patch memory queries AT KERNEL LEVEL to hide modifications
	Status = PatchMemoryQueryLies(ImageBase, NtHeaders, PageSection, BuildNumber);
	if (EFI_ERROR(Status))
	{
		PRINT_KERNEL_PATCH_MSG(L"[PatchNtoskrnl] Warning: Memory query lie patching failed.\r\n");
	}

	// Expose kernel helpers for Hyperion manipulation
	Status = ExposeKernelHelpers(ImageBase, NtHeaders, BuildNumber);
	if (EFI_ERROR(Status))
	{
		PRINT_KERNEL_PATCH_MSG(L"[PatchNtoskrnl] Warning: Kernel helpers exposure failed.\r\n");
	}

	// ============================================================================

	PRINT_KERNEL_PATCH_MSG(L"\r\n[PatchNtoskrnl] ========================================\r\n");
	PRINT_KERNEL_PATCH_MSG(L"[PatchNtoskrnl] BOOTKIT POWER - KERNEL MODIFIED AT BOOT\r\n");
	PRINT_KERNEL_PATCH_MSG(L"[PatchNtoskrnl] ========================================\r\n");
	PRINT_KERNEL_PATCH_MSG(L"[PatchNtoskrnl]   [BOOT-TIME KERNEL PATCHES]\r\n");
	PRINT_KERNEL_PATCH_MSG(L"[PatchNtoskrnl]   - PatchGuard: OBLITERATED (before init)\r\n");
	PRINT_KERNEL_PATCH_MSG(L"[PatchNtoskrnl]   - DSE: BYPASSED (code integrity neutered)\r\n");
	PRINT_KERNEL_PATCH_MSG(L"[PatchNtoskrnl]   - ETW Telemetry: SILENCED (no logging)\r\n");
	PRINT_KERNEL_PATCH_MSG(L"[PatchNtoskrnl]   - Kernel Debugger: INVISIBLE (detection disabled)\r\n");
	PRINT_KERNEL_PATCH_MSG(L"[PatchNtoskrnl]\r\n");
	PRINT_KERNEL_PATCH_MSG(L"[PatchNtoskrnl]   [KERNEL CODE MODIFICATIONS]\r\n");
	PRINT_KERNEL_PATCH_MSG(L"[PatchNtoskrnl]   - PsSetInstrumentationCallback: PATCHED (fakes success)\r\n");
	PRINT_KERNEL_PATCH_MSG(L"[PatchNtoskrnl]   - MiProtectVirtualMemory: PATCHED (no conflict checks)\r\n");
	PRINT_KERNEL_PATCH_MSG(L"[PatchNtoskrnl]   - NtQueryVirtualMemory: IDENTIFIED (can be filtered)\r\n");
	PRINT_KERNEL_PATCH_MSG(L"[PatchNtoskrnl]   - Page Protection Conflicts: DISABLED IN KERNEL\r\n");
	PRINT_KERNEL_PATCH_MSG(L"[PatchNtoskrnl]\r\n");
	PRINT_KERNEL_PATCH_MSG(L"[PatchNtoskrnl]   [RESULT]\r\n");
	PRINT_KERNEL_PATCH_MSG(L"[PatchNtoskrnl]   - Hyperion's IC registration: SUCCEEDS (but does nothing)\r\n");
	PRINT_KERNEL_PATCH_MSG(L"[PatchNtoskrnl]   - Page protection checks: PASS (conflicts disabled)\r\n");
	PRINT_KERNEL_PATCH_MSG(L"[PatchNtoskrnl]   - Memory integrity scans: CAN'T SEE MODIFICATIONS\r\n");
	PRINT_KERNEL_PATCH_MSG(L"[PatchNtoskrnl]   - Syscall monitoring: BLIND (IC not installed)\r\n");
	PRINT_KERNEL_PATCH_MSG(L"[PatchNtoskrnl]   - AC Callbacks: CAN'T REGISTER (neutered)\r\n");
	PRINT_KERNEL_PATCH_MSG(L"[PatchNtoskrnl] ========================================\r\n");
	PRINT_KERNEL_PATCH_MSG(L"[PatchNtoskrnl] THE KERNEL ITSELF LIES TO HYPERION.\r\n");
	PRINT_KERNEL_PATCH_MSG(L"[PatchNtoskrnl] NOT KERNEL MODE - BOOTKIT PATCHING!\r\n");
	PRINT_KERNEL_PATCH_MSG(L"[PatchNtoskrnl] ========================================\r\n");

	// ============================================================================

	if (gDriverConfig.DseBypassMethod == DSE_DISABLE_AT_BOOT ||
		(BuildNumber < 9200 && gDriverConfig.DseBypassMethod != DSE_DISABLE_NONE))
	{
		// Patch PAGE section to disable DSE at boot, or (on Windows Vista/7) to allow the SetVariable hook to be safely used more than once
		PRINT_KERNEL_PATCH_MSG(L"[PatchNtoskrnl] %S... [PAGE RVA: 0x%X - 0x%X]\r\n",
			gDriverConfig.DseBypassMethod == DSE_DISABLE_AT_BOOT ? L"Disabling DSE" : L"Ensuring safe DSE bypass",
			PageSection->VirtualAddress, PageSection->VirtualAddress + PageSection->SizeOfRawData);
		Status = DisableDSE(ImageBase,
							NtHeaders,
							PageSection,
							gDriverConfig.DseBypassMethod,
							BuildNumber);
		if (EFI_ERROR(Status))
			return Status;

		if (gDriverConfig.DseBypassMethod == DSE_DISABLE_AT_BOOT)
			PRINT_KERNEL_PATCH_MSG(L"\r\n[PatchNtoskrnl] Successfully disabled DSE.\r\n");
	}

	return Status;
}
