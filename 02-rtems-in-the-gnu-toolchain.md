# What RTEMS support actually consists of in GCC, binutils and newlib

Measured against GCC 15.2.0, binutils-gdb master, and newlib `7d4336cf`.

## The structural insight

**The toolchain contains no RTEMS code. It contains a set of promises about RTEMS.**

GCC's specs promise `-lrtemsbsp -lrtemscpu` exist and a `linkcmds` will be found.
`gthr-rtems.h` promises `_Mutex_Acquire` exists. newlib promises `__getreent` exists. Every one
of those promises is fulfilled by the RTEMS kernel tree, not by the toolchain.

`-qrtems` is the seam. Without it, GCC links a fake `crt0.o` full of stubs so autoconf link
probes succeed against a kernel that isn't there. With it, the real BSP takes over.

## The triple

`config.sub` contains exactly **one** RTEMS line — an entry in the accepted-OS list:

```
	     | cygwin* | msys* | pe* | moss* | proelf* | rtems* \
```

`rtems*` is a glob. No version is special-cased anywhere in GCC, binutils or newlib.
**Adding RTEMS 8 requires zero toolchain changes.** `config.guess` has zero RTEMS mentions —
RTEMS is only ever a target, never a build or host.

## binutils: essentially nothing

71 files mention rtems; **47 are ChangeLogs**, leaving 18. **Zero files have `rtems` in their
name.** No `te-rtems.h`, no RTEMS emulation, no target vector, no `ELFOSABI_RTEMS`.

54 non-ChangeLog lines total, of which:

- 6 are **rejections** (`*-*-rtemsaout*`, `hppa*-*-rtems*`, … → `exit 1`)
- 10 are testsuite exclusions
- 2 remove `libffi` and `libgloss` from `noconfigdirs`
- ~30 are live configuration

And of those ~30, **28 are literal `| foo-*-rtems*` alternatives glued onto an existing
generic-ELF `case` whose body is shared unchanged.** Exactly **two** produce output differing
from the plain-ELF triple, both for x86_64: two extra BFD selvecs (`x86_64_pe_vec`,
`x86_64_pei_vec`, so `objcopy` can emit PE for UEFI) and one extra GDB tdep object.

The line that summarises the whole relationship is `gas/configure.tgt:445`:

```
  *-*-elf | *-*-rtems* | *-*-sysv4*)	fmt=elf ;;
```

**From binutils' perspective RTEMS is a synonym for bare ELF.** That catch-all means an
architecture nobody has ever considered running RTEMS on still assembles correctly. Linker
scripts live in the RTEMS BSP, not in binutils — pulled in by `-T linkcmds%s` from GCC's spec.

## GCC: ~1,600 lines, of which ~300 are the OS

58 files have `rtems` in the name, totalling 5,332 lines — but 2,927 of those are the **Ada**
runtime, which is optional.

### The OS-level core — written once, ~300 lines

**`gcc/config.gcc:1018-1030`** — the entire OS contract, 13 lines:

```sh
*-*-rtems*)
  case ${enable_threads} in
    "" | yes | rtems) thread_file='rtems' ;;
    posix) thread_file='posix' ;;
    no) ;;
    *) echo 'Unknown thread configuration for RTEMS'; exit 1 ;;
  esac
  tmake_file="${tmake_file} t-rtems"
  extra_options="${extra_options} rtems.opt"
  default_use_cxa_atexit=yes
  use_gcc_stdint=wrap
  ;;
```

**`gcc/config/rtems.h`** — 64 lines, 24 of substance, and the most important file in the whole
integration:

```c
#undef STARTFILE_SPEC
#define STARTFILE_SPEC "%{!qrtems:crt0%O%s} %{qrtems:" RTEMS_STARTFILE_SPEC "}"

#undef ENDFILE_SPEC
#define ENDFILE_SPEC "%{qrtems:" RTEMS_ENDFILE_SPEC " %{!qnolinkcmds:-T linkcmds%s}}"

#undef LIB_SPEC
#define LIB_SPEC "%{!qrtems:" STD_LIB_SPEC "} " \
"%{qrtems:--start-group -lrtemsbsp -lrtemscpu -latomic -lc -lgcc --end-group}"

#define TARGET_POSIX_IO
#define TARGET_HAVE_LIBATOMIC true
#define OS_CC1_SPEC " %{!ftls-model=*:-ftls-model=local-exec}"
```

The `--start-group … --end-group` resolves the circular dependency between kernel and libc.

**`gcc/config/rtems.opt`** — declares `-qrtems`, `-qnolinkcmds`, `-pthread`. 15 substantive lines.

**`libgcc/config/gthr-rtems.h`** — 247 lines, 24 inline functions, each 1–3 lines. The design is
notable: **thread lifecycle and TLS go through pthreads; mutexes and condition variables bypass
pthreads entirely** and call RTEMS SuperCore self-contained objects (`_Mutex_Acquire`,
`_Condition_Wait`, …). `__gthread_active_p()` is a hard `return 1` — RTEMS is always threaded.

**`gcc/configure.ac:2092`** — `rtems` is a hardcoded member of a 12-element thread-model list.
This is the one place adding a new OS thread model touches GCC's core configure.

### Per-architecture — 35 to 80 lines each

The floor is about 30 lines. RISC-V's entire `rtems.h` body is seven:

```c
#undef TARGET_OS_CPP_BUILTINS
#define TARGET_OS_CPP_BUILTINS()		\
    do {					\
	builtin_define ("__rtems__");		\
	builtin_define ("__USE_INIT_FINI__");	\
	builtin_assert ("system=rtems");	\
    } while (0)
```

