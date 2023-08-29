#include <Uefi.h>
#include <Pi/PiDxeCis.h>

#include <Protocol/EfiGuard.h>
#include <Protocol/SimpleFileSystem.h>
#include <Protocol/LoadedImage.h>
#include <Protocol/LegacyBios.h>
#include <Library/UefiLib.h>
#include <Library/DebugLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/DevicePathLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiBootManagerLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>


//
// Define whether the loader should prompt for driver configuration or not.
// If this is 0, the defaults are used and Windows will be booted with no user interaction.
// This can be overridden on the command line with -D CONFIGURE_DRIVER=[0|1]
//
#ifndef CONFIGURE_DRIVER
#define CONFIGURE_DRIVER	0
#endif


//
// Paths to the driver to try
//
#ifndef EFIGUARD_DRIVER_FILENAME
#define EFIGUARD_DRIVER_FILENAME		L"EfiGuardDxe.efi"
#endif
STATIC CHAR16* mDriverPaths[] = {
	L"\\EFI\\Boot\\" EFIGUARD_DRIVER_FILENAME,
	L"\\EFI\\" EFIGUARD_DRIVER_FILENAME,
	L"\\" EFIGUARD_DRIVER_FILENAME
};


VOID
EFIAPI
BmSetMemoryTypeInformationVariable(
	IN BOOLEAN Boot
	);


STATIC
BOOLEAN
EFIAPI
WaitForKey(
	VOID
	)
{
	EFI_INPUT_KEY Key = { 0, 0 };
	UINTN Index = 0;
	gBS->WaitForEvent(1, &gST->ConIn->WaitForKey, &Index);
	gST->ConIn->ReadKeyStroke(gST->ConIn, &Key);

	return Key.ScanCode != SCAN_ESC;
}

#if CONFIGURE_DRIVER

STATIC
UINT16
EFIAPI
PromptInput(
	IN CONST UINT16* AcceptedChars,
	IN UINTN NumAcceptedChars,
	IN UINT16 DefaultSelection
	)
{
	UINT16 SelectedChar;

	while (TRUE)
	{
		SelectedChar = CHAR_NULL;

		EFI_INPUT_KEY Key = { 0, 0 };
		UINTN Index = 0;
		gBS->WaitForEvent(1, &gST->ConIn->WaitForKey, &Index);
		gST->ConIn->ReadKeyStroke(gST->ConIn, &Key);

		if (Key.UnicodeChar == CHAR_LINEFEED || Key.UnicodeChar == CHAR_CARRIAGE_RETURN)
		{
			SelectedChar = DefaultSelection;
			break;
		}

		for (UINTN i = 0; i < NumAcceptedChars; ++i)
		{
			if (Key.UnicodeChar == AcceptedChars[i])
			{
				SelectedChar = Key.UnicodeChar;
				break;
			}
		}

		if (SelectedChar != CHAR_NULL)
			break;
	}

	Print(L"%c\r\n\r\n", SelectedChar);
	return SelectedChar;
}

#endif


// 
// Try to find a file by browsing each device
// 
STATIC
EFI_STATUS
LocateFile(
	IN CHAR16* ImagePath,
	OUT EFI_DEVICE_PATH** DevicePath
	)
{
	*DevicePath = NULL;

	UINTN NumHandles;
	EFI_HANDLE* Handles;
	EFI_STATUS Status = gBS->LocateHandleBuffer(ByProtocol,
												&gEfiSimpleFileSystemProtocolGuid,
												NULL,
												&NumHandles,
												&Handles);
	if (EFI_ERROR(Status))
		return Status;

	DEBUG((DEBUG_INFO, "[LOADER] Number of UEFI Filesystem Devices: %llu\r\n", NumHandles));

	for (UINTN i = 0; i < NumHandles; i++)
	{
		EFI_FILE_IO_INTERFACE *IoDevice;
		Status = gBS->OpenProtocol(Handles[i],
									&gEfiSimpleFileSystemProtocolGuid,
									(VOID**)&IoDevice,
									gImageHandle,
									NULL,
									EFI_OPEN_PROTOCOL_GET_PROTOCOL);
		if (Status != EFI_SUCCESS)
			continue;

		EFI_FILE_HANDLE VolumeHandle;
		Status = IoDevice->OpenVolume(IoDevice, &VolumeHandle);
		if (EFI_ERROR(Status))
			continue;

		EFI_FILE_HANDLE FileHandle;
		Status = VolumeHandle->Open(VolumeHandle,
									&FileHandle,
									ImagePath,
									EFI_FILE_MODE_READ,
									EFI_FILE_READ_ONLY);
		if (!EFI_ERROR(Status))
		{
			VolumeHandle->Close(FileHandle);
			*DevicePath = FileDevicePath(Handles[i], ImagePath);
			CHAR16 *PathString = ConvertDevicePathToText(*DevicePath, TRUE, TRUE);
			DEBUG((DEBUG_INFO, "[LOADER] Found file at %S.\r\n", PathString));
			if (PathString != NULL)
				FreePool(PathString);
			break;
		}
	}

	FreePool(Handles);

	return Status;
}

