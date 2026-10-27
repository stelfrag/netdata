# Function: Alert Evidence

## Quick Info

- **Provided by**: the Netdata Agent itself
- **Type**: JSON document for one transition; tables (recent alerts, processes around one alert) in the dashboard
- **Availability**: every Agent; the process and connection evidence needs Linux and a disk-based database (`dbengine`)
- **Required Access**: signed-in users of the same space, with access to sensitive data
- **MCP tool**: `get_alert_evidence`

## Purpose

When an alert fires, the metrics, the logs and the alert history stay available, but the processes that were running
and the open connections disappear, and nothing says what changed on the host before. The `alert-evidence` Function
answers, for one alert transition, with everything the Agent kept for that moment:

| Section              | What it holds                                                                                   | Source                                                                       |
|----------------------|-------------------------------------------------------------------------------------------------|------------------------------------------------------------------------------|
| `processes_at`       | the processes at the moment of the alert, busiest first                                         | the [recorded past](processes.md#recorded-past-linux) of `processes`         |
| `processes_window`   | the processes from 10 minutes before to 2 minutes after, with how many started and exited       | the same                                                                     |
| `files`              | the files the busiest processes by I/O were writing in that window, with mount point and size   | the window's answer of `processes` ([files](processes.md#recorded-past-linux)) |
| `connection_events`  | the TCP connections processes opened and closed in that window, with addresses and bytes        | the window's answer of `processes` ([connections](processes.md#recorded-past-linux)) |
| `connections`        | the open connections captured when the alert fired                                              | `network-connections`, captured by the Agent at the time                     |
| `host_changes`       | host changes in the 24 hours before the alert (reboot, kernel, OS, Netdata version, CPUs, RAM…) | [host change events](/src/libnetdata/log/README.md#host-change-events) in the systemd journal |
| `journal`            | the time range to read in the logs                                                              | a pointer for the `systemd-journal` Function                                 |
| `correlated_metrics` | the metrics that changed most in the 10 minutes before the alert, against the 40 minutes before | a pointer for `/api/v2/weights` or the MCP tool `find_correlated_metrics`    |

## How the evidence is kept

- When a local alert is raised to WARNING or CRITICAL (and on an anomaly episode or a manual
  `netdatacli save-process-history`), the Agent saves the processes recorded from 10 minutes before to 2 minutes
  after, and captures the open connections once.
- Connection snapshots are kept compressed in `alert-evidence/` under the Agent's cache directory, up to 16 MB, the
  oldest deleted first. Alerts within 10 seconds of each other share one snapshot.
- Nothing is saved on Agents without a disk-based database (`ram`, `alloc` or `none`) or with the IoT profile.
- Each Agent keeps the evidence of the alerts it evaluates for itself. On a parent, ask about a child's alert on the
  child: its own Agent kept the evidence.

## Usage

```text
alert-evidence transition:<transition id>
```

Get the transition id from the alert transitions (the MCP tool `list_alert_transitions`, or
`/api/v2/alert_transitions`), or from the dashboard view below.

### In the dashboard

Open **Alert Evidence** in the Functions tab. It has an **Alert** picker:

- **Recent alerts** (the default) lists this Agent's WARNING and CRITICAL transitions of the last 24 hours (at most
  500, newest first): transition id, time, alert, instance, new and previous status, value.
- Pick one of the last 100 alerts to see the processes from 10 minutes before to 2 minutes after it: average and peak
  CPU, peak memory, first and last seen, and whether each process started or exited in that time. Command lines are not
  shown.
- When nothing was saved for the alert you pick (for example it fired before the Agent recorded processes, or the
  saved history was deleted to stay within the disk limit), the table says so in one row.

The same views answer `alert-evidence` (or `alert-evidence alert:all`) and `alert-evidence alert:<transition id>`.

## Answer

```json
{
  "status": 200,
  "type": "alert-evidence",
  "transition": { "id": "...", "node": "...", "alert": "...", "when": 1793020000, "new_status": "WARNING", "...": "..." },
  "evidence": {
    "processes_at":       { "status": "ok", "running": 529, "recorded_processes": 53, "recorded": { "...": "..." }, "rows": [ { "Cmd": "...", "CPU": 99.9, "...": "..." } ] },
    "processes_window":   { "status": "ok", "running": { "min": 520, "avg": 528.4, "max": 541 }, "recorded_processes": 881, "rows": [ "..." ], "started": 633, "exited": 621 },
    "files":              { "status": "ok", "source": "processes_window", "list": [ { "process": "dd", "path": "/mnt/small/fill.bin", "mount": "/mnt/small", "bytes_first": 0, "bytes_last": 209715200, "deleted": false, "...": "..." } ], "total": 3, "truncated": false },
    "connection_events":  { "status": "ok", "source": "processes_window", "total": 412, "lost": 0, "netns_other": 0, "by_process": [ { "process": "curl", "opens": 0, "closes": 200, "bytes_sent": 16400, "bytes_received": 98000, "...": "..." } ], "list": [ { "time_ut": 1793019990000000, "event": "close", "process": "unknown", "local": "10.0.0.5:41022", "remote": "192.0.2.10:443", "state": "fin-wait1", "...": "..." } ], "truncated": true },
    "connections":        { "status": "ok", "captured": 1793020005, "rows": [ "..." ], "total": 120 },
    "host_changes":       { "status": "ok", "includes_startup": false, "events": [ { "ND_HOST_CHANGE": "kernel", "...": "..." } ] },
    "journal":            { "status": "pointer", "function": "systemd-journal", "after": 1793019400, "before": 1793020120 },
    "correlated_metrics": { "status": "pointer", "api": "/api/v2/weights", "method": "ks2", "...": "..." }
  }
}
```

Every section reports a `status`:

| Status         | Meaning                                                                                             |
|----------------|-----------------------------------------------------------------------------------------------------|
| `ok`           | the evidence is in the answer                                                                       |
| `not_saved`    | nothing was recorded or saved for that moment                                                       |
| `not_captured` | no connection snapshot was taken near that moment                                                   |
| `expired`      | the moment is older than the oldest connection snapshot kept                                        |
| `unavailable`  | the source is not running here (for example no flight recorder, no network viewer, no journal)     |
| `disabled`     | connection snapshots are turned off, or the Agent has no disk-based database                        |
| `other_node`   | the transition was evaluated for another node; ask that node's Agent                                |
| `error`        | the source answered with an error; `code` holds the HTTP code                                       |

`processes_at`, `processes_window` and `connections` can also carry a `save` object. It says whether that evidence
is on disk:

- `status` is `ok` (saved), `pending` (the window is still being recorded and saved as it goes), `not_requested`
  (only in memory, no save asked for it) or `not_saved` (a save could not be written).
- When it is `not_saved`, `reason` is `disk_full` or `write_error`, and `in_ram` says whether the Agent still holds
  the data.

Data still held is in the answer, and the Agent keeps trying to write it. Once the disk has space again, `status`
becomes `ok`. When the data is gone, the section's own `status` is `not_saved` with the same `reason`.

When the filesystem is full, the Agent deletes its own oldest saved process history and connection snapshots to make
room for the new ones, once per disk-full episode. It never deletes anything else.

`running` is every process on the host (at the alert, or `min`/`avg`/`max` over the window); `recorded_processes` is
the ones the recorder kept in detail: the busiest by CPU, memory and I/O, and every process that started or exited.
`recorded.totals` adds the CPU and memory of all processes. Process rows show at most 50 of the recorded processes and
connection rows at most 1000; `truncated` says there were more, and the connections' `total` how many. `started` and `exited` are counted over the busiest 1000 processes of the window; `counts_partial` is true when
there were more. `processes_at` is `ok` only when the recorder has a sample from the minute before the alert. Command
lines are never included.

`files` lists at most 20 files, the ones that grew most in the window first (`total` and `truncated` say how many
there were); each has the process (`key`, `pid`, `process`), `path`, `mount`, the size when first and last recorded
(`bytes_first`, `bytes_last`, with `first_ut` and `last_ut`) and `deleted` (still written to after it was deleted, so
its space is not freed). Its status follows `processes_window`; it is `unavailable` when `apps.plugin` did not look at
files (file charts off, or `without-recorder-files`).

`connection_events` shows the TCP connections opened and closed in the window, in the network namespace of
`apps.plugin`: `by_process` (the 20 processes with the most events, with their opens, closes and the bytes of their
closed connections) and `list` (the newest 100 events, oldest first, each with the process, `event`, `found_by`,
`local`, `remote`, `state`, `direction` and the bytes). `total` counts the events of the window and `lost` those not
recorded; `truncated` says the lists are partial. When processes held sockets in other network namespaces (containers
with their own network), `netns_other` counts them and `coverage` says their connections are not covered. Its status
follows `processes_window`; it is `unavailable` when `apps.plugin` did not record connections (file charts off,
`without-recorder-connections`, or a kernel that does not report TCP closes).

`host_changes` reads the systemd journal. It is `unavailable` when the Agent logs to files instead; the changes are
then in its daemon log. The changes the Agent finds at startup are logged once it is running, which can be after the
first alerts it evaluates: for an alert leaving `UNINITIALIZED`, the window also covers the 2 minutes after the alert,
and `includes_startup` is true.

## Configuration

Connection snapshots are on by default. To turn them off, in `netdata.conf`:

```text
[health]
    capture connections on alerts = no
```

The process evidence follows the `apps.plugin` flight recorder options; see
[Recorded Past](processes.md#recorded-past-linux).
