# Evidence: what clang does with an RTEMS triple today

clang 23.0.0git (llvm-project 0594c0187), macOS/arm64

## __rtems__ is predefined by BOTH compilers
```
$ microblaze-rtems7-gcc -dM -E - </dev/null | grep rtems
#define __rtems__ 1

$ clang --target=arm-unknown-rtems7 -dM -E - </dev/null | grep rtems
#define __rtems__ 1
```

## but the driver has no RTEMS ToolChain, so the link falls through to the HOST compiler
```
$ clang --target=arm-unknown-rtems7 -### hello.c   (last line)
 "/usr/bin/gcc" "-o" "a.out" "$TMPDIR/rt-53be83.o"
```