//
// Find the optimal available console output mode and set it if it's not already the current mode
//
STATIC
EFI_STATUS
EFIAPI
SetHighestAvailableTextMode(
	VOID
	)
{
	if (gST->ConOut == NULL)
		return EFI_NOT_READY;

	INT32 MaxModeNum = 0;
	UINTN Cols, Rows, MaxWeightedColsXRows = 0;
	EFI_STATUS Status = EFI_SUCCESS;

	for (INT32 ModeNum = 0; ModeNum < gST->ConOut->Mode->MaxMode; ModeNum++)
	{
		Status = gST->ConOut->QueryMode(gST->ConOut, ModeNum, &Cols, &Rows);
		if (EFI_ERROR(Status))
			continue;

		// Accept only modes where the total of (Rows * Columns) >= the previous known best.
		// Use 16:10 as an arbitrary weighting that lies in between the common 4:3 and 16:9 ratios
		CONST UINTN WeightedColsXRows = (16 * Rows) * (10 * Cols);
		if (WeightedColsXRows >= MaxWeightedColsXRows)
		{
			MaxWeightedColsXRows = WeightedColsXRows;
			MaxModeNum = ModeNum;
		}
	}

	if (gST->ConOut->Mode->Mode != MaxModeNum)
	{
		Status = gST->ConOut->SetMode(gST->ConOut, MaxModeNum);
	}

	// Clear screen and enable cursor
	gST->ConOut->ClearScreen(gST->ConOut);
	gST->ConOut->EnableCursor(gST->ConOut, TRUE);

	return Status;
}

