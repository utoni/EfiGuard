# HYPERION — The Roblox usermode anti-tamper
*By: Ntzu v010 — Hyperion analysis*

> Hey, been a long time, right? In this post I'll be talking about **HYPERION** — the usermode anti-tamper behind Roblox that completely changed the exploiting community for the game.

---

## Table of contents
1. [Introduction](#introduction)
2. [Main Components](#main-components)
   - [Obfuscation techniques](#obfuscation-techniques)
   - [Memory & import protection](#memory--import-protection)
   - [Initialization routine & Instrumentation Callback](#initialization-routine--instrumentation-callback)
3. [Memory Management & Protection](#memory-management--protection)
4. [Advanced Protections](#advanced-protections)
5. [Hypervisor & Virtualization Detections](#hypervisor--virtualization-detections)
6. [Scanning Processes](#scanning-processes)
7. [Limitations](#limitations)
8. [Conclusion](#conclusion)
9. [Bonus: Common ways people try to bypass their checks (notes)](#bonus-common-ways-people-try-to-bypass-their-checks-notes)

---

## Introduction

**Hyperion** (originally an anti-tamper product by Byfron) is the usermode anti-tamper used by Roblox. Roblox acquired the product and it is commonly referred to simply as *Hyperion*. Although often lumped together with anti-cheat systems, Hyperion’s primary objective is **anti-tamper**: preventing modifications to Roblox’s executable code and protecting the runtime environment from unauthorized changes.

Its architecture focuses on making reverse engineering, debugging, and tampering extremely difficult. At first glance many expected a “typical” anti-tamper, but Hyperion goes well beyond the usual: a combination of obfuscation, dynamic protections, kernel-adjacent monitoring hooks, and execution-time controls.

---

## Main Components

There’s a lot here — below are the main pieces grouped for clarity.

### Obfuscation techniques

- **Fake instruction sequences:** The executable is filled with arbitrary sequences that confuse static disassemblers. Repetitive fake patterns make it hard to know which blocks are functional code vs. junk. Tools like IDA Pro or Binary Ninja can be misled into incomplete or inaccurate disassembly.
- **Dead code inflation:** Non-functional code that inflates stack frames and obscures control flow, making functions appear more complex than they are.
- **Unconditional jumps to decrypted addresses:** Control flow sometimes jumps to dynamically decrypted destinations, breaking linear analysis and forcing reversers to resolve or emulate jump destinations in real time.

### Memory & import protection

- **Dynamic import encryption:** Imports are encrypted and decrypted only when needed (similar to protections applied to page table entries). Incorrect access can trigger traps or crashes.
- **Memory monitoring & page protections:** Hyperion hooks allocation/protection syscalls (e.g. `NtProtectVirtualMemory`, `NtAllocateVirtualMemory`) to whitelist specific executable regions and restrict unauthorized changes.
- **Dual mapped views for syscalls:** Memory regions are managed with paired views (RW vs RX) so code can be managed and executed without exposing its layout.
- **Hash-based import resolution:** Imports may be resolved through hashed keys (e.g., FNV-1a-32) with handler tables per module (`ntdll`, `kernelbase`), allowing only validated keys to be decrypted and invoked.

### Initialization routine & Instrumentation Callback

- **Pre-execution control via loader:** Hyperion ensures its module runs before Roblox’s main code by leveraging loader entry techniques (entry DLLs, etc.). This allows it to set up protections, encrypt `.text`, register hooks, and preload/protect imports.
- **Custom memory section mappings:** Important libraries are mapped with non-standard flags (e.g., `SEC_NOCHANGE`) to prevent simple Copy-On-Write tampering and remapping.
- **Instrumentation Callback (IC):** Hyperion registers an undocumented Windows instrumentation callback to intercept usermode→kernelmode transitions. ICs let the product monitor and control threads, manage exceptions, and prevent unauthorized actions at the transition boundary.

- **Thread control via LdrInitializeThunk hooking:** Hyperion hooks `LdrInitializeThunk` (which is invoked when a thread is created) to intercept thread startup. They perform whitelist checks to determine whether a thread is allowed to run; unapproved threads can be terminated immediately. This check works in tandem with other `ntdll` hooks to centralize thread validation and control.

> Roblox will also tamper with or corrupt allocated pages if they are executable but not present in Hyperion's whitelist. Some researchers bypass this by unlinking the VAD (Virtual Address Descriptor) node from kernel memory so those allocations do not appear in usermode scans, while others manually whitelist their pages to keep them intact.

- **Inline syscalls & custom syscall stubs:** Rather than relying exclusively on standard system call wrappers, Hyperion employs inline syscalls, custom syscall stubs stored in RX memory, and dynamic syscall ID assignment to complicate syscall tracking and patching.

---

## Memory Management & Protection

Hyperion uses several layered runtime protections:

- **Dynamic code encryption & JIT decryption:** The `.text` section is encrypted in memory; code is decrypted just-in-time (JIT) for execution. Pages are typically marked no-access and are decrypted only when the runtime verifies the access.
- **No-access protections + exception-driven decryption:** Unauthorized access attempts trigger exceptions; exception handlers validate the access and perform on-the-fly decryption, temporarily marking pages executable and then re-encrypting them afterward.
- **Trap pages and execution verification:** Randomly placed trap pages will crash the process if accessed unexpectedly, preventing brute-force decryptors and blind scanning attempts.
- **Periodic memory scans & whitelisting:** Hyperion periodically scans executable pages and cross-references them against a whitelist. Any unapproved executable region will have its execute permission revoked and can cause a crash if used.
- **Dual-view memory to avoid race conditions:** The product maintains two views of the same physical region — one encrypted/read-only and another writable/decrypted — to avoid race conditions and prevent concurrent tampering attempts.

> **Operational note:** Roblox will actively tamper with or corrupt allocated pages if they are executable but not present in Hyperion's whitelist. A common bypass some researchers use is to unlink the VAD (Virtual Address Descriptor) node from kernel memory so that those allocations do not appear in usermode scans; others opt to have their pages manually whitelisted so they survive Hyperion's checks.

- **YARA scanning:** Hyperion integrates YARA-style scanning to detect known tamper signatures (implementation details vary; not expanded here).

---

## Advanced Protections

Beyond memory protections and thread hooks, Hyperion implements more subtle anti-analysis techniques:

- **INT3 breakpoints at branching instructions:** INT3 is placed at key branches; when hit, an exception handler conditionally restores the original instruction for execution and then re-applies the breakpoint. This is combined with timing checks to distinguish normal execution from debugging/tracing.
- **Nanomite technique (timing-based INT3 validation):** INT3 breakpoints replace first instructions of many functions; the handler measures timing between exceptions and only allows execution if timings match expected norms, making debugging by stepping or slow tracing detectable.
- **Inline syscall usage (again):** Inline syscalls obscure the point where kernel transitions happen and complicate hooking or interception of those transitions by analysts.

---

## Hypervisor & Virtualization Detections

Hyperion includes checks to detect virtualized or instrumented environments:

- **CPUID / compatibility mode tricks:** Forcing the CPU into a different compatibility mode (e.g., 32-bit) and executing specific instructions can produce side-effects or overflows that reveal incorrect hypervisor handling.
- **Trap flags and forced VMExit:** Setting trap flags or using instructions that cause VMExits (or raise specific exceptions like `#UD`) can reveal hypervisor emulation bugs, because some hypervisors mishandle these states or fail to mirror expected behavior.
- **#UD (undefined instruction) exception detection:** Hyperion leverages #UD exceptions to detect inconsistencies in hypervisor emulation, especially in those that improperly handle instructions like `syscall` and `ret`. Some hypervisors disable CPU extensions, causing `#UD` exceptions when they encounter those instructions. Hyperion identifies these by checking if the hypervisor assumes every `#UD` relates to syscalls. The proper handling method is to inspect the instruction that triggered the exception and confirm it is a syscall before emulating or handling it; if not, the #UD should be propagated back to the guest.

Note: Because Hyperion runs in usermode (CPL3), hardware-level or well-hidden kernel/rootkit-level hypervisors may bypass some usermode checks. Hyperion’s HV detection is strong but not infallible against advanced kernel/hypervisor solutions.

---

## Scanning Processes

Hyperion actively scans for known reversing/debugging tools and suspicious artifacts:


- **Named objects and registry inspection:** Hyperion inspects named objects and registry keys associated with debugging tools; matches can trigger countermeasures.
- **Simple but effective approach:** Many detections rely on presence checks (names, handles, known artifacts). While simple, when combined with other protections they create friction for analysts.

---

## Limitations

Hyperion is robust, but it has constraints:

- **Usermode limitations (CPL3):** Running entirely in usermode means it cannot directly detect or control kernel-level manipulation, kernel rootkits, or hardware backed hypervisors.
- **Easily-targetable attack surface for some checks:** Because some checks depend on usermode queries (e.g., `NtQueryDirectoryObject`) or process-visible artifacts, an attacker with sufficient privileges can hide or spoof those artifacts by hooking or interposing relevant APIs.
- **Remapping / unmapping / hooking strategies:** In some cases, unmapping the Hyperion module, installing hooks for syscalls, and then remapping can allow an attacker to intercept or fake results the IC expects.

---

## Conclusion

Hyperion represents a large step up from typical usermode protections by combining runtime encryption, exception-driven decryption, instrumentation callbacks, syscall obfuscation, and multiple runtime integrity checks. It elevates the bar for static reverse engineering and simple dynamic analysis.

However, Hyperion’s usermode nature leaves it vulnerable to more privileged attacks (kernel-level or hardware-backed) and to clever interposition/hooking tricks. The system is strong at raising the cost of reversing and tampering, but not impregnable.

---

Hyperion’s design explicitly addresses common race patterns.

---

*End of analysis — have fun reading!*

