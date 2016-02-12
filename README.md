Socketless HTTP 2.

### Why.

Mostly because libuv, and therefore libh2o, tends to segfault a lot when used
from Python. Also, because why not?

### C API

`make obj/libcno.a`

Just read core.h. Basically, you create a `cno_connection_t`, then follow a simple
chain of `cno_connection_init` -> connect some callbacks -> `cno_connection_made` ->
`cno_connection_data_received` -> (repeat while I/O is still possible) ->
`cno_connection_lost` -> `cno_connection_reset`, skipping to the last step if
anything returns an error and using `cno_write_message` + `cno_write_data` or
`cno_write_push` or `cno_write_reset` to send some stuff of your own.

Oh, and here's one thing missing from core.h. You'll need it.

```c
struct cno_buffer_t { char *data; size_t size; };
struct cno_buffer_t CNO_BUFFER_EMPTY;
struct cno_buffer_t CNO_BUFFER_STRING(char *str /* null-terminated */);
```

### Python API

```bash
pip3 install cffi
pip3 install git+https://github.com/pyos/libcno
```

| C constant   | Python constant      |
| ------------ | -------------------- |
| `CNO_CLIENT` | `cno.raw.CNO_CLIENT` |
| etc.         | ...                  |

`cno.raw.Connection` is an almost complete 1:1 mapping to C API functions.

| C function                                       | `cno.raw.Connection` method                                   |
| ------------------------------------------------ | ------------------------------------------------------------- |
| `cno_connection_init(c, CNO_SERVER)`             | `c = cno.raw.Connection(server=True)`                         |
| `cno_connection_reset(c)`                        | `del c`                                                       |
| `cno_connection_made(c, CNO_HTTP2)`              | `c.connection_made(http2=True)`                               |
| `cno_connection_lost(c)`                         | `c.connection_lost()`                                         |
| `cno_connection_data_received(c, data, length)`  | `c.data_received(data)`                                       |
| `cno_connection_is_http2(c)`                     | `c.is_http2`                                                  |
| `cno_stream_next_id(c)`                          | `c.next_stream`                                               |
| `cno_write_reset(c, stream, code)`               | `c.write_reset(stream, code)`                                 |
| `cno_write_push(c, stream, msg)`                 | `c.write_push(stream, method, path, headers)`                 |
| `cno_write_message(c, stream, msg, final)`       | `c.write_message(stream, code, method, path, headers, final)` |
| `cno_write_data(c, stream, data, length, final)` | `c.write_data(stream, data, final)`                           |

Event receivers must be defined as methods of `Connection` subclasses.

| C event                                 | `cno.raw.Connection` method                                        |
| --------------------------------------- | ------------------------------------------------------------------ |
| `on_write(data, length)`                | `def on_write(self, data)`                                         |
| `on_stream_start(stream)`               | `def on_stream_start(self, stream)`                                |
| `on_stream_end(stream)`                 | `def on_stream_end(self, stream)`                                  |
| `on_flow_increase(stream)`              | `def on_flow_increase(self, stream)`                               |
| `on_message_start(stream, msg)`         | `def on_message_start(self, stream, code, method, path, headers)`  |
| `on_message_trail(stream, msg)`         | `def on_message_trail(self, stream, trailers)`                     |
| `on_message_data(stream, data, length)` | `def on_message_data(self, stream, data)`                          |
| `on_message_push(stream, msg, parent)`  | `def on_message_push(self, stream, parent, method, path, headers)` |
| `on_message_end(stream)`                | `def on_message_end(self, stream)`                                 |


On Python 3.5+, higher-level asyncio bindings are also available. Server:

```python
async def handle(request: cno.Request):
    request.method   # :: str
    request.path     # :: str
    request.headers  # :: [(str, str)]
    request.conn     # :: cno.Server -- `protocol` (below)
    request.payload  # :: asyncio.StreamReader

    # Pushed resources inherit :authority and :scheme from the request unless overriden.
    request.push('GET', '/index.css', [('x-extra-header', 'value')])

    if all_data_is_available:
        await request.respond(200, [('content-length', '4')], b'!!!\n')
    else:
        # `Channel` is a subclass of `asyncio.Queue`.
        channel = cno.Channel(max_buffered_chunks, loop=request.conn.loop)
        await channel.put(b'!!!')  # this should preferably be done in a separate
        await channel.put(b'\n')   # coroutine, naturally.
        channel.close()
        await request.respond(200, [], channel)

protocol = cno.Server(event_loop, handle)
# server = await event_loop.create_server(lambda: protocol, '', 8000, ssl=...)
```

Client:

```python
client = await cno.connect(event_loop, 'https://example.com/')
client.loop       # :: asyncio.BaseEventLoop -- `event_loop`
client.transport  # :: asyncio.Transport
client.is_http2   # :: bool
client.scheme     # :: str  -- https
client.authority  # :: str  -- example.com

response = await client.request('GET', '/', [('user-agent', 'libcno/0.1')])  # cno.Response
response.code     # :: int
response.headers  # :: [(str, str)]
response.payload  # :: asyncio.StreamReader
async for push in response.pushed:
    push.method   # :: str
    push.path     # :: str
    push.headers  # :: [(str, str)]
    await push.response  # :: cno.Response
    # or push.cancel()

response = await client.request('POST', '/whatever', [], b'payload')
# `request`, like `respond`, also accepts `cno.Channel`s as payload.

client.transport.close()

# `cno.connect` automatically sets up a default SSL context and creates
# a TCP connection. To simply create an `asyncio.Protocol`:
client = cno.Client(event_loop, authority='example.com', scheme='https')

# A shorthand for `cno.connect` followed by `client.request`:
response = await cno.request(event_loop, 'GET', 'https://example.com/path', ...)
response.conn.transport.close()
```
