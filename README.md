# NFS Sync Tool

> 🌐 This documentation is available in **English** and **Greek**.
> **English** | [Ελληνικά](README.el.md)

![C++17](https://img.shields.io/badge/C%2B%2B-17-blue.svg)
![Platform](https://img.shields.io/badge/platform-Linux-lightgrey.svg)
![License](https://img.shields.io/badge/license-Apache%202.0-green.svg)
![Tests](https://img.shields.io/badge/tests-68%20passing-brightgreen.svg)

Distributed file synchronization between remote directories over TCP.
A central **manager** with a pool of worker threads orchestrates `LIST → PULL → PUSH`
transfers between **clients** that expose local directories, while a **console**
provides live administration without restarts.

Written in C++17 using POSIX sockets, threads and condition variables. No external
dependencies, and no calls to `scp`, `rsync` or the shell.

> Assignment 2 – Systems Programming, Department of Informatics & Telecommunications,
> National and Kapodistrian University of Athens.

---

## Contents

- [Architecture](#architecture)
- [Building](#building)
- [Quick start](#quick-start)
- [Command line interface](#command-line-interface)
- [Configuration file](#configuration-file)
- [Console commands](#console-commands)
- [Wire protocol](#wire-protocol)
- [Logging](#logging)
- [Concurrency model](#concurrency-model)
- [Error handling](#error-handling)
- [Testing](#testing)
- [Project layout](#project-layout)
- [Enterprise usage](#enterprise-usage)
- [Limitations and non-goals](#limitations-and-non-goals)
- [Troubleshooting](#troubleshooting)
- [License](#license)

---

## Architecture

```
                       ┌──────────────────────────┐
   console commands    │        nfs_manager       │
   ──────────────────► │                          │
   add / cancel /      │  ┌────────────────────┐  │
   shutdown            │  │   BoundedBuffer    │  │   one task per file
                       │  │  producer/consumer │  │
                       │  │   + back-pressure  │  │
                       │  └─────────┬──────────┘  │
                       │            │             │
                       │   ┌────────▼────────┐    │
                       │   │ worker thread 1 │    │
                       │   │ worker thread 2 │    │
                       │   │      ...  N     │    │
                       │   └────┬───────┬────┘    │
                       └────────│───────│─────────┘
                        PULL    │       │   PUSH
                    ┌───────────▼─┐   ┌─▼───────────┐
                    │ nfs_client  │   │ nfs_client  │
                    │  (source)   │   │  (target)   │
                    │ /src_dir    │   │ /tgt_dir    │
                    └─────────────┘   └─────────────┘
```

Lifecycle of a transfer:

1. The manager's main thread reads the config file and sends `LIST` to every source client.
2. Each returned filename becomes a `SyncTask` pushed into the `BoundedBuffer`. When the
   buffer is full, the producer blocks (back-pressure).
3. Each worker thread takes a task, `PULL`s the file from the source client into memory,
   and `PUSH`es it to the target client in 4 KB chunks.
4. Every phase is logged in the manager log with a `SUCCESS` or `ERROR` result.
5. A dedicated thread listens for console commands, and each command is served on its
   own thread, so administrative actions never stall synchronization.

## Building

Requirements: `g++` with C++17 support, GNU Make, Linux (tested on Ubuntu 22.04 / WSL2).

```bash
make clean && make all
```

This produces three executables: `nfs_manager`, `nfs_client` and `nfs_console`.
The build uses `-std=c++17 -Wall -Wextra -pthread` and produces no warnings.

## Quick start

Four terminals on the same machine:

```bash
# 1. Setup
mkdir -p /tmp/demo/src /tmp/demo/tgt
echo "Hello from syspro2" > /tmp/demo/src/file1.txt
cat > /tmp/demo/nfs.cfg <<'EOF'
/tmp/demo/src@127.0.0.1:5556 /tmp/demo/tgt@127.0.0.1:5557
EOF

# 2. terminal A - client exposing the source directory
./nfs_client -p 5556 -l source_client.log

# 3. terminal B - client exposing the target directory
./nfs_client -p 5557 -l target_client.log

# 4. terminal C - manager: 5 workers, 7-slot buffer, console port 5555
./nfs_manager -l manager.log -c /tmp/demo/nfs.cfg -n 5 -p 5555 -b 7

# 5. terminal D - console
./nfs_console -l console.log -h 127.0.0.1 -p 5555
> add /tmp/demo/src2@127.0.0.1:5556 /tmp/demo/tgt2@127.0.0.1:5557
> cancel /tmp/demo/src
> shutdown
```

To run across multiple machines, only the hostnames in the config file and the
console's `-h` change.

## Command line interface

### `nfs_manager`

```
./nfs_manager -l <manager_logfile> -c <config_file> -n <worker_limit> -p <port_number> -b <bufferSize>
```

| Option | Description |
|---|---|
| `-l` | Manager log file. Created or truncated at startup. |
| `-c` | Configuration file listing the source → target pairs. |
| `-n` | Number of worker threads in the pool. |
| `-p` | Port on which the manager accepts connections from `nfs_console`. |
| `-b` | Capacity of the bounded buffer, in tasks (files). |

All options are required. A missing option or an invalid value exits with status `1`
and a usage message.

### `nfs_client`

```
./nfs_client -p <port_number> [-l <client_logfile>]
```

| Option | Description |
|---|---|
| `-p` | Port on which the client accepts commands from the manager. |
| `-l` | *(optional)* Log file. Default: `nfs_client_<port>.log`. |

The client serves each manager connection on its own thread, so a single client can
act as both source and target for several directories at once.

### `nfs_console`

```
./nfs_console -l <console-logfile> -h <host_IP> -p <host_port>
```

| Option | Description |
|---|---|
| `-l` | Log file for issued commands. |
| `-h` | Manager address or hostname. |
| `-p` | Manager command port (the manager's `-p`). |

The console opens a new connection per command, prints every response line, and exits
after `shutdown` or at end of input. It also reads commands from a pipe, so it can be
scripted:

```bash
printf 'add /a@h1:5556 /b@h2:5557\nshutdown\n' | ./nfs_console -l c.log -h mgr -p 5555
```

## Configuration file

One line per synchronization pair:

```
/source_dir@source_host:source_port /target_dir@target_host:target_port
```

Example:

```
# production assets
/srv/assets@10.0.1.10:5556      /var/www/assets@10.0.2.20:5557
/srv/reports@10.0.1.10:5556     /mnt/archive/reports@10.0.3.30:5557
```

- Paths are resolved **on the machine of the corresponding client**, so absolute paths
  are recommended.
- Blank lines and lines starting with `#` are ignored.
- Files with CRLF line endings are supported.
- An invalid line or a port outside `1–65535` makes the manager exit with an error message.

## Console commands

| Command | Description |
|---|---|
| `add <source_dir@host:port> <target_dir@host:port>` | Registers a new pair, runs `LIST` and queues its files immediately. |
| `cancel <source_dir>` | Stops synchronizing the directory and drops its queued tasks. The `<source_dir@host:port>` form is also accepted. |
| `shutdown` | Shuts the manager down gracefully. |

Responses (shown in the console and written to the manager log):

```
[2025-02-10 10:00:01] Added file: /dir1/file1@1.2.3.4:8080 -> /dir2/file1@4.5.6.7:8090
[2025-02-10 10:00:05] Already in queue: /dir1@1.2.3.4:8080
[2025-02-10 10:23:01] Synchronization stopped for /dir1@1.2.3.4:8080
[2025-02-10 10:23:05] Directory not being synchronized: /dir9
[2025-02-10 10:23:02] Shutting down manager...
[2025-02-10 10:23:02] Waiting for all active workers to finish.
[2025-02-10 10:23:03] Processing remaining queued tasks.
[2025-02-10 10:24:01] Manager shutdown complete.
```

`shutdown` semantics: the buffer stops accepting new tasks, in-flight tasks complete, the
queue drains normally, and only then do the worker threads and the process exit. No
transfer is ever interrupted midway.

## Wire protocol

Text commands with a binary payload, over TCP. The manager always initiates the
connection; `nfs_client` never connects on its own.

### LIST

```
→  LIST <source_dir>\n
←  file1.txt\n
   file2.txt\n
   .\n
```

Only regular files are returned; subdirectories and special files are skipped.
A single dot on its own line terminates the list.

### PULL

```
→  PULL <source_dir>/<file>\n
←  <filesize><space><data>
```

On error the client replies `-1 <error message>\n` (e.g. `-1 Permission denied`).
`<filesize>` is a decimal byte count, followed by **exactly** that many bytes.
The manager reads the header one byte at a time, so it never consumes binary data
by mistake.

### PUSH

```
→  PUSH <target_dir>/<file> -1\n          create / truncate the file
→  PUSH <target_dir>/<file> <n> <n bytes> one data chunk (n ≤ 4096)
→  PUSH <target_dir>/<file> 0\n           end of file, close the descriptor
```

The target client creates any missing intermediate directories.

## Logging

All three executables write thread-safe logs with a `YYYY-MM-DD HH:MM:SS` timestamp and
flush after every entry, so the files are usable with `tail -f`.

**Manager – one line per transfer:**

```
[TIMESTAMP] [SOURCE_DIR] [TARGET_DIR] [THREAD_PID] [OPERATION] [RESULT] [DETAILS]
```

```
[2025-02-10 10:00:01] [/dir1/file1@1.2.3.4:8080] [/dir2/file1@4.5.6.7:8090] [1234] [PULL] [SUCCESS] [10 bytes pulled]
[2025-02-10 10:00:02] [/dir1/file1@1.2.3.4:8080] [/dir2/file1@4.5.6.7:8090] [1234] [PUSH] [SUCCESS] [10 bytes pushed]
[2025-02-10 10:00:03] [/dir1/file2@1.2.3.4:8080] [/dir2/file2@4.5.6.7:8090] [1235] [PULL] [ERROR] [File: file2.txt Permission denied]
```

`THREAD_PID` is the real OS thread ID (`gettid`), so log lines can be matched against
`top -H`, `ps -L` or `perf`.

**Console – one line per command:**

```
[2025-02-10 10:00:01] Command add /dir1@1.2.3.4:8080 -> /dir2@4.5.6.7:8090
[2025-02-10 10:23:01] Command cancel /dir1
[2025-02-10 10:23:01] Command shutdown
```

**Client – one line per command served**, with the data size and the cause of any failure.

## Concurrency model

| Mechanism | Role |
|---|---|
| `BoundedBuffer` | Producer/consumer queue with two condition variables (`cv_full_`, `cv_empty_`). Its upper bound caps memory use and applies back-pressure to `LIST`. |
| `all_syncs_mtx` | Guards the table of active syncs (state, error counters, last sync time). |
| `log_mutex` | Serializes writes to the log files. |
| `std::once_flag` | Guarantees the worker threads are joined exactly once, whether shutdown starts from the console or from the main thread. |
| `shutdown()` on the listening socket | Wakes the blocked `accept()` so the command thread exits deterministically, without busy-waiting or signals. |

The sync table is copied under the lock and all network I/O happens outside the
critical section, so a slow or unresponsive peer never blocks console commands.

## Error handling

- A failed connection to a source or target is logged as `[ERROR]`, increments the
  pair's error counter and does **not** bring the manager down; the next task proceeds normally.
- Client-side errors (`-1 <reason>`) are passed through verbatim into the log's `DETAILS` field.
- Every console command runs inside `try/catch`: syntax errors produce a response message,
  never a process exit.
- `SIGPIPE` is ignored; a dropped connection surfaces as a failed write and is handled locally.
- If the command port is already in use, the manager logs it and exits instead of
  running in an inconsistent state.

## Testing

The repository includes an automated test suite:

```bash
./run_tests.sh              # functional tests
./run_tests.sh --valgrind   # plus a memory-leak check

bash run_tests.sh               # if the execute bit was lost on checkout
PORT_BASE=8200 ./run_tests.sh   # if the default ports 7700+ are taken
```

The suite spawns real processes in an isolated temporary directory and covers:

| Area | What is checked |
|---|---|
| Build | Successful compilation, zero warnings under `-Wall -Wextra` |
| CLI | Missing or invalid options for all three executables |
| Config | Missing file, malformed line, out-of-range port, CRLF, comments |
| Protocol | `LIST`, `PULL`, `PULL` of a missing file, chunked `PUSH`, automatic directory creation |
| Integrity | 0-byte files, files just under/over/exactly at the chunk size, 3.3 MB binaries — byte-for-byte comparison |
| Concurrency | Confirms that multiple worker threads did work |
| Console | `add`, duplicate `add`, malformed `add`, `cancel` with a plain path, `cancel` of an unknown directory, unknown command |
| Shutdown | All four messages, queue drained, process actually exits |
| Logs | Format verified with regular expressions |
| Back-pressure | 40 files with `-n 1 -b 1`, with no deadlock or data loss |
| Errors | Target down, source down, client-side error, unreachable manager, port already in use |
| Memory | `valgrind --leak-check=full`: zero definite leaks, zero errors |

The suite exits with status `0` only if every check passes, so it can be dropped into
a CI pipeline as-is.

## Project layout

```
nfs_manager.cpp        Orchestration: worker pool, LIST/PULL/PUSH, console server, shutdown
nfs_client.cpp         Per-directory server implementing LIST / PULL / PUSH
nfs_console.cpp        Interactive CLI for the manager
buffer.{h,cpp}         BoundedBuffer: task queue with back-pressure and graceful drain
config_parser.{h,cpp}  Reads and validates the configuration file
logger.{h,cpp}         Thread-safe logging for manager / client / console
common.h               Shared helpers (timestamps)
Makefile               Targets: all, clean
run_tests.sh           Functional test suite
LICENSE, NOTICE        Apache License 2.0
```

## Enterprise usage

The architecture — central orchestration, a per-node agent, a bounded queue and a
per-file audit log — maps onto scenarios that production environments usually solve
with ad-hoc `rsync` in cron. Example applications:

**1. Distributing build artifacts to edge nodes.**
After a successful build, the pipeline sends `add /builds/release-42@build01:5556
/opt/app/releases@edge07:5557` to the console. The manager fans the files out to many
edge nodes in parallel, with `-n` controlling how many transfers run at once and `-b`
capping memory use. Every file leaves an audit line.

**2. Propagating configuration and feature flags.**
A per-environment configuration directory is pushed to every application server.
`cancel` isolates a misbehaving node immediately, without restarting the manager or
editing the configuration file.

**3. Collecting logs and telemetry.**
In the reverse direction, nodes act as sources and a central host as the target:
periodic collection of log files for analysis, with no SSH access needed from the
center into the nodes.

**4. Moving data between network zones.**
In strictly segmented environments (DMZ ↔ internal network), the firewall only needs
to allow one port per agent and a single, controlled protocol, instead of general SSH
access. The per-file audit log covers traceability requirements (which file, from
where, to where, when, and with what result).

**5. Warm standby / disaster recovery.**
Periodic replication of critical directories (user uploads, database dumps,
certificates) to a standby data center, with one `add` per cycle from a scheduler
(cron, systemd timer). The per-pair error counter and the `[ERROR]` log lines can feed
alerting for a node that has fallen behind.

**6. Rolling out ML models and datasets.**
Model weights are copied to inference nodes before a version switch. Because
`shutdown` completes in-flight tasks first, a half-written model file is never left
on a production node.

**7. Staging assets for render / media farms.**
Large binaries are pushed to render nodes before a job starts, with the bounded buffer
preventing network and memory saturation when hundreds of files are queued at once.

### Operational integration

The agents and the manager are simple, long-running processes with no dependencies,
so they fit directly into `systemd`:

```ini
# /etc/systemd/system/nfs-client.service
[Unit]
Description=NFS Sync agent
After=network-online.target

[Service]
ExecStart=/opt/nfs-sync/nfs_client -p 5556 -l /var/log/nfs-sync/client.log
Restart=always
User=nfssync

[Install]
WantedBy=multi-user.target
```

The manager can be driven by automation, since the console accepts commands from a pipe:

```bash
# a CI/CD pipeline step
printf 'add %s@%s:5556 %s@%s:5557\n' "$ARTIFACT_DIR" "$BUILD_HOST" "$DEPLOY_DIR" "$EDGE_HOST" \
  | nfs_console -l "$CI_LOG" -h "$MANAGER_HOST" -p 5555
```

### What a production deployment would require

This is an academic implementation focused on correct use of sockets, threads and
condition variables. Real-world use would additionally need:

- **Security:** TLS or mTLS on all connections, authentication of the manager to the
  agents, and a per-agent allow-list of directories.
- **Integrity:** a per-file checksum (e.g. SHA-256) verified after `PUSH`, writing to a
  temporary file followed by an atomic rename.
- **Incremental sync:** change detection via `inotify` or mtime/size comparison instead
  of copying everything each time.
- **Resilience:** retries with exponential backoff, per-socket timeouts, and a
  disk-backed queue that survives restarts.
- **Observability:** exported metrics (Prometheus) and structured JSON logs.
- **Scalability:** socket-to-socket streaming instead of loading whole files into
  memory, so file size is not bounded by available RAM.

## Limitations and non-goals

- Synchronization is **one-way** and **one-shot** per registration: files are copied when
  the pair is added, not continuously.
- Only top-level regular files are synchronized; subdirectories, symlinks and
  permissions/ownership are not copied.
- Each file is loaded entirely into the manager's memory during `PULL`.
- There is no encryption or authentication; it is intended for trusted networks.
- `cancel` stops future synchronization and drops queued tasks; it does not interrupt a
  transfer that is already running.

## Troubleshooting

| Symptom | Cause / fix |
|---|---|
| `Error connecting to source ...: Connection refused` | The source client is not running on the configured port. Check with `ss -tlnp \| grep 5556`. |
| The log shows `[PULL] [SUCCESS]` but no `[PUSH]` | The target client is unreachable; the `[PUSH] [ERROR]` line shows the address that failed. |
| `[PULL] [ERROR] [File: x Permission denied]` | The user running the source client cannot read the file. |
| `[PUSH] [ERROR]` with `Error opening for PUSH init` in the client log | The target directory is not writable by the client's user. |
| The manager exits immediately with `cannot bind command port` | The `-p` port is already in use. |
| Nothing syncs and the log shows `No syncs configured` | The config file is empty or contains only comments. |
| Leftover processes from an earlier run | `pkill -f nfs_client; pkill -f nfs_manager; pkill -f nfs_console` |

## License

Distributed under the **Apache License 2.0**. See [LICENSE](LICENSE) and [NOTICE](NOTICE).

---

Author: [Andreaslmpr](https://github.com/Andreaslmpr) · C++17 · Linux / WSL2
