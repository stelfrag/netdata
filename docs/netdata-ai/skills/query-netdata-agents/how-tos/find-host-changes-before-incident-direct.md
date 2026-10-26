# Find what changed on a host before an incident through a direct Agent call

## Question

Something broke on a node. Did the host change shortly before: a reboot, a new kernel, OS or Netdata version, a
different CPU count or RAM, disk space, or a new default interface or IP?

## Inputs

- `NODE_UUID`, `MACHINE_GUID`, `AGENT_HOST` (host:port, no scheme) of the target Agent (Linux with systemd-journal).
- `NETDATA_CLOUD_TOKEN` and `NETDATA_CLOUD_HOSTNAME` in `<repo>/.env` (for the bearer the wrapper mints).
- The time range to look at, in seconds (`AFTER`, `BEFORE`; negative is relative to now).
- `jq`.

## Steps

1. Load the token-safe wrappers:

   ```bash
   REPO_ROOT="$(git rev-parse --show-toplevel)"
   source "$REPO_ROOT/docs/netdata-ai/skills/query-netdata-agents/scripts/_lib.sh"
   agents_load_env
   ```

2. Ask the `systemd-journal` Function for host change events only. The journal stores the message ID without dashes:

   ```bash
   AFTER=-604800; BEFORE=0
   agents_query_agent --node "$NODE_UUID" --host "$AGENT_HOST" --machine-guid "$MACHINE_GUID" \
       POST '/api/v3/function?function=systemd-journal' \
       "{\"after\":$AFTER,\"before\":$BEFORE,\"last\":200,\"direction\":\"backward\",
         \"selections\":{\"__logs_sources\":[\"all\"],\"MESSAGE_ID\":[\"adfd0357c3d6490fb5c7cd9337492f11\"]}}" \
     > "$REPO_ROOT/.local/audits/query-netdata-agents/host-changes.json"
   ```

3. Project one line per change:

   ```bash
   jq -r '(.columns | to_entries | map({(.key): .value.index}) | add) as $c
          | .data[] | [ (.[$c.timestamp] / 1000000 | todate),
                        .[$c.ND_HOST_CHANGE], .[$c.ND_HOST_CHANGE_OLD], .[$c.ND_HOST_CHANGE_NEW] ] | @tsv' \
     "$REPO_ROOT/.local/audits/query-netdata-agents/host-changes.json"
   ```

## Output

- `ND_HOST_CHANGE` names what changed: `reboot`, `kernel`, `os`, `netdata_version`, `cpus`, `ram` (found when the
  Agent starts) and `cpus`, `ram`, `disk_space`, `default_iface`, `default_iface_ip` (found while it runs).
- Startup events carry the time the Agent started, not the time the host changed; a `reboot` event means the host
  booted at some point before that.
- An empty old or new value means the value appeared or disappeared, for example the default route went away.

## Notes / gotchas

- Runtime values are checked every 30 minutes (first 5 minutes after start), so a change is seen up to 30 minutes late.
- RAM differences of 1% or less are not reported.
- Nothing is logged on the Agent's first start, when a value could not be detected, or when nothing changed: an empty
  answer means no change was seen, not that the Agent was not running.
- Without systemd-journal, the same events are in the Agent's daemon log (`host_change=` in logfmt) or the Windows
  event log (`HostChange` fields).

## Source guides

- [query-logs.md](../query-logs.md)
- [src/libnetdata/log/README.md](../../../../../src/libnetdata/log/README.md#host-change-events)
