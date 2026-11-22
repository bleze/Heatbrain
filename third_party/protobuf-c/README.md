# protobuf-c runtime

`protobuf-c/protobuf-c.c` and `protobuf-c/protobuf-c.h` from
[protobuf-c](https://github.com/protobuf-c/protobuf-c) v1.5.2, unmodified (BSD-2-Clause, see
`LICENSE`). Only the C runtime is needed: OpenSDG's `control_protocol.pb-c.c` is already
generated, so neither `protoc` nor Google's protobuf library is part of the build.
