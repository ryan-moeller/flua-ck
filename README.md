# Concurrency Kit for Lua

[![13.5-STABLE Build Status](https://api.cirrus-ci.com/github/ryan-moeller/flua-ck.svg?branch=main&task=snapshots/amd64/13.5-STABLE)](https://cirrus-ci.com/github/ryan-moeller/flua-ck)
[![14.3-STABLE Build Status](https://api.cirrus-ci.com/github/ryan-moeller/flua-ck.svg?branch=main&task=snapshots/amd64/14.3-STABLE)](https://cirrus-ci.com/github/ryan-moeller/flua-ck)
[![15.0-STABLE Build Status](https://api.cirrus-ci.com/github/ryan-moeller/flua-ck.svg?branch=main&task=snapshots/amd64/15.0-STABLE)](https://cirrus-ci.com/github/ryan-moeller/flua-ck)
[![13.5-RELEASE Build Status](https://api.cirrus-ci.com/github/ryan-moeller/flua-ck.svg?branch=main&task=releases/amd64/13.5-RELEASE)](https://cirrus-ci.com/github/ryan-moeller/flua-ck)
[![14.3-RELEASE Build Status](https://api.cirrus-ci.com/github/ryan-moeller/flua-ck.svg?branch=main&task=releases/amd64/14.3-RELEASE)](https://cirrus-ci.com/github/ryan-moeller/flua-ck)
[![15.0-RELEASE Build Status](https://api.cirrus-ci.com/github/ryan-moeller/flua-ck.svg?branch=main&task=releases/amd64/15.0-RELEASE)](https://cirrus-ci.com/github/ryan-moeller/flua-ck)

ck - Lua bindings for Concurrency Kit

## Description

The `ck` module is a collection of data structures built on top of Concurrency
Kit.  It provides an extensible serialization and deserialization (serde)
interface for sharing Lua values between separate states running on different
threads in the same process.

Several data structures are exposed for various use cases, such as constant
references, mutable references, queues, rings, etc.

## Dependencies

While FreeBSD does vendor a copy of Concurrency Kit for use in the kernel, it is
stripped of headers required for use in userland.  Therefore, the concurrencykit
package is required:

```
# pkg install -y concurrencykit
```

As Lua is a single-threaded environment out of the box, some means of spawning
threads with new Lua states and passing lightuserdata values between states must
be provided.  This module was designed with the pthread module from [flualibs][]
in mind but does not depend on it.

[flualibs]: https://github.com/ryan-moeller/flualibs

The build system compiles the module for FreeBSD's base system Lua interpreter
(flua).  It requires the FreeBSD sources in /usr/src or at a path specified by
SRCTOP in the build environment.

The sources themselves should not require anything flua-specific, so building
for another Lua environment may be possible with some adjustments to the
Makefile.

## Build Example

Building on FreeBSD with all dependencies in place is as simple as running make:

```
$ make
# make install # optional
```

## TODO

- improve tests and samples
- improve README
- EXAMPLES in manpages
- implement missing CK features
