# OpenSDG (vendored)

Client implementation of the SecureDeviceGrid protocol, used to talk to Danfoss DEVIreg Smart
thermostats. Upstream: https://github.com/Sonic-Amiga/opensdg (GPL-3.0, see LICENSE.txt),
commit dd564d6 ("Bump version to 1.0.3"). Only `library/` and `include/` are kept; the Java,
JNI, test app and Debian packaging are left out.

`library/control_protocol.pb-c.{c,h}` are generated once from `control_protocol.proto` with
protoc-gen-c (protobuf-c 1.5.x) and committed, so building heatbrain needs only the protobuf-c
runtime library. Regenerate them if the .proto changes.

Upstream's legal note: SecureDeviceGrid is a proprietary protocol by Trifork; this library is
for personal, non-commercial use. heatbrain uses it to control the user's own thermostat.

One local change: `library/mainloop.h` declares `main_cb` as `extern` (upstream defines it in the
header, which GCC 10+ rejects as a multiple definition; it is defined once in `mainloop_events.c`).