STATIC
EFI_STATUS
EFIAPI
StartAndConfigureDriver(
	IN EFI_HANDLE ImageHandle,
	IN EFI_SYSTEM_TABLE* SystemTable
	)
{
	EFIGUARD_DRIVER_PROTOCOL* EfiGuardDriverProtocol;
	EFI_DEVICE_PATH *DriverDevicePath = NULL;

	// 
	// Check if the driver is loaded 
	// 
	EFI_STATUS Status = gBS->LocateProtocol(&gEfiGuardDriverProtocolGuid,
											NULL,
											(VOID**)&EfiGuardDriverProtocol);
	ASSERT((!EFI_ERROR(Status) || Status == EFI_NOT_FOUND));
	if (Status == EFI_NOT_FOUND)
	{
		Print(L"[LOADER] Locating and loading driver file %S...\r\n", EFIGUARD_DRIVER_FILENAME);
		for (UINT32 i = 0; i < ARRAY_SIZE(mDriverPaths); ++i)
		{
			Status = LocateFile(mDriverPaths[i], &DriverDevicePath);
			if (!EFI_ERROR(Status))
				break;
		}
		if (EFI_ERROR(Status))
		{
			Print(L"[LOADER] Failed to find driver file %S.\r\n", EFIGUARD_DRIVER_FILENAME);
			goto Exit;
		}

		EFI_HANDLE DriverHandle = NULL;
		Status = gBS->LoadImage(FALSE, // Request is not from boot manager
								ImageHandle,
								DriverDevicePath,
								NULL,
								0,
								&DriverHandle);
		if (EFI_ERROR(Status))
		{
			Print(L"[LOADER] LoadImage failed: %llx (%r).\r\n", Status, Status);
			goto Exit;
		}

		Status = gBS->StartImage(DriverHandle, NULL, NULL);
		if (EFI_ERROR(Status))
		{
			Print(L"[LOADER] StartImage failed: %llx (%r).\r\n", Status, Status);
			goto Exit;
		}

		Status = gBS->LocateProtocol(&gEfiGuardDriverProtocolGuid,
									NULL,
									(VOID**)&EfiGuardDriverProtocol);
		if (EFI_ERROR(Status))
		{
			Print(L"[LOADER] LocateProtocol failed: %llx (%r).\r\n", Status, Status);
			goto Exit;
		}
	}
	else
	{
		Print(L"[LOADER] The driver is already loaded.\r\n");
		Status = EFI_ALREADY_STARTED;
		goto Exit;
	}

#if CONFIGURE_DRIVER
	//
	// Interactive driver configuration
	//
	Print(L"\r\nChoose the type of DSE bypass to use, or press ENTER for default:\r\n"
		L"    [1] No DSE bypass\r\n    [2] Boot time DSE bypass\r\n    [3] Runtime SetVariable hook (default)\r\n    ");
	CONST UINT16 AcceptedDseBypasses[] = { L'1', L'2', L'3' };
	CONST UINT16 SelectedDseBypass = PromptInput(AcceptedDseBypasses,
												sizeof(AcceptedDseBypasses) / sizeof(UINT16),
												L'3');

	Print(L"Wait for a keypress to continue after each patch stage? (for debugging)\n"
		L"    [1] Yes\r\n    [2] No (default)\r\n    ");
	CONST UINT16 YesNo[] = { L'1', L'2' };
	CONST UINT16 SelectedWaitForKeyPress = PromptInput(YesNo,
											sizeof(YesNo) / sizeof(UINT16),
											L'2');

	EFIGUARD_CONFIGURATION_DATA ConfigData;
	if (SelectedDseBypass == L'1')
		ConfigData.DseBypassMethod = DSE_DISABLE_NONE;
	else if (SelectedDseBypass == L'2')
		ConfigData.DseBypassMethod = DSE_DISABLE_AT_BOOT;
	else
		ConfigData.DseBypassMethod = DSE_DISABLE_SETVARIABLE_HOOK;
	ConfigData.WaitForKeyPress = (BOOLEAN)(SelectedWaitForKeyPress == L'1');

	//
	// Send the configuration data to the driver
	//
	Status = EfiGuardDriverProtocol->Configure(&ConfigData);

	if (EFI_ERROR(Status))
		Print(L"[LOADER] Driver Configure() returned error %llx (%r).\r\n", Status, Status);
#endif

Exit:
	if (DriverDevicePath != NULL)
		FreePool(DriverDevicePath);

	return Status;
}

