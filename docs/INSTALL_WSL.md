# Install the operating system and the toolchain

**COMP6697001 Operating Systems - AOL Case Study, Practical Work part 1**

This document covers the first requirement of the assignment. It installs
Linux and a C compiler. It uses Ubuntu under WSL 2 on Windows 11. The last
section lists the commands to record as evidence.

---

## Why WSL 2 counts as a Linux installation

**WSL 1** translated Linux system calls into Windows calls. It was a
compatibility layer. POSIX timers and the job control signals were
incomplete.

**WSL 2 runs a real, unmodified Linux kernel** in a light virtual machine. It
has a real init, a real `/proc`, real cgroups and real job control. On this
machine:

```
$ uname -srm
Linux 6.18.33.2-microsoft-standard-WSL2 x86_64
```

That is a real Linux 6.18 kernel. Each mechanism that this project needs is
therefore genuine and not an emulation.

| Requirement of the project | Source under WSL 2 |
|---|---|
| `timer_create(CLOCK_MONOTONIC)` | POSIX timers in the Linux kernel that WSL 2 runs |
| `SIGSTOP` and `SIGCONT` | Real job control, enforced by that kernel |
| POSIX real-time signals | That kernel queues `SIGRTMIN` to `SIGRTMAX` |
| `CLOCK_PROCESS_CPUTIME_ID` | Per-process CPU clocks from the real scheduler |
| `/proc/PID/stat` | A real procfs, which the `--verify` check reads |
| `getrusage()` through `wait4()` | Real resource accounting |

The project checks itself against `/proc` and `getrusage()` because the kernel
below it is real. On WSL 1, or on a translation layer such as Cygwin, those
checks would give no useful result.

A full virtual machine, such as VirtualBox, VMware or Hyper-V, works in the
same way. A direct installation on hardware also works. No part of the code
depends on WSL. Only `scripts/setup_wsl.sh` detects the difference, and it
prints one line about it.

---

## Part 1. Install WSL 2 and Ubuntu

### Requirements

- Windows 11, or Windows 10 version 2004 (build 19041) or later
- Hardware virtualization enabled in the firmware
- About 2 GB of free disk space

### Step 1. Install WSL and Ubuntu

1. Open **PowerShell as Administrator**.
2. Run this command.

   ```powershell
   wsl --install -d Ubuntu
   ```

   The command enables the two Windows features that WSL needs, which are
   Virtual Machine Platform and Windows Subsystem for Linux. It then
   downloads the WSL 2 kernel and installs the current Ubuntu LTS release.
3. Restart the computer when Windows asks. The feature changes need a
   restart.

If WSL is already present but runs version 1, convert it.

```powershell
wsl --set-default-version 2
wsl --set-version Ubuntu-24.04 2
```

### Step 2. Create the UNIX account

Ubuntu starts by itself after the installation. It asks for a user name and a
password. This account is separate from the Windows account. `sudo` asks for
this password. The terminal does not show the password as you type it.

### Step 3. Confirm the WSL version

1. Return to PowerShell.
2. Run this command.

   ```powershell
   wsl -l -v
   ```

   ```
     NAME              STATE           VERSION
   * Ubuntu-24.04      Stopped         2
   ```
3. Check that the `VERSION` column shows 2. If it shows 1, run the
   `--set-version` command above. The project builds on WSL 1, but the check
   against `/proc` does not give correct results there.

### Step 4. Update the system

Run this command in the Ubuntu shell.

```bash
sudo apt update && sudo apt upgrade -y
```

---

## Part 2. Install the C toolchain

1. Install the compiler and the build tools.

   ```bash
   sudo apt install -y build-essential
   ```

   `build-essential` is a meta-package. It installs `gcc`, `g++`, `make`, the
   C library headers in `libc6-dev`, and the linker.
2. Install `strace`. This step is optional. The demonstration uses `strace`
   to show the system calls.

   ```bash
   sudo apt install -y strace
   ```

### Confirm the toolchain

```bash
gcc --version
make --version
```

```
gcc (Ubuntu 13.3.0-6ubuntu2~24.04.1) 13.3.0
GNU Make 4.3
```

Check that the headers are present.

```bash
ls /usr/include/signal.h /usr/include/time.h /usr/include/sys/wait.h
```

---

## Part 3. Build and run the project

Windows drives appear under `/mnt` inside WSL.

```bash
cd "/mnt/c/Users/thoma/Documents/Project OS"

./scripts/setup_wsl.sh    # checks the toolchain, offers to install what is missing
make                      # build
make check                # 38 tests
make run                  # a live run
```

