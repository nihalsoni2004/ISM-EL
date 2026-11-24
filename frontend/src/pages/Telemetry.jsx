import { Activity, Cpu, RotateCcw } from 'lucide-react';
import { useState } from 'react';
import { request } from '../api/client';
import { apiPath, post, useOperations, useResource } from '../api/state';
import { Badge, Button, Field, Fields, Metric, Notice, OperationForm, Page, Panel, Progress, QueryState, Table } from '../components/UI';
import { bytes, date, integer, number } from '../storage';

const categoryLabels = {
  VIRTUAL_DISK_READ: 'Virtual disk reads',
  VIRTUAL_DISK_WRITE: 'Virtual disk writes',
  RAID_LOGICAL_READ: 'Logical RAID reads',
  RAID_LOGICAL_WRITE: 'Logical RAID writes',
  REBUILD_READ: 'Rebuild physical reads',
  REBUILD_WRITE: 'Rebuild physical writes',
  REBUILD_BATCH: 'Reconstruction batches',
  REBUILD_VERIFY: 'Rebuild verification batches',
};
const profileLabels = { HDD_7200: '7200 RPM HDD', SATA_SSD: 'SATA SSD', NVME_SSD: 'NVMe SSD' };
const format = (value) => Number.isFinite(value)
  ? new Intl.NumberFormat(undefined, { maximumFractionDigits: 6 }).format(value) : 'Unavailable';
const quantity = (value, unit) => Number.isFinite(value) ? format(value) + ' ' + unit : 'Unavailable';
const illustrativeWorkload = [
  { device: 0, arrival_ms: 0, operation: 'READ', bytes: 4096, access: 'RANDOM' },
  { device: 0, arrival_ms: 0.01, operation: 'WRITE', bytes: 4096, access: 'RANDOM' },
  { device: 1, arrival_ms: 0, operation: 'READ', bytes: 4096, access: 'RANDOM' },
];
const categoryRows = (categories) => categories.map((item) => ({ ...item, id: item.category }));

function CountTable({ categories, label }) {
  return <Table label={label} rows={categoryRows(categories)} columns={[
    { key: 'category', title: 'Operation', render: (row) => categoryLabels[row.category] || row.category },
    { key: 'attempted', title: 'Attempted', render: (row) => number(row.attempted) },
    { key: 'completed', title: 'Completed', render: (row) => number(row.completed) },
    { key: 'failed', title: 'Failed', render: (row) => number(row.failed) },
    { key: 'blocks', title: 'Completed blocks', render: (row) => number(row.blocks) },
    { key: 'bytes', title: 'Completed bytes', render: (row) => number(row.bytes) },
    { key: 'elapsed', title: 'Total elapsed (ms)', render: (row) => format(row.elapsed_ns / 1e6) },
  ]} />;
}

function LatencyTable({ categories }) {
  const windows = { EMPTY: 'Empty', ENTIRE_OBSERVATION: 'Entire observation', RECENT_SAMPLES: 'Recent samples' };
  return <Table label="Measured latency summaries" rows={categoryRows(categories)} columns={[
    { key: 'category', title: 'Operation', render: (row) => categoryLabels[row.category] || row.category },
    { key: 'count', title: 'Samples retained / total', render: (row) => number(row.latency.sample_count) + ' / ' + number(row.latency.count) },
    { key: 'window', title: 'Window', render: (row) => windows[row.latency.window] },
    ...[['min_ms', 'Min (ms)'], ['max_ms', 'Max (ms)'], ['mean_ms', 'Mean (ms)'], ['p50_ms', 'p50 (ms)'],
      ['p95_ms', 'p95 (ms)'], ['p99_ms', 'p99 (ms)']].map(([key, title]) => ({ key, title, render: (row) => format(row.latency[key]) })),
  ]} />;
}

