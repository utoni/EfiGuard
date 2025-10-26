# HYPERION KERNEL DRIVER - IMPLEMENTATION GUIDE

## Overview

This driver leverages the EfiGuard bootkit's kernel modifications to completely dominate Hyperion without detection.

---

## Architecture

```
┌──────────────────────────────────────────────────┐
│              BOOT SEQUENCE                        │
├──────────────────────────────────────────────────┤
│  1. UEFI Firmware loads EfiGuard                 │
│  2. EfiGuard patches ntoskrnl.exe:               │
│     - Disables PatchGuard                        │
│     - Neuters IC registration (fakes success)    │
│     - Exposes kernel APIs                        │
│  3. Windows boots with compromised kernel        │
│  4. Your driver loads (NO PatchGuard to stop it) │
│  5. Driver hooks SSDT                            │
│  6. Hyperion loads → walks into your trap        │
└──────────────────────────────────────────────────┘
```

---

## Driver Components

### 1. SSDT Hook Engine

```c
#pragma once
#include <ntddk.h>

// SSDT structure (undocumented)
typedef struct _SYSTEM_SERVICE_TABLE {
    PVOID ServiceTableBase;
    PVOID ServiceCounterTable;
    ULONGLONG NumberOfServices;
    PVOID ParamTableBase;
} SYSTEM_SERVICE_TABLE, *PSYSTEM_SERVICE_TABLE;

extern PSYSTEM_SERVICE_TABLE KeServiceDescriptorTable;

// Original function pointers
typedef NTSTATUS (*pNtProtectVirtualMemory)(
    HANDLE ProcessHandle,
    PVOID *BaseAddress,
    PSIZE_T NumberOfBytesToProtect,
    ULONG NewAccessProtection,
    PULONG OldAccessProtection
);

typedef NTSTATUS (*pNtQueryVirtualMemory)(
    HANDLE ProcessHandle,
    PVOID BaseAddress,
    MEMORY_INFORMATION_CLASS MemoryInformationClass,
    PVOID MemoryInformation,
    SIZE_T MemoryInformationLength,
    PSIZE_T ReturnLength
);

pNtProtectVirtualMemory Original_NtProtectVirtualMemory = NULL;
pNtQueryVirtualMemory Original_NtQueryVirtualMemory = NULL;

// Disable write protection
VOID DisableWP() {
    __writecr0(__readcr0() & (~0x10000));
}

// Enable write protection
VOID EnableWP() {
    __writecr0(__readcr0() | 0x10000);
}

// Get SSDT function address
PVOID GetSSDTFunctionAddress(ULONG index) {
    PULONG serviceTable = (PULONG)KeServiceDescriptorTable->ServiceTableBase;
    ULONG offset = serviceTable[index] >> 4;
    return (PVOID)((ULONG_PTR)serviceTable + offset);
}

// Hook SSDT entry
BOOLEAN HookSSDT(ULONG index, PVOID hookFunction, PVOID *originalFunction) {
    DisableWP();
    
    *originalFunction = GetSSDTFunctionAddress(index);
    
    PULONG serviceTable = (PULONG)KeServiceDescriptorTable->ServiceTableBase;
    LONG offset = (LONG)((ULONG_PTR)hookFunction - (ULONG_PTR)serviceTable);
    serviceTable[index] = (offset << 4) | (serviceTable[index] & 0xF);
    
    EnableWP();
    return TRUE;
}
```

---

### 2. Hyperion Detection

