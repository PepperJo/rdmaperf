# Rdmaperf

A multi-client (m-to-1) RDMA benchmark tool to measure one-sided RDMA operation (read/write/fadd/cas) performance.
The server allocates and registers memory for clients to make one-sided RDMA operations to, for example
```
rdmaperf_server -s 16M -ip 10.0.0.1 -p 1001 -h
```
listens on ip/port 10.0.0.1:1001 and allocates 16MB of huge page memory for clients to do RDMA operations to. Other than
accepting client connections the server is completely passive (hence one-sided).

The client supports various access pattern to read/write/fadd/cas to/from the remote memory of the server, for example
```
rdmaperf_client -l 0 -tx 2 -cq_mod 2 -op read -t lat -ip 10.0.0.1 -p 1001 -d 10 -s 16 -a 64 -h
```
connects to server on 10.0.0.1:1001 and read at the 1st 16bytes of the 16MB of the server's remote memory.
The send queue depth is 2 and only every 2nd send operation is signaled, i.e. generates a completion. We indicate
it to be a latency experiment so every operation's completion time is measured individually (resp. cp_mod many) and
a sample set of times is stored to compute median or mean. In throughput mode operations per seconds are reported.
Stats are reported every second.