function Measured({ telemetry }) {
  const find = (category) => telemetry.categories.find((row) => row.category === category);
  const totalBlocks = (first, second) => {
    const a = find(first)?.blocks;
    const b = find(second)?.blocks;
    return Number.isFinite(a) && Number.isFinite(b) ? number(a + b) : null;
  };
  const logicalRead = find('RAID_LOGICAL_READ')?.completed;
  const logicalWrite = find('RAID_LOGICAL_WRITE')?.completed;
  const rates = telemetry.logical_rates;
  const sampled = telemetry.categories.some((row) => row.latency.sample_count > 0);
  return <>
    <div className="metrics-grid">
      <Metric title="Logical RAID reads" value={logicalRead == null ? null : number(logicalRead)} icon={Activity} subtitle="Completed requests" />
      <Metric title="Logical RAID writes" value={logicalWrite == null ? null : number(logicalWrite)} icon={Activity} accent="blue" subtitle="Completed requests" />
      <Metric title="Physical blocks read" value={totalBlocks('VIRTUAL_DISK_READ', 'REBUILD_READ')} icon={Cpu} accent="purple" subtitle="All origins, including rebuild" />
      <Metric title="Physical blocks written" value={totalBlocks('VIRTUAL_DISK_WRITE', 'REBUILD_WRITE')} icon={Cpu} accent="amber" subtitle="All origins, including rebuild" />
    </div>
    <Panel title="Foreground logical rates" description="Averages since engine start or the last telemetry reset. Idle time is included.">
      <Fields items={[
        ['Observation window', quantity(telemetry.observation.seconds, 's')],
        ['Observation generation', number(telemetry.observation.generation)],
        ['Completed logical requests', number(rates.completed_requests)],
        ['Completed logical bytes', bytes(rates.bytes)],
        ['Request rate', quantity(rates.iops, 'IOPS')],
        ['Throughput', quantity(rates.mib_per_second, 'MiB/s')],
      ]} />
    </Panel>
    <Panel title="Operation counters" description="Each row has its own counting boundary. Physical and logical rows represent different layers.">
      <CountTable categories={telemetry.categories} label="Measured operation counters" />
    </Panel>
    <Panel title="Latency summaries" description={'Monotonic durations. The latest ' + number(telemetry.latency_sample_limit_per_category) + ' samples per category are retained.'}>
      {sampled ? <LatencyTable categories={telemetry.categories} /> : <div className="empty">No measurements collected yet.</div>}
    </Panel>
  </>;
}

function RebuildMeasurements() {
  const [selected, setSelected] = useState('');
  const jobs = useResource('rebuilds', { field: 'jobs', interval: selected ? false : 4000 });
  const detail = useResource(['telemetry', 'rebuilds', selected], { enabled: Boolean(selected),
    interval: (query) => ['COMPLETED', 'FAILED'].includes(query.state.data?.job?.status) ? false : 4000,
  });
  return <Panel title="Rebuild telemetry" description="Progress comes from the durable job. Timed operations cover the current engine process.">
    <QueryState query={jobs} empty="No rebuild jobs have been prepared.">{(items) => <div className="panel-body">
      <Field label="Rebuild job"><select value={selected} onChange={(event) => setSelected(event.target.value)}>
        <option value="">Select a rebuild job</option>
        {items.map((job) => <option key={job.job_id} value={job.job_id}>{job.job_id} · {job.array_id}</option>)}
      </select></Field>
    </div>}</QueryState>
    {selected && <QueryState query={detail}>{(data) => <>
      <div className="panel-body rebuild-detail">
        <Progress job={data.job} />
        <Fields items={[
          ['Job state', <Badge value={data.job.status} />],
          ['RAID level', 'RAID ' + data.raid_level],
          ['Durable completed blocks', number(data.job.completed_blocks)],
          ['Durable remaining blocks', number(data.remaining_blocks)],
          ['Last durable checkpoint', date(data.last_checkpoint_at)],
        ]} />
        {!data.retained && <Notice>No process-local measurements are retained for this job.</Notice>}
      </div>
      <CountTable categories={data.categories} label="Measured rebuild operations" />
      {data.categories.some((row) => row.latency.sample_count > 0) && <LatencyTable categories={data.categories} />}
    </>}</QueryState>}
  </Panel>;
}

function ProfileParameters({ parameters }) {
  return <Fields items={[
    ['Sequential read / write overhead', format(parameters.sequential_read_ms) + ' / ' + format(parameters.sequential_write_ms) + ' ms'],
    ['Random read / write overhead', format(parameters.random_read_ms) + ' / ' + format(parameters.random_write_ms) + ' ms'],
    ['Read / write bandwidth', format(parameters.read_mib_per_second) + ' / ' + format(parameters.write_mib_per_second) + ' MiB/s'],
    ['Service slots per device', number(parameters.queue_depth)],
  ]} />;
}