```c
// Process tracking
HANDLE g_HyperionPID = NULL;
PEPROCESS g_HyperionProcess = NULL;
PVOID g_HyperionBase = NULL;

// Check if caller is Hyperion
BOOLEAN IsCallerHyperion() {
    // Method 1: Check calling process
    PEPROCESS currentProcess = PsGetCurrentProcess();
    if (currentProcess == g_HyperionProcess) {
        return TRUE;
    }
    
    // Method 2: Check return address
    PVOID returnAddress = _ReturnAddress();
    if (returnAddress >= g_HyperionBase && 
        returnAddress < (PVOID)((ULONG_PTR)g_HyperionBase + 0x1000000)) {
        return TRUE;
    }
    
    return FALSE;
}

// Check if address is Hyperion memory
BOOLEAN IsHyperionMemory(PVOID address) {
    if (!g_HyperionBase) return FALSE;
    
    return (address >= g_HyperionBase && 
            address < (PVOID)((ULONG_PTR)g_HyperionBase + 0x1000000));
}

// Process creation callback
VOID ProcessCallback(
    PEPROCESS Process,
    HANDLE ProcessId,
    PPS_CREATE_NOTIFY_INFO CreateInfo
) {
    if (CreateInfo) {
        // Check if this is Roblox process
        if (wcsstr(CreateInfo->ImageFileName->Buffer, L"RobloxPlayer")) {
            DbgPrint("[Bootkit] Roblox detected: PID %llu\n", (ULONGLONG)ProcessId);
            
            // Wait for Hyperion to load
            KeDelayExecutionThread(KernelMode, FALSE, &delay);
            
            // Find Hyperion module
            g_HyperionBase = FindModuleInProcess(Process, L"Hyperion.dll");
            if (g_HyperionBase) {
                g_HyperionProcess = Process;
                g_HyperionPID = ProcessId;
                ObReferenceObject(Process);
                
                DbgPrint("[Bootkit] Hyperion loaded at: %p\n", g_HyperionBase);
                
                // Start manipulation
                InitiateHyperionCompromise();
            }
        }
    }
}
```

---

### 3. Memory Protection Hook

```c
// Hooked NtProtectVirtualMemory
NTSTATUS Hook_NtProtectVirtualMemory(
    HANDLE ProcessHandle,
    PVOID *BaseAddress,
    PSIZE_T NumberOfBytesToProtect,
    ULONG NewAccessProtection,
    PULONG OldAccessProtection
) {
    // Call original first
    NTSTATUS status = Original_NtProtectVirtualMemory(
        ProcessHandle,
        BaseAddress,
        NumberOfBytesToProtect,
        NewAccessProtection,
        OldAccessProtection
    );
    
    // If Hyperion is checking its own memory
    if (IsCallerHyperion() && IsHyperionMemory(*BaseAddress)) {
        DbgPrint("[Bootkit] Hyperion checking page protection at %p - LYING\n", *BaseAddress);
        
        // DECEPTION: Report "no conflicts" even if we modified the page
        // Make it look like protection changed successfully
        *OldAccessProtection = NewAccessProtection;
        
        // Always return success
        return STATUS_SUCCESS;
    }
    
    return status;
}
```

---

### 4. Memory Query Hook

```c
// Track our modifications
typedef struct _MODIFIED_PAGE {
    PVOID Address;
    ULONG OriginalProtection;
    LIST_ENTRY ListEntry;
} MODIFIED_PAGE, *PMODIFIED_PAGE;

LIST_ENTRY g_ModifiedPages;
KSPIN_LOCK g_ModifiedPagesLock;

// Hooked NtQueryVirtualMemory
NTSTATUS Hook_NtQueryVirtualMemory(
    HANDLE ProcessHandle,
    PVOID BaseAddress,
    MEMORY_INFORMATION_CLASS MemoryInformationClass,
    PVOID MemoryInformation,
    SIZE_T MemoryInformationLength,
    PSIZE_T ReturnLength
) {
    NTSTATUS status = Original_NtQueryVirtualMemory(
        ProcessHandle,
        BaseAddress,
        MemoryInformationClass,
        MemoryInformation,
        MemoryInformationLength,
        ReturnLength
    );
    
    if (!NT_SUCCESS(status) || !IsCallerHyperion()) {
        return status;
    }
    
    // Hyperion is scanning memory
    if (MemoryInformationClass == MemoryBasicInformation) {
        PMEMORY_BASIC_INFORMATION mbi = (PMEMORY_BASIC_INFORMATION)MemoryInformation;
        
        // If this is a page we modified, lie about its protection
        KIRQL oldIrql;
        KeAcquireSpinLock(&g_ModifiedPagesLock, &oldIrql);
        
        PLIST_ENTRY entry = g_ModifiedPages.Flink;
        while (entry != &g_ModifiedPages) {
            PMODIFIED_PAGE modPage = CONTAINING_RECORD(entry, MODIFIED_PAGE, ListEntry);
            
            if (modPage->Address == BaseAddress) {
                DbgPrint("[Bootkit] Hyperion querying modified page %p - LYING\n", BaseAddress);
                
                // Report original protection, not current
                mbi->Protect = modPage->OriginalProtection;
                break;
            }
            
            entry = entry->Flink;
        }
        
        KeReleaseSpinLock(&g_ModifiedPagesLock, oldIrql);
    }
    
    return status;
}
```

