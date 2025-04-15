## What is StormWeaver ?

TODO new proper readme as everything completely changed

For now look at `scenarios/basic.lua` for an example, and just execut `bin/stormweaver scenarios/basic.lua`

## Getting StormWeaver

The easiest way to get StormWeaver is to download the tgz from the Releases page.

The build has no external dependencies or setup requirement, it works immediately after extracting.

Just make sure to download the binary for the correct glibc version. To check the version, execute for example:

```
ldd --version
```

## How to build StormWeaver ?

* Install and setup conan2, ninja-build and a recent C++ compiler with C++20 support
* Download/checkout StormWeaver and change the working directory to it
* Run Conan to build the dependencies and StormWeaver:
  ```
  conan build . --build=missing --settings=build_type=<Debug/Release>
  ```
  This automatically builds the StormWeaver executables in the bin directory␍

The used libraries, including the SQL client libraries are statically linked, the binary itself is enough to run StormWeaver.␍
␍
## Initial conan2 setup␍

To use conan2, first install it using the instructions at https://docs.conan.io/2/installation.html␍
␍
And then create a default conan2 profile, using conan profile detect␍
␍
The default profile usually selects an older C++ standard, which has to be changed by editing `~/.conan2/profiles/default` and setting the `compiler.cppstd` to `20`.␍
␍
## How to rebuild/test (for developers)␍

After the initial build, the project can be rebuilt by either:␍
␍
* Executing `cmake --build --target install --preset conan-debug` (or `conan-release`) in the StormWeaver directory␍
* Going to the `build/<Debug/Release>` directory, and executing `ninja install` or `cmake --build . --target install`

## How to build with sanitizers

Sanitizers are options to the `conan build` command, for example:

```
conan build . --build=missing --settings=build_type=<Debug/Release> -o '&:asan=True' -o '&:ubsan=True'
```

builds StormWeaver with address and undefined behavior sanitizers.

All sanitizer options:

* asan
* ubsan
* tsan
* msan

Note: `tsan` and `asan` can't be used together, and `tsan` shows false positives because of missing suppressions, that's a TODO.