Plus a `config.gcc` stanza (~4–16 lines), a `libgcc/config.host` line, and a `t-rtems` multilib
fragment. **MicroBlaze's `t-rtems` is one line — a comment.** Multilibs are optional; the file
must exist only because `config.gcc` names it.

17 architecture headers, 28–305 lines. The 305-line outlier is PowerPC, and it is a PowerPC
biarch problem, not an RTEMS problem. `gcc/config/sh/rtems.h` is **dead code** — nothing
references it.

### The fragile part

`libstdc++-v3/configure.ac:390-417` hard-asserts POSIX facilities the cross-configure cannot
probe, with this comment:

```sh
        # These functions are defined in librtemscpu.  We don't use
        # -qrtems during configure, so we don't link that in, and fail
        # to find them.
        glibcxx_cv_chdir=yes
        glibcxx_cv_chmod=yes
        glibcxx_cv_mkdir=yes
```

That is the direct cost of the `-qrtems` design, and the TLS exclusion list beside it
(`bfin lm32 mips moxie or1k v850`) is hand-maintained. Similarly `libgcc/config/t-rtems`
reaches sideways into the newlib source tree:

```make
LIBGCC2_INCLUDES = -I$(srcdir)/../newlib/libc/sys/rtems/include
```

because `gthr-rtems.h` includes `<sys/lock.h>`, which only exists once newlib is present. This
bidirectional dependency is why the RSB builds GCC and newlib together.

### Optional runtime libraries

| | lines |
|---|---:|
| `libatomic/config/rtems/` | 78 |
| `libgomp/config/rtems/` | 730 — including `GOMP_RTEMS_THREAD_POOLS`, static pools because RTEMS cannot create threads at OpenMP-region entry in hard real time |
| `gcc/ada/` GNAT runtime | 2,927 |
| `libstdc++-v3` | ~40 lines of configure conditionals; **no dedicated directory**, uses `os/generic` |

## newlib: ~8,800 lines, but only 6 lines of C

`newlib/libc/sys/rtems/` is 54 files, 8,588 lines — and **~95% headers**. Only three objects
compile into `libc.a`, amounting to **6 lines of executable C** (`__cpuset_alloc`,
`__cpuset_free`, and an empty `not_required_by_rtems()`).

Roughly 5,000 of the header lines are an **imported FreeBSD networking ABI**, not RTEMS work.

`crt0.c` is 236 lines and **never runs**:

```c
/*  Each RTEMS BSP provides its own crt0 and linker script. ... So this fake
 *  crt0.c provides all the symbols required to successfully link a program.
 *  The resulting program will not run but this is enough to satisfy the
 *  autoconf macro AC_PROG_CC.  */
```

It stubs malloc, the `<sys/lock.h>` primitives, 13 libstdc++ gthread shims, ~31 POSIX syscalls,
and the 22-entry `_XYZ_r` reentrant family including `__getreent`.

The real intellectual content is about 500 lines:

- **`machine/_types.h`** (43 lines) — the type ABI: 64-bit `_off_t`, `__ino_t`, `__dev_t`
- **`sys/lock.h`** (410 lines) — the shared ABI that `gthr-rtems.h`, `libatomic/config/rtems/`
  and `libgomp/config/rtems/` all compile against
- **`machine/_threads.h`** (53 lines) — C11 threads mapped onto kernel objects
- **`machine/_libatomic.h`** (46 lines)

Plus ~180 lines of `#ifdef __rtems__` across ~20 generic headers — the largest being ~48 lines
in `sys/features.h` declaring POSIX conformance, and a **different `struct sigaction`** in
`sys/signal.h`.

### The whole kernel↔libc contract

`newlib/libc/include/sys/config.h:246-250` is the entire hook:

```c
#if defined(__rtems__)
#define __FILENAME_MAX__ 255
#define _READ_WRITE_RETURN_TYPE _ssize_t
#define __DYNAMIC_REENT__
#endif
```

`__DYNAMIC_REENT__` makes `_REENT` expand to `__getreent()`, which RTEMS implements in cpukit.
Combined with the `-D*_PROVIDED` flags in `configure.host`, the contract is: **one function
(`__getreent`), the `_XYZ_r` syscall family, malloc, exit, signal, and the `<sys/lock.h>`
self-contained objects.**

### libgloss: there is none

`find libgloss -iname '*rtems*'` → no matches. It is actively removed:

```sh
  *-*-rtems*)
    noconfigdirs="$noconfigdirs target-libgloss"
```

RTEMS BSPs supply their own crt0, linker script and console driver.

## Summary table

| Component | What's required | Size |
|---|---|---:|
| `config.sub` | one glob | **1 line** |
| binutils | ~30 lines of pattern-appends across 4 config files; zero source files | **~30 lines**, only 2 behavioural |
| GCC core configure | `rtems` in the thread-model whitelist | **1 token** |
| GCC OS stanza | `config.gcc` | **13 lines** |
| GCC shared OS header | `config/rtems.h` — the `-qrtems` machinery | **24 substantive** |
| GCC driver options | `config/rtems.opt` | **15 substantive** |
| GCC thread model | `gthr-rtems.h` + `gthr.m4` + `t-rtems` | **252 lines** |
| GCC per architecture | `<arch>/rtems.h`, `t-rtems`, config stanzas | **35–80 lines each** |
| newlib port dir | type + lock ABI, stub crt0, BSD headers | **8,588 lines** (~500 substantive) |
| newlib host config | `configure.host` | **9 lines** |
| libgloss | explicitly disabled | **0** |

**The architecture-independent OS work in GCC is ~300 lines, and it was written once.**