`scripts/setup_wsl.sh` performs the checks in this document with one command.
It reports the distribution, the kernel and the compiler. It lists each
missing package and asks before it installs anything. It then builds the
project and runs a short job to show that the timer works.

### A note about a build on /mnt/c

The project builds and runs correctly from the Windows filesystem. The files
stay visible to Windows, which is convenient. Two effects are worth knowing.

- A build on `/mnt/c` is slower than a build on the Linux filesystem, because
  each file operation crosses the 9P protocol boundary. The difference is
  small for a project of this size.
- `make` can report `Clock skew detected` when the Windows clock and the WSL
  clock differ. This usually follows a sleep. The message is harmless. To
  remove it:

  ```bash
  sudo hwclock -s     # set the WSL clock from the hardware clock
  make clean && make
  ```

To build on the Linux filesystem instead:

```bash
cp -r "/mnt/c/Users/thoma/Documents/Project OS" ~/tsim && cd ~/tsim && make
```

---

## Part 4. Evidence to record for the video

The installation carries 30% of the mark, so record it. These seven steps need
about one minute and cover each claim.

1. Show the WSL version. Run this command in PowerShell.

   ```bash
   wsl -l -v
   ```
2. Show the distribution and the kernel.

   ```bash
   lsb_release -a
   uname -srm
   ```
3. Show that the kernel is a real Linux kernel with a real procfs.

   ```bash
   ls /proc/self/ | head
   cat /proc/version
   ```
4. Show the compiler and the build tools.

   ```bash
   gcc --version
   make --version
   ```
5. Build the project with every warning option enabled.

   ```bash
   make clean && make
   ```
6. Show that the real-time signals exist.

   ```bash
   ./tsim --version
   getconf _POSIX_RTSIG_MAX
   ```
7. Run the tests and the `/proc` check.

   ```bash
   make check
   ./tsim --verify --no-color
   ```

Step 5 must produce no warning. The build uses these options:

```
-Wall -Wextra -Wpedantic -Wshadow -Wstrict-prototypes
-Wmissing-prototypes -Wold-style-definition
```

Step 7 is the most important step. `make check` runs 38 tests. `./tsim
--verify` ends with the line below. The line shows that the single-CPU rule
held for each tick of the run, measured against `/proc` and not against the
records of the program.

```
EXCLUSIVITY CHECK   77 tick(s) sampled from /proc, 0 violation(s)
```

---

## Problems and solutions

| Symptom | Cause and solution |
|---|---|
| `wsl --install` reports a virtualization error | Hardware virtualization is off. Enable VT-x, AMD-V or SVM in the BIOS or UEFI setup |
| `wsl -l -v` shows version 1 | Run `wsl --set-version Ubuntu-24.04 2`, then restart the distribution |
| `gcc: command not found` | Run `sudo apt install build-essential` |
| `fatal error: time.h: No such file` | The headers are absent. Run `sudo apt install libc6-dev` |
| `undefined reference to 'timer_create'` | The link line has no `-lrt`. The supplied Makefile includes it. Versions of glibc before 2.34 require it |
| `make: Clock skew detected` | The Windows clock and the WSL clock differ. Run `sudo hwclock -s`, then `make clean && make` |
| `Permission denied` on `./tsim` or a script | Run `chmod +x tsim scripts/*.sh tests/*.sh` |
| A script reports `$'\r': command not found` | The file has Windows line endings. Run `sed -i 's/\r$//' scripts/*.sh tests/*.sh` |
| `tsim` appears to stop responding | The program has a watchdog, so this should not occur. Ctrl-C stops it and still prints the report. Add `-T 10` to print the process table every 10 ticks |

---

## The tested environment

| Item | Version |
|---|---|
| Host | Windows 11 Home 10.0.26200 |
| Subsystem | WSL 2 |
| Distribution | Ubuntu 24.04.5 LTS (Noble Numbat) |
| Kernel | Linux 6.18.33.2-microsoft-standard-WSL2 x86_64 |
| Compiler | gcc 13.3.0 |
| C library | glibc 2.39 (NPTL 2.39) |
| Build tool | GNU Make 4.3 |
| Result | 24 regression tests, 14 edge cases and the stress suite all pass, with no compiler warning |

The code targets **Ubuntu 20.04 LTS and newer**. One detail depends on the
version. `timer_create()` was part of `librt` in versions of glibc before
2.34, and Ubuntu 20.04 ships glibc 2.31. Later releases moved the function
into the main C library and still supply an empty `librt` stub. The link line
`-lrt` therefore works on each supported release.
