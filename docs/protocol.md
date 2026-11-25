# Protocol

A frame is an 8-byte header in network byte order followed by a payload.

| offset | type | meaning |
| --- | --- | --- |
| 0 | u32 | payload length, not including the header |
| 4 | u16 | type |
| 6 | u16 | flags |
| 8 | bytes | payload |

Types: `Data=1`, `Ping=2`, `Pong=3`, `Error=4`, `Close=5`. Flags are not interpreted by the library. Ping is not answered automatically. An echo server that sends the same type and payload back will echo pings as pings.

The decoder in `src/protocol/codec.hpp` copies bytes. It does not cast the buffer to a struct. If the length is greater than `max_frame_size`, it returns `FrameTooLarge` before allocating the payload. An unknown type is `BadType`. An empty payload is valid.

`MessageView` points at the decoder's payload vector and is valid only until `on_message` returns. The next frame reuses that storage.

`examples/pubsub` and `examples/kv` put their commands in the payload as text (`SUB`, `PUB`, `GET`, `SET`, ...). That text is not part of the frame header.