//
// Attempt to boot each Windows boot option in the BootOptions array.
// This function is a combined and simplified version of BootBootOptions (BdsDxe) and EfiBootManagerBoot (UefiBootManagerLib),
// except for the fact that we are of course not in the BDS phase and also not a driver or the platform boot manager.
// The Windows boot manager doesn't have to know about all this, that would only confuse it
//
STATIC
BOOLEAN
TryBootOptionsInOrder(
	IN EFI_BOOT_MANAGER_LOAD_OPTION *BootOptions,
	IN UINTN BootOptionCount,
	IN UINT16 CurrentBootOptionIndex,
	IN BOOLEAN OnlyBootWindows
	)
{
	//
	// Iterate over the boot options 'in BootOrder order'
	//
	EFI_DEVICE_PATH_PROTOCOL* FullPath;
	for (UINTN Index = 0; Index < BootOptionCount; ++Index)
	{
		//
		// This is us
		//
		if (BootOptions[Index].OptionNumber == CurrentBootOptionIndex)
			continue;

		//
		// No LOAD_OPTION_ACTIVE, no load
		//
		if ((BootOptions[Index].Attributes & LOAD_OPTION_ACTIVE) == 0)
			continue;

		//
		// Ignore LOAD_OPTION_CATEGORY_APP entries
		//
		if ((BootOptions[Index].Attributes & LOAD_OPTION_CATEGORY) != LOAD_OPTION_CATEGORY_BOOT)
			continue;

		//
		// Ignore legacy (BBS) entries, unless non-Windows entries are allowed (second boot attempt)
		//
		const BOOLEAN IsLegacy = DevicePathType(BootOptions[Index].FilePath) == BBS_DEVICE_PATH &&
			DevicePathSubType(BootOptions[Index].FilePath) == BBS_BBS_DP;
		if (OnlyBootWindows && IsLegacy)
			continue;

		//
		// Filter out non-Windows boot entries.
		// Check the description first as "Windows Boot Manager" entries are obviously going to boot Windows.
		// However the inverse is not true, i.e. not all entries that boot Windows will have this description.
		//
		BOOLEAN MaybeWindows = FALSE;
		if (BootOptions[Index].Description != NULL &&
			StrStr(BootOptions[Index].Description, L"Windows Boot Manager") != NULL)
		{
			MaybeWindows = TRUE;
		}

		// We need the full path to LoadImage the file with BootPolicy = TRUE.
		UINTN FileSize;
		VOID* FileBuffer = EfiBootManagerGetLoadOptionBuffer(BootOptions[Index].FilePath, &FullPath, &FileSize);
		if (FileBuffer != NULL)
			FreePool(FileBuffer);

		// EDK2's EfiBootManagerGetLoadOptionBuffer will sometimes give a NULL "full path"
		// from an originally non-NULL file path. If so, swap it back (and don't free it).
		if (FullPath == NULL)
			FullPath = BootOptions[Index].FilePath;

		// Get the text representation of the device path
		CHAR16* ConvertedPath = ConvertDevicePathToText(FullPath, FALSE, FALSE);

		// If this is not a named "Windows Boot Manager" entry, apply some heuristics based on the device path,
		// which must end in "bootmgfw.efi" or "bootx64.efi". In the latter case we may get false positives,
		// but for some types of boots the filename will always be bootx64.efi, so this can't be avoided.
		if (!MaybeWindows &&
			ConvertedPath != NULL &&
			(StrStr(ConvertedPath, L"bootmgfw.efi") != NULL || StrStr(ConvertedPath, L"BOOTMGFW.EFI") != NULL ||
			StrStr(ConvertedPath, L"bootx64.efi") != NULL || StrStr(ConvertedPath, L"BOOTX64.EFI") != NULL))
		{
			MaybeWindows = TRUE;
		}

		if (OnlyBootWindows && !MaybeWindows)
		{
			if (FullPath != BootOptions[Index].FilePath)
				FreePool(FullPath);
			if (ConvertedPath != NULL)
				FreePool(ConvertedPath);
			
			// Not Windows; skip this entry
			continue;
		}

		// Print what we're booting
		if (ConvertedPath != NULL)
		{
			Print(L"Booting %Sdevice path %S...\r\n", IsLegacy ? L"legacy " : L"", ConvertedPath);
			FreePool(ConvertedPath);
		}

		//
		// Boot this image.
		//
		// DO NOT: call EfiBootManagerBoot(BootOption) to 'simplify' this process.
		// The driver will not work in this case due to EfiBootManagerBoot calling BmSetMemoryTypeInformationVariable(),
		// which performs a warm reset of the system if, for example, the category of the current boot option changed
		// from 'app' to 'boot'. Which is precisely what we are doing...
		//
		// Change the BootCurrent variable to the option number for our boot selection
		UINT16 OptionNumber = (UINT16)BootOptions[Index].OptionNumber;
		EFI_STATUS Status = gRT->SetVariable(EFI_BOOT_CURRENT_VARIABLE_NAME,
											&gEfiGlobalVariableGuid,
											EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_RUNTIME_ACCESS,
											sizeof(UINT16),
											&OptionNumber);
		ASSERT_EFI_ERROR(Status);

		// Signal the EVT_SIGNAL_READY_TO_BOOT event
		EfiSignalEventReadyToBoot();

		// Handle BBS entries
		if (IsLegacy)
		{
			Print(L"\r\nNOTE: EfiGuard does not support legacy (non-UEFI) Windows installations.\r\n"
				L"The legacy OS will be booted, but EfiGuard will not work.\r\nPress any key to acknowledge...\r\n");
			WaitForKey();

			EFI_LEGACY_BIOS_PROTOCOL *LegacyBios;
			Status = gBS->LocateProtocol(&gEfiLegacyBiosProtocolGuid,
										NULL,
										(VOID**)&LegacyBios);
			ASSERT_EFI_ERROR(Status);

			BootOptions[Index].Status = LegacyBios->LegacyBoot(LegacyBios,
															(BBS_BBS_DEVICE_PATH*)BootOptions[Index].FilePath,
															BootOptions[Index].OptionalDataSize,
															BootOptions[Index].OptionalData);
			return !EFI_ERROR(BootOptions[Index].Status);
		}

		// So again, DO NOT call this abortion:
		//BmSetMemoryTypeInformationVariable((BOOLEAN)((BootOptions[Index].Attributes & LOAD_OPTION_CATEGORY) == LOAD_OPTION_CATEGORY_BOOT));
		//
		// OK, maybe call it after all, but pretend this is *not* a boot entry, so that the system will not go into an infinite boot (reset) loop.
		// This may or may not fix hibernation related issues (S4 entry/resume). See https://github.com/Mattiwatti/EfiGuard/issues/12
		BmSetMemoryTypeInformationVariable(FALSE);

		// Ensure the image path is connected end-to-end by Dispatch()ing any required drivers through DXE services
		EfiBootManagerConnectDevicePath(BootOptions[Index].FilePath, NULL);

		// Instead of creating a ramdisk and reading the file into it (¿que?), just pass the path we saved earlier.
		// This is the point where the driver kicks in via its LoadImage hook.
		EFI_HANDLE ImageHandle = NULL;
		Status = gBS->LoadImage(TRUE,
								gImageHandle,
								FullPath,
								NULL,
								0,
								&ImageHandle);

		if (FullPath != BootOptions[Index].FilePath)
			FreePool(FullPath);

		if (EFI_ERROR(Status))
		{
			// Unload if execution could not be deferred to avoid a resource leak
			if (Status == EFI_SECURITY_VIOLATION)
				gBS->UnloadImage(ImageHandle);

			Print(L"LoadImage error %llx (%r)\r\n", Status, Status);
			continue;
		}

		// Get loaded image info
		EFI_LOADED_IMAGE_PROTOCOL* ImageInfo;
		Status = gBS->OpenProtocol(ImageHandle,
									&gEfiLoadedImageProtocolGuid,
									(VOID**)&ImageInfo,
									gImageHandle,
									NULL,
									EFI_OPEN_PROTOCOL_GET_PROTOCOL);
		ASSERT_EFI_ERROR(Status);

		// Set image load options from the boot option
		ImageInfo->LoadOptionsSize = BootOptions[Index].OptionalDataSize;
		ImageInfo->LoadOptions = BootOptions[Index].OptionalData;

		// "Clean to NULL because the image is loaded directly from the firmware's boot manager." (EDK2) Good call, I agree
		ImageInfo->ParentHandle = NULL;

		// Enable the Watchdog Timer for 5 minutes before calling the image
		gBS->SetWatchdogTimer((UINTN)(5 * 60), 0x0000, 0x00, NULL);

		// Start the image and set the return code in the boot option status
		Status = gBS->StartImage(ImageHandle,
								&BootOptions[Index].ExitDataSize,
								&BootOptions[Index].ExitData);
		BootOptions[Index].Status = Status;
		if (EFI_ERROR(Status))
		{
			Print(L"StartImage error %llx (%r)\r\n", Status, Status);
			continue;
		}

		//
		// Success. Code below is never executed
		//

		// Clear the watchdog timer after the image returns
		gBS->SetWatchdogTimer(0x0000, 0x0000, 0x0000, NULL);

		// Clear the BootCurrent variable
		gRT->SetVariable(EFI_BOOT_CURRENT_VARIABLE_NAME,
						&gEfiGlobalVariableGuid,
						0,
						0,
						NULL);

		if (BootOptions[Index].Status == EFI_SUCCESS)
			return TRUE;
	}

	// All boot attempts failed, or no suitable entries were found
	return FALSE;
}

