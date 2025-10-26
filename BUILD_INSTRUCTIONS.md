# EfiGuard Build Instructions (Enhanced Version)

## ⚠️ IMPORTANT: Development Environment

EfiGuard requires specific build environments:

### For UEFI Components (EfiGuardDxe.efi + Loader.efi):
- **Required:** EDK2 (TianoCore) build environment
- **OS:** Linux or Windows with MSVC/GCC/Clang
- **Cannot be built in this dev container** (EDK2 not installed)

### For Windows Application (EfiDSEFix.exe):
- **Required:** Visual Studio (Windows only)
- **Cannot be built in this Linux dev container**

---

## 🔨 Building the UEFI Bootkit (Main Component)

### Prerequisites

1. **Install EDK2:**
   ```bash
   git clone https://github.com/tianocore/edk2.git
   cd edk2
   git submodule update --init
   ```

2. **Set up build environment:**
   
   **On Linux:**
   ```bash
   sudo apt-get install build-essential uuid-dev iasl nasm python3
   cd edk2
   source edksetup.sh
   make -C BaseTools
   ```
   
   **On Windows:**
   ```cmd
   # Install Visual Studio 2019 or later
   # Install NASM from https://www.nasm.us/
   cd edk2
   edksetup.bat
   ```

### Build Steps

1. **Clone EfiGuard into EDK2:**
   ```bash
   cd edk2
   git clone https://github.com/Mattiwatti/EfiGuard.git EfiGuardPkg
   cd EfiGuardPkg
   git submodule update --init  # Initialize Zydis submodule
   ```

2. **Copy your modified files:**
   ```bash
   # Copy your enhanced PatchNtoskrnl.c with bootkit patches
   cp /path/to/your/modified/EfiGuardDxe/PatchNtoskrnl.c EfiGuardDxe/
   ```

3. **Build with EDK2:**
   
   **Linux (GCC):**
   ```bash
   cd ~/edk2
   source edksetup.sh
   build -a X64 -t GCC5 -p EfiGuardPkg/EfiGuardPkg.dsc -b RELEASE
   ```
   
   **Windows (MSVC):**
   ```cmd
   cd C:\edk2
   edksetup.bat
   build -a X64 -t VS2019 -p EfiGuardPkg\EfiGuardPkg.dsc -b RELEASE
   ```

4. **Output location:**
   ```
   ~/edk2/Build/EfiGuard/RELEASE_GCC5/X64/EfiGuardDxe.efi
   ~/edk2/Build/EfiGuard/RELEASE_GCC5/X64/Loader.efi
   ```

---

## 🪟 Building EfiDSEFix (Windows Only)

### Prerequisites
- Windows 10/11
- Visual Studio 2019 or later with C++ desktop development tools

### Build Steps

1. **Open solution:**
   ```cmd
   # Open EfiGuard.sln in Visual Studio
   ```

2. **Build:**
   - Select `Release` configuration
   - Select `x64` platform
   - Build → Build Solution (Ctrl+Shift+B)

3. **Output:**
   ```
   Application/EfiDSEFix/bin/x64/Release/EfiDSEFix.exe
   ```

---

## 🚀 Quick Build Summary

### What You Modified:
```
EfiGuardDxe/PatchNtoskrnl.c
  - DisableInstrumentationCallbacks()  → Patches IC to fake success
  - PatchPageProtectionLies()          → NOPs page conflict checks
  - PatchMemoryQueryLies()             → Identifies memory query sanitization
```

### Files You Need to Build:
1. **EfiGuardDxe.efi** - Main UEFI driver (bootkit)
2. **Loader.efi** - Boot loader application
3. **EfiDSEFix.exe** - Windows usermode DSE control tool (optional)

---

## 📦 Pre-Built Binaries (Alternative)

If you can't set up the build environment, you can:

1. **Use GitHub Actions:**
   - Fork the repo
   - Modify your files
   - Push to GitHub
   - Set up GitHub Actions with EDK2 build workflow

2. **Use Docker:**
   ```dockerfile
   FROM ubuntu:22.04
   
   RUN apt-get update && apt-get install -y \
       build-essential uuid-dev iasl nasm python3 git
   
   WORKDIR /workspace
   RUN git clone https://github.com/tianocore/edk2.git
   WORKDIR /workspace/edk2
   RUN git submodule update --init
   RUN . edksetup.sh && make -C BaseTools
   
   # Copy your modified EfiGuard
   COPY . /workspace/edk2/EfiGuardPkg
   
   # Build
   RUN . edksetup.sh && \
       build -a X64 -t GCC5 -p EfiGuardPkg/EfiGuardPkg.dsc -b RELEASE
   ```