function Simulation() {
  const profiles = useResource(['model', 'profiles']);
  const { run, busy, locked, setFeedback } = useOperations();
  const [profile, setProfile] = useState('HDD_7200');
  const [result, setResult] = useState(null);
  const [validation, setValidation] = useState('');
  async function simulate(event) {
    event.preventDefault();
    if (locked || profiles.isError) return;
    const data = new FormData(event.currentTarget);
    const devices = data.get('devices');
    if (!integer(devices, 1, profiles.data.limits.max_devices)) {
      setValidation('Enter a whole device count within the scenario bounds.');
      return;
    }
    let workload;
    try { workload = JSON.parse(data.get('workload')); }
    catch { setValidation('Enter the workload as a JSON array.'); return; }
    if (!Array.isArray(workload) || workload.length < 1 || workload.length > profiles.data.limits.max_requests) {
      setValidation('Enter between 1 and ' + profiles.data.limits.max_requests + ' modeled requests.');
      return;
    }
    setValidation('');
    setResult(null);
    const computed = await run({ label: 'Running simulation', task: () =>
      request(apiPath('model', 'simulate'), { method: 'POST', mutates: false, body: { profile, devices: Number(devices), workload } }) });
    if (computed) setResult(computed);
  }

  return <Panel title="Device performance model" description="Hypothetical device service and queueing from an explicit scenario."
    action={<Badge label="MODELED" color="warning" />}>
    <QueryState query={profiles}>{(data) => {
      const choice = data.profiles.find((item) => item.profile === profile);
      return <div className="panel-body model-body">
        <form className="form" onSubmit={simulate} onChange={() => { setResult(null); setFeedback(null); }}>
          <div className="form-grid">
            <Field label="Device profile"><select value={profile} onChange={(event) => setProfile(event.target.value)} disabled={Boolean(busy)}>
              {data.profiles.map((item) => <option key={item.profile} value={item.profile}>{profileLabels[item.profile]} · {item.profile}</option>)}
            </select></Field>
            <Field label="Modeled devices"><input type="number" name="devices" min="1" max={data.limits.max_devices} defaultValue="2" disabled={Boolean(busy)} /></Field>
          </div>
          {choice && <ProfileParameters parameters={choice.parameters} />}
          <Field label="Scenario requests (JSON)" hint="The example queues two requests on device 0 and overlaps one on device 1. Add depends_on with earlier request indices to sequence dependent I/O.">
            <textarea name="workload" rows={8} maxLength={32000} spellCheck={false} disabled={Boolean(busy)}
              defaultValue={JSON.stringify(illustrativeWorkload, null, 2)} />
          </Field>
          {validation && <Notice tone="danger" role="alert">{validation}</Notice>}
          <div className="actions"><Button type="submit" variant="primary" disabled={locked || profiles.isError || !choice}>
            {busy === 'Running simulation' ? 'Calculating…' : 'Run model scenario'}
          </Button></div>
        </form>
        {result && <div aria-live="polite" className="model-results">
          <div className="capacity-title"><span>Modeled elapsed time · {result.profile}</span><strong>{quantity(result.total_modeled_elapsed_ms, 'ms')}</strong></div>
          <Table label="Modeled request results" rows={result.requests.map((row) => ({ ...row, id: row.index }))} columns={[
            { key: 'index', title: 'Request', render: (row) => '#' + row.index },
            { key: 'device', title: 'Device' }, { key: 'operation', title: 'Operation' },
            { key: 'bytes', title: 'Bytes', render: (row) => number(row.bytes) },
            ...[['effective_arrival_ms', 'Arrival (ms)'], ['start_ms', 'Start (ms)'], ['service_ms', 'Service (ms)'],
              ['queue_wait_ms', 'Queue wait (ms)'], ['completion_ms', 'Completion (ms)']].map(([key, title]) =>
              ({ key, title, render: (row) => format(row[key]) })),
          ]} />
        </div>}
      </div>;
    }}</QueryState>
  </Panel>;
}

export default function Telemetry() {
  const telemetry = useResource('telemetry', { interval: 4000 });
  const { run, locked, setFeedback } = useOperations();
  const [reset, setReset] = useState(false);
  return <Page title="Storage telemetry" description="Inspect engine measurements and run separate hypothetical device scenarios."
    action={<Button icon={RotateCcw} disabled={locked || !telemetry.data} onClick={() => { setFeedback(null); setReset(true); }}>Reset measurements</Button>}>
    <Panel title="Engine observations" description="Actual C++ operations in the current engine process"
      action={<Badge label="MEASURED" color="accent" />}>
      {telemetry.data === undefined && <QueryState query={telemetry}>{() => null}</QueryState>}
    </Panel>
    {telemetry.data !== undefined && <QueryState query={telemetry}>{(data) => <Measured telemetry={data} />}</QueryState>}
    <RebuildMeasurements />
    <Simulation />
    {reset && <OperationForm title="Reset measurements" description="Start a new observation window and clear the current engine’s telemetry."
      submit="Confirm reset" onClose={() => setReset(false)} onSubmit={async () => {
        const reply = await run({ label: 'Resetting measurements', task: () => post(['telemetry', 'reset']), roots: ['telemetry'],
          uncertainTarget: 'the observation generation on Telemetry', success: () => 'Measurements reset. A new observation window is active.' });
        if (reply) setReset(false);
      }}>
      <p>Disk images, storage metadata, and durable rebuild progress are preserved.</p>
    </OperationForm>}
  </Page>;
}
