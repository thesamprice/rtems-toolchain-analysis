# What RTEMS already has for Clang

This section is from my own inspection and testing of the RTEMS tree and a current Clang, not
from documentation. Commands and outputs are in [`evidence/`](evidence/).

## RTEMS's build system already speaks Clang

This surprised me. `wscript:51`:

```python
compilers = ["gcc", "clang"]
```

and there is a `--rtems-compiler` option. The mechanism is a pair of parallel option specs:

| | `spec/build/cpukit/optgcc.yml` | `spec/build/cpukit/optclang.yml` |
|---|---|---|
| CC | `${PROGRAM_PREFIX}gcc` | `clang` |
| CXX | `${PROGRAM_PREFIX}g++` | `clang++` |
| AR | `${PROGRAM_PREFIX}ar` | `llvm-ar` |
| target selection | via the program prefix (`microblaze-rtems7-`) | `--target=${ARCH}${ARCH_BITS}-unknown-rtems${__RTEMS_MAJOR__}` appended to `ABI_FLAGS` |
| linker flags | **`-qrtems`** appended to `LDFLAGS` *and* `PKGCONFIG_LDFLAGS` | *nothing* |
| toolchain check | yes — see below | none |

Both are `enabled-by: gcc` / `enabled-by: clang`, selected by the compiler option.

Authorship is Hesham Almatary (Cambridge) and embedded brains, **2020** — this came out of CHERI
work, and it has not obviously been maintained since.

### Coverage is narrow

Only two BSP families reference the clang specs:

```
  riscv/{noel,griscv,esp32,mbv,riscv,niosv}/grp.yml
  sparc/leon3/{grp.yml,abiclang.yml}
```

RTEMS 7 has **13 architectures and 202 BSP variants**. Clang scaffolding exists for two
architecture families.

`sparc/leon3/abiclang.yml` is a full parallel ABI-flags file — `-mcpu=leon3`, `-mfix-ut700`,
`-mfix-ut699`, `-mcpu=gr712rc`, `-mcpu=gr740` — mirroring the GCC one, because the flag spellings
differ between compilers.

### `-qrtems` is the missing piece

`optgcc.yml` appends `-qrtems` to the link flags. That single GCC spec (see
[02](02-rtems-in-the-gnu-toolchain.md)) pulls in `crti/crtbegin/crtend/crtn`, `-T linkcmds`, and
the `--start-group -lrtemsbsp -lrtemscpu -latomic -lc -lgcc --end-group` group.

**Clang has no `-qrtems`.** `optclang.yml` compensates only partially — `spec/build/bsps/optclang.yml`
sets `LINKCMDS_START_DIRECTIVE` and `LINKCMDS_ALIGN_DIRECTIVE` so the linker command files can be
generated differently, but the start files and the library group have to come from somewhere else.

This is the concrete shape of the driver gap: **replicating `-qrtems` is the single most
load-bearing piece of work** on the LLVM side.

### RTEMS enforces that you used its GCC

`optgcc.yml` contains a check with no Clang counterpart:

```python
out = conf.cmd_and_log([conf.env.CC[0], "--version"], ...)
mobj = re.search(r"Build (\d{4}\.\d{2}.\d{2})", out)
...
required_key = "2026.06.12"
if key < required_key:
    policy = conf.env["GCC_BUILD_KEY_POLICY"]
    if policy == "ERROR":
        conf.fatal(f"{msg}: {required_key}")
```

The string it parses is stamped by the RSB — `rtems/config/rtems-gcc-message.binc`:

```
%define gcc_version_message \
    RTEMS %{rtems_version}, Build %{gcc_build_date}, RSB %{_sbgit_id}, Newlib %{newlib_version}
```

which is why the installed compiler reports:

```
microblaze-rtems7-gcc (GCC) 12.4.1 20240905 (RTEMS 7, Build 2026.06.12,
                            RSB 105f43d..., Newlib 7d4336cf)
```

So RTEMS does not merely *prefer* an RSB-built GCC — the kernel build **refuses to proceed**
with a toolchain older than a pinned date, unless you set `GCC_BUILD_KEY_POLICY`. A Clang build
bypasses this entirely, which means the clang path has no equivalent guarantee that the C
library and kernel headers it compiles against match.

## What Clang does with an RTEMS triple today

Tested with clang 23.0.0git (`llvm-project` `0594c0187`) on macOS/arm64.

**`__rtems__` is already predefined** — this is *not* a gap:

```
$ microblaze-rtems7-gcc -dM -E - </dev/null | grep rtems
#define __rtems__ 1

$ clang --target=arm-unknown-rtems7 -dM -E - </dev/null | grep rtems
#define __rtems__ 1
```

**But the link step falls through to the host compiler:**

```
$ clang --target=arm-unknown-rtems7 -### hello.c    # last line
 "/usr/bin/gcc" "-o" "a.out" "/var/folders/.../rt-53be83.o"
```

That is `/usr/bin/gcc` — the **macOS host** compiler — being handed an ARM object. There is no
RTEMS ToolChain in the Clang driver, so an RTEMS triple lands on a generic fallback that
delegates linking to whatever `gcc` is on `PATH`.

Compilation also warns:

```
clang: warning: unknown platform, assuming -mfloat-abi=soft
```

— the ARM driver has no idea what OS this is.

## Prior art

Joel Sherrill proposed initial `*-rtems*` support on the LLVM list in **June 2011**; Doug Gregor
committed it as LLVM r134282 and Clang r134283 on 1 July 2011. The approach was modelled on
FreeBSD's multi-architecture handling, plus a change to stop `/usr/include` being added to the
search path because "RTEMS tools are never self-hosted". Three days later Sherrill reported that
`__rtems__` was not being defined.

Fifteen years on, what survives is an enum value and a preprocessor define. There is a
long-standing RTEMS ticket (#3930) for RISC-V Clang/LLVM support, blocked on the RSB not being
able to build LLVM's dependencies.

## Reading

The RTEMS side is further along than the LLVM side. The build system can already select Clang,
pass a correct triple, and use `llvm-ar`; two architecture families have ABI flag sets. What is
missing on the RTEMS side is coverage (2 of 13 architectures) and the `-qrtems` equivalent.
What is missing on the LLVM side is the driver itself.