EFI_STATUS
EFIAPI
UefiMain(
	IN EFI_HANDLE ImageHandle,
	IN EFI_SYSTEM_TABLE* SystemTable
	)
{
	//
	// Connect all drivers to all controllers
	//
	EfiBootManagerConnectAll();

	//
	// Set the highest available console mode and clear the screen
	//
	SetHighestAvailableTextMode();

	//
	// Turn off the watchdog timer
	//
	gBS->SetWatchdogTimer(0, 0, 0, NULL);

	//
	// Locate, load, start and configure the driver
	//
	CONST EFI_STATUS DriverStatus = StartAndConfigureDriver(ImageHandle, SystemTable);
	if (DriverStatus == EFI_ALREADY_STARTED)
		return EFI_SUCCESS;

	if (EFI_ERROR(DriverStatus))
	{
		Print(L"\r\nERROR: driver load failed with status %llx (%r).\r\n"
			L"Press any key to continue, or press ESC to return to the firmware or shell.\r\n",
			DriverStatus, DriverStatus);
		if (!WaitForKey())
		{
			return DriverStatus;
		}
	}

	//
	// Start the "boot through" procedure to boot Windows.
	//
	// First obtain our own boot option number, since we don't want to boot ourselves again
	UINT16 CurrentBootOptionIndex;
	UINT32 Attributes = EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_RUNTIME_ACCESS;
	UINTN Size = sizeof(CurrentBootOptionIndex);
	CONST EFI_STATUS Status = gRT->GetVariable(EFI_BOOT_CURRENT_VARIABLE_NAME,
												&gEfiGlobalVariableGuid,
												&Attributes,
												&Size,
												&CurrentBootOptionIndex);
	if (EFI_ERROR(Status))
	{
		CurrentBootOptionIndex = 0xFFFF;
		Print(L"WARNING: failed to query the current boot option index variable.\r\n"
			L"This could lead to the current device being booted recursively.\r\n"
			L"If you booted from a removable device, it is recommended that you remove it now.\r\n"
			L"\r\nPress any key to continue...\r\n");
		WaitForKey();
	}

	// Query all boot options, and try each following the order set in the "BootOrder" variable, except
	// (1) Do not boot ourselves again, and
	// (2) The description or filename must indicate the boot option is some form of Windows.
	UINTN BootOptionCount;
	EFI_BOOT_MANAGER_LOAD_OPTION* BootOptions = EfiBootManagerGetLoadOptions(&BootOptionCount, LoadOptionTypeBoot);
	BOOLEAN BootSuccess = TryBootOptionsInOrder(BootOptions,
												BootOptionCount,
												CurrentBootOptionIndex,
												TRUE);
	if (!BootSuccess)
	{
		// We did not find any Windows boot entry; retry without the "must be Windows" restriction.
		BootSuccess = TryBootOptionsInOrder(BootOptions,
											BootOptionCount,
											CurrentBootOptionIndex,
											FALSE);
	}
	EfiBootManagerFreeLoadOptions(BootOptions, BootOptionCount);

	if (BootSuccess)
		return EFI_SUCCESS;

	// We should never reach this unless something is seriously wrong (no boot device / partition table corrupted / catastrophic boot manager failure...)
	Print(L"Failed to boot anything. This is super bad!\r\n"
		L"Press any key to return to the firmware or shell,\r\nwhich will surely fix this and not make things worse.\r\n");
	WaitForKey();

	gBS->Exit(gImageHandle, EFI_SUCCESS, 0, NULL);

	return EFI_SUCCESS;
}