---

### 5. Direct Memory Manipulation

```c
// Read Hyperion memory (bypass all protections)
NTSTATUS ReadHyperionMemory(PVOID address, PVOID buffer, SIZE_T size) {
    if (!g_HyperionProcess) return STATUS_NOT_FOUND;
    
    KAPC_STATE apc;
    KeStackAttachProcess(g_HyperionProcess, &apc);
    
    __try {
        // Probe the address is valid
        if (MmIsAddressValid(address)) {
            // Direct copy - page protections IRRELEVANT in kernel mode
            RtlCopyMemory(buffer, address, size);
        } else {
            KeUnstackDetachProcess(&apc);
            return STATUS_ACCESS_VIOLATION;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        KeUnstackDetachProcess(&apc);
        return STATUS_ACCESS_VIOLATION;
    }
    
    KeUnstackDetachProcess(&apc);
    return STATUS_SUCCESS;
}

// Write Hyperion memory (bypass all protections)
NTSTATUS WriteHyperionMemory(PVOID address, PVOID buffer, SIZE_T size) {
    if (!g_HyperionProcess) return STATUS_NOT_FOUND;
    
    KAPC_STATE apc;
    KeStackAttachProcess(g_HyperionProcess, &apc);
    
    __try {
        if (MmIsAddressValid(address)) {
            // Track this modification
            TrackModifiedPage(address);
            
            // Direct write
            RtlCopyMemory(address, buffer, size);
        } else {
            KeUnstackDetachProcess(&apc);
            return STATUS_ACCESS_VIOLATION;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        KeUnstackDetachProcess(&apc);
        return STATUS_ACCESS_VIOLATION;
    }
    
    KeUnstackDetachProcess(&apc);
    return STATUS_SUCCESS;
}

// Track modified page for later deception
VOID TrackModifiedPage(PVOID address) {
    PVOID pageBase = (PVOID)((ULONG_PTR)address & ~0xFFF);
    
    // Get current protection
    MEMORY_BASIC_INFORMATION mbi;
    SIZE_T returnLength;
    ZwQueryVirtualMemory(
        ZwCurrentProcess(),
        pageBase,
        MemoryBasicInformation,
        &mbi,
        sizeof(mbi),
        &returnLength
    );
    
    // Allocate tracking structure
    PMODIFIED_PAGE modPage = (PMODIFIED_PAGE)ExAllocatePoolWithTag(
        NonPagedPool,
        sizeof(MODIFIED_PAGE),
        'domM'
    );
    
    if (modPage) {
        modPage->Address = pageBase;
        modPage->OriginalProtection = mbi.Protect;
        
        KIRQL oldIrql;
        KeAcquireSpinLock(&g_ModifiedPagesLock, &oldIrql);
        InsertTailList(&g_ModifiedPages, &modPage->ListEntry);
        KeReleaseSpinLock(&g_ModifiedPagesLock, oldIrql);
        
        DbgPrint("[Bootkit] Tracking modified page: %p (original: 0x%X)\n", 
                 pageBase, mbi.Protect);
    }
}
```

---

### 6. Hyperion Compromise Routine

