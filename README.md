# Concurrency Kit for Lua

[![13.5-STABLE Build Status](https://api.cirrus-ci.com/github/ryan-moeller/flua-ck.svg?branch=main&task=snapshots/amd64/13.5-STABLE)](https://cirrus-ci.com/github/ryan-moeller/flua-ck)
[![14.3-STABLE Build Status](https://api.cirrus-ci.com/github/ryan-moeller/flua-ck.svg?branch=main&task=snapshots/amd64/14.3-STABLE)](https://cirrus-ci.com/github/ryan-moeller/flua-ck)
[![15.0-STABLE Build Status](https://api.cirrus-ci.com/github/ryan-moeller/flua-ck.svg?branch=main&task=snapshots/amd64/15.0-STABLE)](https://cirrus-ci.com/github/ryan-moeller/flua-ck)
[![13.5-RELEASE Build Status](https://api.cirrus-ci.com/github/ryan-moeller/flua-ck.svg?branch=main&task=releases/amd64/13.5-RELEASE)](https://cirrus-ci.com/github/ryan-moeller/flua-ck)
[![14.3-RELEASE Build Status](https://api.cirrus-ci.com/github/ryan-moeller/flua-ck.svg?branch=main&task=releases/amd64/14.3-RELEASE)](https://cirrus-ci.com/github/ryan-moeller/flua-ck)
[![15.0-RC2 Build Status](https://api.cirrus-ci.com/github/ryan-moeller/flua-ck.svg?branch=main&task=releases/amd64/15.0-RC2)](https://cirrus-ci.com/github/ryan-moeller/flua-ck)

## Dependencies

While FreeBSD does vendor a copy of Concurrency Kit for use in the kernel, it is
stripped of headers required for use in userland.  Therefore, the concurrencykit
package is required:

```
# pkg install -y concurrencykit
```

## Build Example

The build system is set to compile the module for FreeBSD's base system Lua
interpreter (flua):

```
$ make
# make install # optional
```

The sources themselves should not require anything flua-specific, so building
for another Lua environment may be possible with some adjustments to the
Makefile.

## TODO

- improve tests and samples
- improve README
- implement missing CK features
