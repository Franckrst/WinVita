# Quick start

## Clone

```bash
git clone git@gitlab.com:claude5564407/winx86.git
cd winx86
```

## Build for qemu-arm / Linux (dev, tests)

This is the default target — it produces an ARMv7 binary that runs under
`qemu-arm` on a Linux development machine, no Vita hardware needed. It's
the first tier of the three-level validation methodology (see d2vita's own
documentation): fast, good for checking correctness, but **its performance
numbers are never comparable to the console**.

```bash
./build.sh
# -> build-arm/libwinx86.a
```

## Build for real PS Vita hardware

```bash
TARGET=vita ./build.sh
# -> build-vita/libwinx86_vita.a
```

Requires [VitaSDK](https://vitasdk.org/) installed and on the `PATH`.

## Desktop compile check (portable, no ARM dynarec)

To quickly check that the portable slice (PE32 loader + import bridge)
compiles on your dev machine, with no need for an ARM cross-compiler:

```bash
cmake -B build
cmake --build build
```

This builds `libwinx86_core.a` and `pe_analyze` (the PE32 import-table
inspection tool).

## What you get, what you still have to write

winx86 alone runs nothing: there's no `main()`, no Win32 shim, no game
hook. After building `libwinx86.a`, the next step is to link your own boot
executable against this library and write the few hundred lines that load
your PE32, register your shims, and start the scheduler — see [Extension
point](extension.md).