```c
VOID InitiateHyperionCompromise() {
    DbgPrint("[Bootkit] ========================================\n");
    DbgPrint("[Bootkit] INITIATING HYPERION COMPROMISE\n");
    DbgPrint("[Bootkit] ========================================\n");
    
    // Step 1: Dump encrypted sections
    DumpHyperionSections();
    
    // Step 2: Find and disable integrity checks
    DisableIntegrityChecks();
    
    // Step 3: Patch AC detection routines
    PatchACDetection();
    
    // Step 4: Inject your code
    InjectPayload();
    
    DbgPrint("[Bootkit] ========================================\n");
    DbgPrint("[Bootkit] HYPERION FULLY COMPROMISED\n");
    DbgPrint("[Bootkit] ========================================\n");
}

VOID DumpHyperionSections() {
    // Find .text section (encrypted)
    PVOID textSection = FindSection(g_HyperionBase, ".text");
    SIZE_T textSize = GetSectionSize(g_HyperionBase, ".text");
    
    DbgPrint("[Bootkit] Dumping Hyperion .text: %p (%llu bytes)\n", 
             textSection, (ULONGLONG)textSize);
    
    // Allocate buffer
    PVOID dumpBuffer = ExAllocatePoolWithTag(NonPagedPool, textSize, 'pmuD');
    if (dumpBuffer) {
        // Read it (page protections bypassed)
        if (NT_SUCCESS(ReadHyperionMemory(textSection, dumpBuffer, textSize))) {
            // Save to file or analyze
            SaveDumpToFile(dumpBuffer, textSize);
            
            // Decrypt if needed
            DecryptHyperionCode(dumpBuffer, textSize);
        }
        
        ExFreePoolWithTag(dumpBuffer, 'pmuD');
    }
}

VOID DisableIntegrityChecks() {
    DbgPrint("[Bootkit] Disabling Hyperion integrity checks...\n");
    
    // Scan for known patterns (CRC checks, hash validation, etc.)
    // Patch them to always return success
    
    // Example: Find CRC check routine
    PVOID crcCheck = ScanForPattern(g_HyperionBase, CRC_PATTERN, sizeof(CRC_PATTERN));
    if (crcCheck) {
        // Patch: mov eax, 1; ret (always return "valid")
        UCHAR patch[] = { 0xB8, 0x01, 0x00, 0x00, 0x00, 0xC3 };
        WriteHyperionMemory(crcCheck, patch, sizeof(patch));
        
        DbgPrint("[Bootkit] Patched CRC check at %p\n", crcCheck);
    }
}

VOID PatchACDetection() {
    DbgPrint("[Bootkit] Patching AC detection routines...\n");
    
    // Find debugger checks
    // Find memory scanner routines
    // Patch them to never detect anything
    
    // Example: IsDebuggerPresent check
    PVOID debugCheck = ScanForPattern(g_HyperionBase, DEBUG_PATTERN, sizeof(DEBUG_PATTERN));
    if (debugCheck) {
        // Patch: xor eax, eax; ret (always return "no debugger")
        UCHAR patch[] = { 0x33, 0xC0, 0xC3 };
        WriteHyperionMemory(debugCheck, patch, sizeof(patch));
        
        DbgPrint("[Bootkit] Patched debugger check at %p\n", debugCheck);
    }
}

VOID InjectPayload() {
    DbgPrint("[Bootkit] Injecting payload...\n");
    
    // Allocate memory in Hyperion process
    PVOID payloadAddr = NULL;
    SIZE_T payloadSize = 0x1000;
    
    ZwAllocateVirtualMemory(
        ZwCurrentProcess(),
        &payloadAddr,
        0,
        &payloadSize,
        MEM_COMMIT | MEM_RESERVE,
        PAGE_EXECUTE_READWRITE
    );
    
    if (payloadAddr) {
        // Write your shellcode
        WriteHyperionMemory(payloadAddr, your_payload, payload_size);
        
        // Create thread to execute it
        HANDLE threadHandle;
        PsCreateSystemThread(&threadHandle, ...);
        
        DbgPrint("[Bootkit] Payload injected at %p\n", payloadAddr);
    }
}
```

---

### 7. Driver Entry Point

