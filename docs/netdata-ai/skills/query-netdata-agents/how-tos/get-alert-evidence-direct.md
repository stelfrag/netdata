# Get the evidence for an alert through a direct Agent call

## Question

An alert fired on a node. Which processes were running and busiest then, what started or exited around it, which
connections were open, and did the host change before it?

## Inputs

- `NODE_UUID`, `MACHINE_GUID`, `AGENT_HOST` (host:port, no scheme) of the Agent that evaluated the alert.
- `NETDATA_CLOUD_TOKEN` and `NETDATA_CLOUD_HOSTNAME` in `<repo>/.env` (for the bearer the wrapper mints).
- `jq`.

## Steps

1. Load the token-safe wrappers:

   ```bash
   REPO_ROOT="$(git rev-parse --show-toplevel)"
   source "$REPO_ROOT/docs/netdata-ai/skills/query-netdata-agents/scripts/_lib.sh"
   agents_load_env
   ```

2. Find the transition id of the alert. Called without a transition, the Function lists this Agent's WARNING and
   CRITICAL transitions of the last day:

   ```bash
   agents_query_agent --node "$NODE_UUID" --host "$AGENT_HOST" --machine-guid "$MACHINE_GUID" \
       GET '/api/v3/function?function=alert-evidence' \
     | jq -r '.data[] | [.[0], (.[1] / 1000000 | todate), .[2], .[3], .[4]] | @tsv'
   ```

3. Ask for the evidence of one transition:

   ```bash
   TRANSITION_ID=<from step 2>
   agents_query_agent --node "$NODE_UUID" --host "$AGENT_HOST" --machine-guid "$MACHINE_GUID" \
       GET "/api/v3/function?function=alert-evidence%20transition:${TRANSITION_ID}" \
     > "$REPO_ROOT/.local/audits/query-netdata-agents/alert-evidence.json"
   ```

4. Read the sections and their status first, then the busiest processes:

   ```bash
   F="$REPO_ROOT/.local/audits/query-netdata-agents/alert-evidence.json"
   jq '.transition | {alert, when, new_status}' "$F"
   jq '.evidence | map_values(.status)' "$F"
   jq '.evidence.processes_at.rows[:5] | map({Cmd, PID, CPU, RSS, RecordedFor})' "$F"
   jq '.evidence.processes_window | {running, recorded_processes, started, exited, top: (.rows[:5] | map({Cmd, PeakCPU, Started, Exited}))}' "$F"
   jq '.evidence.host_changes.events' "$F"
   ```

## Output

- `processes_at`: the table at the alert (`recorded.sample_ut` is the sample used), busiest first.
- `processes_window`: 10 minutes before to 2 minutes after; `Started` / `Exited` mark processes that began or ended
  inside it, and `started` / `exited` count them.
- `connections`: the sockets open when the alert fired (`captured` is the snapshot time).
- `host_changes`: reboots, kernel/OS/Netdata version changes, CPU/RAM/disk/default-interface changes in the 24 hours
  before.
- `journal` and `correlated_metrics`: the time ranges to use with `systemd-journal` and `/api/v2/weights`
  (method `ks2`, the 10 minutes before the alert against the 40 minutes before that).

## Notes / gotchas

- Use GET: the Agent closes a POST that has no body and no `Content-Length` without answering.
- `host_changes` is `unavailable` when the Agent logs to files instead of the systemd journal.
- Check each section's `status` before its data: `not_saved` (nothing recorded near the alert), `not_captured` or
  `expired` (connections), `unavailable` (the source does not run on this node), `disabled`, `other_node`.
- Ask the Agent that evaluated the alert. A parent's own transitions for a child answer `other_node`: query the child.
- Evidence is kept only on Agents with a disk-based database (`dbengine`), outside the IoT profile; process and
  connection evidence needs Linux.
- Saved process windows and connection snapshots are capped on disk; old incidents can be gone.
- A section's `save.status` of `not_saved` with `reason: disk_full` means the Agent recorded the evidence but could
  not write it. With `in_ram: true` the data is in the answer and is still being retried; otherwise it is gone.
- Command lines are never in the answer.
- Through MCP the same answer comes from the tool `get_alert_evidence` (`transition_id`, optional `node`).

## Source guides

- [query-functions.md](../query-functions.md)
- [docs/functions/alert-evidence.md](../../../../functions/alert-evidence.md)
