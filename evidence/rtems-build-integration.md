# Evidence: RTEMS build-system integration with the toolchain

## cpukit/optgcc.yml — how RTEMS drives GCC
```yaml
SPDX-License-Identifier: CC-BY-SA-4.0 OR BSD-2-Clause
actions:
- set-value: ${PROGRAM_PREFIX}gcc
- substitute: null
- find-program: null
- env-assign: AS
- env-assign: CC
- env-assign: LINK_CC
- set-value: ${PROGRAM_PREFIX}g++
- substitute: null
- find-program: null
- env-assign: CXX
- env-assign: LINK_CXX
- set-value: ${PROGRAM_PREFIX}ar
- substitute: null
- find-program: null
- env-assign: AR
- set-value: ${PROGRAM_PREFIX}ld
- substitute: null
- find-program: null
- env-assign: LD
- script: |
    from waflib import Context

    conf.load("ar g++ gas gcc gccdeps")
```

## the GCC build-key check in the same file
```python
    # Obtain the GCC build key
    out = conf.cmd_and_log([conf.env.CC[0], "--version"],
                           output=Context.STDOUT,
                           quiet=Context.BOTH)
    mobj = re.search(r"Build (\d{4}\.\d{2}.\d{2})", out)
    if mobj:
        key = mobj.group(1)
        msg = "is too old"
        conf.msg("GCC build key", key)
    else:
        key = "0000.00.00"
        msg = "is not present"
        conf.msg("GCC build key", "not present")

    # Check the GCC build key
    required_key = "2026.06.12"
```

## cpukit/optclang.yml — the clang counterpart
```yaml
SPDX-License-Identifier: CC-BY-SA-4.0 OR BSD-2-Clause
actions:
- set-value: clang
- find-program: null
- env-assign: AS
- env-assign: CC
- env-assign: LINK_CC
- set-value: clang++
- find-program: null
- env-assign: CXX
- env-assign: LINK_CXX
- set-value: llvm-ar
- substitute: null
- find-program: null
- env-assign: AR
- set-value: clang
- env-assign: AS
- env-assign: ASM_NAME
- set-value:
  - -c
  - -o
- env-assign: AS_TGT_F
- set-value:
  - -o
- env-assign: ASLNK_TGT_F
- set-value: --target=${ARCH}${ARCH_BITS}-unknown-rtems${__RTEMS_MAJOR__}
- substitute: null
- env-append: ABI_FLAGS
- script: |
    conf.load("ar asm clang clang++ gccdeps")
build-type: option
copyrights:
- Copyright (C) 2020 Hesham Almatary <Hesham.Almatary@cl.cam.ac.uk>
- Copyright (C) 2020 embedded brains GmbH & Co. KG
default: []
description: ''
enabled-by:
- clang
links: []
name: clang
type: build
```

## rsb rtems/config/rtems-gcc-message.binc — the version stamp
```
%ifn %{defined gcc_build_date}
%error No GCC build date defined.
%endif
%define gcc_version_message \
    RTEMS %{rtems_version}, Build %{gcc_build_date}, RSB %{_sbgit_id}, Newlib %{newlib_version}
```