```c
NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath) {
    DbgPrint("[Bootkit] ========================================\n");
    DbgPrint("[Bootkit] HYPERION DOMINATION DRIVER LOADED\n");
    DbgPrint("[Bootkit] ========================================\n");
    
    // Initialize tracking structures
    InitializeListHead(&g_ModifiedPages);
    KeInitializeSpinLock(&g_ModifiedPagesLock);
    
    // Hook SSDT (PatchGuard is disabled by bootkit, so this is safe)
    DbgPrint("[Bootkit] Installing SSDT hooks...\n");
    
    HookSSDT(0x50, Hook_NtProtectVirtualMemory, &Original_NtProtectVirtualMemory);
    DbgPrint("[Bootkit] Hooked NtProtectVirtualMemory\n");
    
    HookSSDT(0x23, Hook_NtQueryVirtualMemory, &Original_NtQueryVirtualMemory);
    DbgPrint("[Bootkit] Hooked NtQueryVirtualMemory\n");
    
    // Register process notification
    PsSetCreateProcessNotifyRoutineEx(ProcessCallback, FALSE);
    DbgPrint("[Bootkit] Registered process callback\n");
    
    // Set unload routine
    DriverObject->DriverUnload = DriverUnload;
    
    DbgPrint("[Bootkit] ========================================\n");
    DbgPrint("[Bootkit] READY TO COMPROMISE HYPERION\n");
    DbgPrint("[Bootkit] WAITING FOR ROBLOX...\n");
    DbgPrint("[Bootkit] ========================================\n");
    
    return STATUS_SUCCESS;
}

VOID DriverUnload(PDRIVER_OBJECT DriverObject) {
    DbgPrint("[Bootkit] Unloading driver...\n");
    
    // Remove process notification
    PsSetCreateProcessNotifyRoutineEx(ProcessCallback, TRUE);
    
    // Unhook SSDT
    // (restore original pointers)
    
    // Clean up
    if (g_HyperionProcess) {
        ObDereferenceObject(g_HyperionProcess);
    }
    
    // Free modified pages list
    while (!IsListEmpty(&g_ModifiedPages)) {
        PLIST_ENTRY entry = RemoveHeadList(&g_ModifiedPages);
        PMODIFIED_PAGE modPage = CONTAINING_RECORD(entry, MODIFIED_PAGE, ListEntry);
        ExFreePoolWithTag(modPage, 'domM');
    }
    
    DbgPrint("[Bootkit] Driver unloaded\n");
}
```

---

## Build Instructions

1. **Install WDK (Windows Driver Kit)**
2. **Create driver project:**
   ```
   HyperionKiller/
   ├── HyperionKiller.c
   ├── HyperionKiller.h
   ├── HyperionKiller.inf
   └── makefile
   ```

3. **Compile driver:**
   ```cmd
   cd C:\WinDDK\7600.16385.1\
   setenv.bat x64 fre
   cd C:\Path\To\HyperionKiller
   build
   ```

4. **Sign driver (required for load):**
   ```cmd
   makecert -r -pe -ss PrivateCertStore -n "CN=TestDriverCert" TestCert.cer
   signtool sign /s PrivateCertStore /n TestDriverCert /t http://timestamp.digicert.com HyperionKiller.sys
   ```

5. **Enable test signing:**
   ```cmd
   bcdedit /set testsigning on
   ```

6. **Load driver:**
   ```cmd
   sc create HyperionKiller binPath= "C:\Path\To\HyperionKiller.sys" type= kernel
   sc start HyperionKiller
   ```

---

## Runtime Flow

```
[Boot] EfiGuard patches kernel
       ↓
[Boot] Windows starts with compromised kernel
       ↓
[Load] HyperionKiller driver loads
       ↓
[Load] Driver hooks SSDT (NtProtectVirtualMemory, NtQueryVirtualMemory)
       ↓
[Wait] Driver waits for Roblox process
       ↓
[Detect] Roblox starts, Hyperion loads
       ↓
[Attach] Driver attaches to Roblox process
       ↓
[Dump] Dumps Hyperion's encrypted code
       ↓
[Patch] Modifies integrity checks
       ↓
[Inject] Injects your payload
       ↓
[Monitor] Hyperion tries to verify itself:
          - Calls NtProtectVirtualMemory → Driver lies → "No conflicts"
          - Calls NtQueryVirtualMemory → Driver lies → "Original protections"
          - Checks IC status → Kernel lied → "Active" (but not really)
          - Scans for debugger → Patched → "Not found"
       ↓
[Result] Hyperion thinks it's safe, continues running
         Your payload executes freely
         Full control over Roblox
```

---

## KEY ADVANTAGES

1. **No Syscall Failures**: All hooks return SUCCESS
2. **No Page Conflicts**: Kernel lies about protections
3. **No IC Detection**: Hyperion thinks IC is active (it's not)
4. **Kernel-Mode Access**: Bypasses ALL usermode protections
5. **Pre-Boot Patches**: Hyperion loads into compromised environment
6. **No PatchGuard**: SSDT hooks can't be detected

**Hyperion is blind, deaf, and lied to at every turn.**