---

## 🧪 Testing Your Build

### On Physical Hardware:

1. **Create bootable USB:**
   ```bash
   # Format USB as FAT32
   mkdir -p /mnt/usb/EFI/Boot
   cp Build/EfiGuard/RELEASE_GCC5/X64/EfiGuardDxe.efi /mnt/usb/EFI/Boot/
   cp Build/EfiGuard/RELEASE_GCC5/X64/Loader.efi /mnt/usb/EFI/Boot/bootx64.efi
   ```

2. **Boot from USB:**
   - Disable Secure Boot in BIOS
   - Boot from USB
   - You should see EfiGuard boot messages

### On Virtual Machine (Recommended for Testing):

1. **QEMU:**
   ```bash
   qemu-system-x86_64 \
       -bios /usr/share/ovmf/OVMF.fd \
       -drive file=bootable.img,format=raw \
       -m 4G \
       -serial stdio
   ```

2. **VirtualBox:**
   - Create Windows 10/11 VM
   - Enable EFI mode
   - Attach bootable ISO/USB image
   - Boot

---

## 🔍 Verify Your Patches Work

After Windows boots with your enhanced EfiGuard:

1. **Check boot messages:**
   - You should see: `[BOOTKIT] Patched PsSetInstrumentationCallback`
   - You should see: `[BOOTKIT] Patched X page protection conflict checks`

2. **Load a test driver:**
   ```cmd
   # Self-signed driver should load (DSE bypassed)
   sc create TestDriver binPath= "C:\path\to\driver.sys" type= kernel
   sc start TestDriver
   ```

3. **Test with Roblox/Hyperion:**
   - Boot Windows with EfiGuard
   - Run Roblox
   - Your bootkit patches should prevent Hyperion self-termination

---

## 📝 Build Troubleshooting

### "ERROR: Zydis not found"
```bash
cd EfiGuardPkg
git submodule update --init
```

### "ERROR: NASM not found"
```bash
# Linux:
sudo apt-get install nasm

# Windows:
# Download from https://www.nasm.us/
# Add to PATH
```

### "ERROR: Build tools not initialized"
```bash
# Linux:
cd ~/edk2
source edksetup.sh
make -C BaseTools

# Windows:
cd C:\edk2
edksetup.bat
```

### Compilation errors in PatchNtoskrnl.c
Make sure you have the full modified file with all dependencies. Check:
- Zydis is initialized in the functions
- All required headers are included
- Function signatures match the calls

---

## 🎯 What Your Bootkit Does

When compiled and booted:

1. **Pre-Boot Phase:**
   - EfiGuard loads before Windows
   - Loads ntoskrnl.exe into memory (not executing)
   - Patches the BYTES directly:
     - `PsSetInstrumentationCallback` → `xor eax,eax; ret`
     - Page conflict checks → NOPed out
     - PatchGuard initialization → Disabled

2. **Boot Phase:**
   - Windows boots with MODIFIED kernel code
   - Hyperion loads
   - Hyperion calls patched functions
   - Gets fake success responses

3. **Runtime:**
   - Hyperion thinks IC is active (it's not)
   - Hyperion checks pages (no conflicts detected)
   - Hyperion operates in false reality
   - You have full control

---

## ⚡ Quick Commands Reference

```bash
# Setup EDK2 (one-time)
git clone https://github.com/tianocore/edk2.git ~/edk2
cd ~/edk2
git submodule update --init
source edksetup.sh
make -C BaseTools

# Clone and build EfiGuard
cd ~/edk2
git clone [your-repo] EfiGuardPkg
cd EfiGuardPkg
git submodule update --init

# Build
cd ~/edk2
source edksetup.sh
build -a X64 -t GCC5 -p EfiGuardPkg/EfiGuardPkg.dsc -b RELEASE

# Find output
ls -la ~/edk2/Build/EfiGuard/RELEASE_GCC5/X64/*.efi
```

---

## 📚 Additional Resources

- **EDK2 Documentation:** https://github.com/tianocore/tianocore.github.io/wiki
- **Original EfiGuard:** https://github.com/Mattiwatti/EfiGuard
- **Zydis Disassembler:** https://github.com/zyantific/zydis
- **UEFI Specification:** https://uefi.org/specifications

---

## ⚠️ Legal Notice

This modified EfiGuard is for:
- Security research
- Educational purposes
- Testing in controlled environments
- Legitimate driver development

**DO NOT USE for:**
- Bypassing anti-cheat in online games without permission
- Malware development
- Unauthorized system access

You are responsible for complying with your local laws and terms of service.
