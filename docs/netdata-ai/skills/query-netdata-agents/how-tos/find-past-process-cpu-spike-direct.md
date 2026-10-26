# Find the process behind a past CPU spike through a direct Agent call

## Question

A Linux node's CPU spiked a few minutes ago (or an alert fired) and the process may have exited since. Which
processes used the CPU then?

## Inputs

- `NODE_UUID`, `MACHINE_GUID`, `AGENT_HOST` (host:port, no scheme) of the target Agent.
- `NETDATA_CLOUD_TOKEN` and `NETDATA_CLOUD_HOSTNAME` in `<repo>/.env` (for the bearer the wrapper mints).
- The moment of the spike, as a unix epoch (`SPIKE_EPOCH`), or a relative time such as `-300`.
- `jq`.

## Steps

1. Load the token-safe wrappers:

   ```bash
   REPO_ROOT="$(git rev-parse --show-toplevel)"
   source "$REPO_ROOT/docs/netdata-ai/skills/query-netdata-agents/scripts/_lib.sh"
   agents_load_env
   ```

2. Ask for the processes seen in a range around the spike. `processes` takes command words, not JSON fields, so
   the time goes into the function string:

   ```bash
   AFTER=$((SPIKE_EPOCH - 120)); BEFORE=$((SPIKE_EPOCH + 60))
   agents_query_agent --node "$NODE_UUID" --host "$AGENT_HOST" --machine-guid "$MACHINE_GUID" \
       POST "/api/v3/function?function=processes%20after:${AFTER}%20before:${BEFORE}" \
     > "$REPO_ROOT/.local/audits/query-netdata-agents/processes-range.json"
   ```

3. Project the answer and its completeness:

   ```bash
   jq '{recorded, processes_total, processes_shown,
        top: [ (.columns | keys_unsorted) as $k | .data[:10][] | [$k, .] | transpose
               | map({(.[0]): .[1]}) | add
               | {Cmd, Exe, PID, AvgCPU, PeakCPU, PeakRSS, FirstSeen, LastSeen, Started, Exited} ]}' \
     "$REPO_ROOT/.local/audits/query-netdata-agents/processes-range.json"
   ```

4. For the exact table at one moment, use `processes%20at:${SPIKE_EPOCH}`; `recorded.sample_ut` is the time of the
   sample returned.

## Output

- `recorded.source`: `ram` for the last 30 minutes, `disk` for windows the Agent saved around alerts and anomalies.
- `recorded.cpu_coverage` / `cpu_coverage_min`: the share of all-process CPU the recorded processes held. Low values
  mean many similar processes shared the CPU and some were not recorded.
- `Exited: "yes"` marks processes that ended inside the range; they are no longer in the live `processes` table.

## Notes / gotchas

- Linux only, and only while the recorder is on (`without-recorder` disables it). A 503 says it is off.
- Older than 30 minutes is answered only from saved windows; a 404 means nothing was recorded for that time.
- `AvgCPU` averages over the whole range, counting 0 while the process was not recorded; use `PeakCPU` for bursts.
- Other `processes` filters (`category:`, `pid:`) do not apply to the recorded past.
- `CmdLine` appears only if the Agent records command lines (`with-recorder-cmdline`) and the caller may see them.
- The response keeps `has_history: false`; treat it as a table, not a time series.

## Source guides

- [query-functions.md](../query-functions.md)
- [docs/functions/processes.md](../../../../functions/processes.md#recorded-past-linux)
