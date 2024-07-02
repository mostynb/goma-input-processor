# Goma client

**Goma is not maintained any more, please use
[reclient](https://github.com/bazelbuild/reclient) instead.**

*Goma* is a distributed compiler service for open-source project such as
Chromium and Android. It's some kind of replacement of distcc+ccache.

NOTE: For non-Googler usage, please see
[Goma for Chromium Contributors](doc/early-access-guide.md).

Google employees interested in contributing to the goma client should use
internal version. see http://go/ma-client-code

[TOC]

## How Goma works

Goma hooks a compile request, and sends it to a backend compile server.
If you have plenty of backend servers, a lot of compile can be processed in
parallel, for example, -j100, -j500 or -j1000.

Also, the Goma backend caches the compile result. If the same compile request
comes, the cached result is returned from the Goma cache server.

## How to build

Goma client can be built on Linux, Mac, and Win.

### Prerequisite

1. Use 64bit platform (Linux, Mac or Win).
1. Install [depot\_tools](http://commondatastorage.googleapis.com/chrome-infra-docs/flat/depot_tools/docs/html/depot_tools_tutorial.html#_setting_up).
1. Install dependencies.
   * On Mac, install Xcode.
   * On Windows, install Visual Studio 2017. Community edition is OK.


### Checkout source


```shell
$ mkdir goma && cd goma
$ gclient config https://chromium.googlesource.com/infra/goma/client
$ gclient sync
$ cd client
```

We assume the Goma client code is checked out to `${GOMA_SRC}`. You can set this
in your environment, but do not `export` it as it will make `gomacc` complain.

If you want to develop goma client, make goma client source unmanaged by
gclient. Open `.gclient` file, and check `"managed"` value.
If it's `True`, changed it to `False`. Otherwise, gclient will manage
your repository, so your checkout can be unintentionally changed with
`gclient sync`.

Move to `client` directory (which is under git repo),
and configure git repository with your username and emails.

```shell
$ cd client
$ git config user.email 'foo@example.com'
$ git config user.name 'Your Name'
```

### Build

```shell
$ cd "${GOMA_SRC}/client"
$ gclient sync
$ gn gen --args="is_debug=false" out/Release
$ ninja -C out/Release
```

#### Several important gn args

The build option can be modified with gn args.

```
is_debug=true/false
  Do debug build if true.
dcheck_always_on=true/false
  Enable DCHECK always (even in release build).
is_asan=true/false
  Use ASan build (with clang).
use_link_time_optimization=true/false
  Currently working only on Win. If true, /LTCG is enable.
use_lld=true/false
  Use lld for link (it will be fast)
```

### Run unittest

```shell
$ cd "${GOMA_SRC}/client"
$ ./build/run_unittest.py --target=Release --build-dir=out
```

### Coding style

Follow Google code style.


- [C++](https://google.github.io/styleguide/cppguide.html)
- [Python](https://github.com/google/styleguide/blob/gh-pages/pyguide.md)

For C++11/14 features, we prefer to follow [Chromium's
guidelines](https://chromium.googlesource.com/chromium/src/+/main/styleguide/c++/c++11.md).


## How to use

### For Chromium development

Goma can be integrated with Chromium development easily.

1. Build goma client
2. Start compiler\_proxy

```
$ "${GOMA_SRC}/client/out/Release/goma_ctl.py" start
```

#### For Chromium

In Chromium src, specify the following args in `gn args`

```
use_goma = true
goma_dir = "${GOMA_SRC}/client/out/Release"  (Replace ${GOMA_SRC} to your checkout)
```

Then build like the following:

```
$ cd /path/to/chromium/src/out/Release
$ ninja -j100 chrome
```

More details are avairable in chromium's build instructions.
* [docs/linux/build\_instructions.md](https://chromium.googlesource.com/chromium/src/+/refs/heads/main/docs/linux/build_instructions.md)
* [docs/windows\_build\_instructions.md](https://chromium.googlesource.com/chromium/src/+/refs/heads/main/docs/windows_build_instructions.md)
* [docs/mac\_build\_instructions.md](https://chromium.googlesource.com/chromium/src/+/refs/heads/main/docs/mac_build_instructions.md)
* [other build instructions](https://chromium.googlesource.com/chromium/src/+/refs/heads/main/docs/README.md#checking-out-and-building)

### For general development

1. Build Goma client
2. Start `compiler_proxy`

```shell
$ ./goma_ctl.py ensure_start
```

3. Change your build script so that `gomacc` is prepended to compiler command.
   For example:

```shell
$ gomacc clang++ -c foo.cc
```

4. Build your product with `make -j100`, `ninja -j100` or larger -j.
   Check http://localhost:8088 to see compiler\_proxy is actually working.

* You can use [autoninja](https://chromium.googlesource.com/chromium/tools/depot_tools.git/+/main/autoninja) in `depot_tools` instead of specifying gomacc manually.


