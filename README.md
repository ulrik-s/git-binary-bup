# git-binary-bup

Using libgit2 ODB to store binaries in git BUP style.

## Building

This project uses CMake. A recent libgit2 development package is required.

```sh
mkdir build && cd build
cmake ..
make
```

## Testing

After building, run the test suite via CTest:

```sh
ctest
```
