import { Activity, Pause, Play, Plus } from 'lucide-react';
import { useState } from 'react';
import { useNavigate, useParams } from 'react-router-dom';
import { activeJob, post, useOperations, useRebuildJobs, useResource } from '../api/state';
import { Badge, Button, Field, Fields, Notice, OperationForm, Page, Panel, Progress, QueryState, RowLink, Table, TextLink } from '../components/UI';
import { bytes, date, integer, maxLba, number } from '../storage';

const policyNames = { FULL_SPEED: 'Full-speed', FIXED_THROTTLE: 'Fixed throttle', ADAPTIVE: 'Adaptive' };
const ms = (microseconds) => `${number(microseconds / 1000)} ms`;

function Prepare({ onClose }) {
  const [array, setArray] = useState('');
  const [failedDisk, setFailedDisk] = useState('');
  const [spareDisk, setSpareDisk] = useState('');
  const [policy, setPolicy] = useState('FULL_SPEED');
  const arrays = useResource('raids', { field: 'arrays' });
  const spares = useResource('spares', { field: 'spares' });
  const members = useResource(['raids', array, 'members'], { field: 'members', enabled: Boolean(array) });
  const { run } = useOperations();
  const navigate = useNavigate();
  const degraded = !arrays.isError ? arrays.data?.filter((item) => item.health === 'DEGRADED') : undefined;
  const failed = !members.isError ? members.data?.filter((item) => item.state === 'FAILED') : undefined;
  const available = !spares.isError ? spares.data?.filter((item) => item.available && !item.assignment) : undefined;
  const valid = degraded?.some((item) => item.array_id === array) && failed?.some((item) => item.disk_id === failedDisk) &&
    available?.some((item) => item.disk_id === spareDisk);
  return <OperationForm title="Prepare rebuild" description="Reserve a hot spare to reconstruct the selected failed member."
    submit="Prepare rebuild" onClose={onClose} disabled={!valid} onSubmit={async (values) => {
      const scheduling = { policy };
      if (policy === 'FIXED_THROTTLE') {
        if (!integer(values.fixed_delay_ms, 1, 50)) return 'Enter a fixed delay from 1 to 50 ms.';
        scheduling.fixed_delay_us = Number(values.fixed_delay_ms) * 1000;
      }
      if (policy === 'ADAPTIVE') {
        if (!integer(values.target_p95_ms, 1, 200) || !integer(values.max_delay_ms, 0, 50)) {
          return 'Enter a p95 target from 1 to 200 ms and a maximum delay from 0 to 50 ms.';
        }
        scheduling.target_read_p95_us = Number(values.target_p95_ms) * 1000;
        scheduling.maximum_delay_us = Number(values.max_delay_ms) * 1000;
      }
      const result = await run({
        label: 'Preparing rebuild',
        task: () => post(['rebuilds'], { array_id: array, failed_disk_id: failedDisk, spare_disk_id: spareDisk,
          ...(policy === 'FULL_SPEED' ? {} : { scheduling }) }),
        success: (job) => job.job_id + ' prepared. Open the job to start reconstruction.',
        roots: ['rebuilds', 'spares', 'raids'], uncertainTarget: 'rebuild jobs for ' + array,
      });
      if (result) { onClose(); navigate('/rebuilds/' + result.job_id); }
    }}>
    <Field label="Degraded RAID array"><select value={array} onChange={(event) => { setArray(event.target.value); setFailedDisk(''); }}>
      <option value="">Choose an array</option>{degraded?.map((item) => <option key={item.array_id}>{item.array_id}</option>)}
    </select></Field>
    <Field label="Failed member"><select value={failedDisk} disabled={!failed?.length} onChange={(event) => setFailedDisk(event.target.value)}>
      <option value="">Choose a member</option>{failed?.map((item) => <option key={item.disk_id}>{item.disk_id}</option>)}
    </select></Field>
    <Field label="Replacement spare"><select value={spareDisk} onChange={(event) => setSpareDisk(event.target.value)}>
      <option value="">Choose an available spare</option>
      {available?.map((item) => <option key={item.disk_id} value={item.disk_id}>{item.disk_id} · {bytes(item.capacity_bytes)}</option>)}
    </select></Field>
    <Field label="Scheduling policy"><select value={policy} onChange={(event) => setPolicy(event.target.value)}>
      {Object.entries(policyNames).map(([value, label]) => <option key={value} value={value}>{label}</option>)}
    </select></Field>
    {policy === 'FIXED_THROTTLE' && <Field label="Inter-batch delay (ms)">
      <input name="fixed_delay_ms" type="number" min="1" max="50" defaultValue="3" required />
    </Field>}
    {policy === 'ADAPTIVE' && <>
      <Field label="Read p95 target (ms)"><input name="target_p95_ms" type="number" min="1" max="200" defaultValue="8" required /></Field>
      <Field label="Maximum inter-batch delay (ms)">
        <input name="max_delay_ms" type="number" min="0" max="50" defaultValue="6" required />
      </Field>
    </>}
    {(arrays.isError || spares.isError || members.isError) && <Notice tone="warning">Eligibility information is unavailable. Refresh before preparing the job.</Notice>}
    {degraded?.length === 0 && <Notice>No degraded arrays are eligible.</Notice>}
    {available?.length === 0 && <Notice>No replacement spares are available. Register one in Hot Spares.</Notice>}
    {array && failed?.length === 0 && <Notice>This array has no eligible failed members.</Notice>}
  </OperationForm>;
}

function JobState({ job }) {
  return <div className="job-state">
    <Badge value={job.status} />
    {job.pause_requested && <span>Pause requested</span>}
    {job.worker_active && job.status !== 'RUNNING' && !job.pause_requested && <span>Worker active</span>}
    {job.worker_error && <span className="danger-text">Worker reported an error</span>}
  </div>;
}

export function Rebuilds() {
  const jobs = useRebuildJobs();
  const [open, setOpen] = useState(false);
  const { locked, setFeedback } = useOperations();
  return <Page title="Rebuild monitor" description="Track durable reconstruction checkpoints and control active RAID rebuilds."
    action={<Button variant="primary" icon={Plus} disabled={locked} onClick={() => { setFeedback(null); setOpen(true); }}>Prepare rebuild</Button>}>
    <Panel title="Rebuild jobs" description="Progress reflects physical blocks recorded by the storage engine.">
      <QueryState query={jobs} empty="No rebuild jobs yet. Prepare a job after registering a spare.">{(items) => <Table label="Rebuild jobs"
        rows={items.map((job) => ({ ...job, id: job.job_id }))} columns={[
          { key: 'id', title: 'Job', render: (job) => <RowLink to={'/rebuilds/' + job.id}>{job.id}</RowLink> },
          { key: 'array_id', title: 'Array', render: (job) => <TextLink to={'/raids/' + job.array_id}>{job.array_id}</TextLink> },
          { key: 'failed_disk_id', title: 'Failed member' }, { key: 'spare_disk_id', title: 'Replacement' },
          { key: 'status', title: 'State', render: (job) => <JobState job={job} /> },
          { key: 'policy', title: 'Policy', render: (job) => policyNames[job.scheduling?.policy ?? 'FULL_SPEED'] },
          { key: 'progress', title: 'Progress', render: (job) => <Progress job={job} /> },
          { key: 'started_at', title: 'Started', render: (job) => date(job.started_at) },
        ]} />}</QueryState>
    </Panel>
    {open && <Prepare onClose={() => setOpen(false)} />}
  </Page>;
}

export function Rebuild() {
  const { id } = useParams();
  return <RebuildDetails key={id} id={id} />;
}

function RebuildDetails({ id }) {
  const jobs = useRebuildJobs();
  const detail = useResource(['rebuilds', id]);
  const listed = jobs.data?.find((item) => item.job_id === id);
  const job = detail.dataUpdatedAt > jobs.dataUpdatedAt ? detail.data : listed || detail.data;
  const scheduler = useResource(['rebuilds', id, 'scheduler'], {
    enabled: Boolean(job?.scheduling), interval: job && activeJob(job) ? 1800 : false,
  });
  const { locked, busy, run, setFeedback } = useOperations();
  const [confirm, setConfirm] = useState('');
  const canStart = job?.status === 'PREPARED' && !activeJob(job);
  const canPause = job && activeJob(job) && !job.pause_requested;
  const canResume = ['PAUSED', 'INTERRUPTED'].includes(job?.status) && !activeJob(job);
  const button = confirm === 'resume' ? 'Resume rebuild' : 'Start rebuild';

  async function change(action, body) {
    return run({
      label: { start: 'Starting rebuild', pause: 'Requesting pause', resume: 'Resuming rebuild' }[action],
      task: () => post(['rebuilds', id, action], body),
      success: () => ({ start: 'Start requested. Check the job state for worker progress.',
        pause: 'Pause requested. The worker stops at a durable checkpoint.',
        resume: 'Resume requested. Check the job state for worker progress.' })[action],
      roots: ['rebuilds', 'raids', 'spares', 'pools'], uncertainTarget: id + ' in Rebuild Monitor',
    });
  }

  return <Page title={id} description="Monitor and control reconstruction of the replacement member." back={['/rebuilds', 'Rebuild Monitor']}>
    <Panel title="Reconstruction progress" action={job && <JobState job={job} />}>
      {jobs.isError && <Notice tone="warning">Live checkpoint updates are unavailable. Refresh to reconnect.</Notice>}
      <QueryState query={{ ...detail, data: job }}>{(item) => <div className="panel-body rebuild-detail">
        <div className="rebuild-graphic"><Activity size={21} aria-hidden="true" /><Progress job={item} /></div>
        <Fields items={[
          ['Completed blocks', number(item.completed_blocks)], ['Total physical blocks', number(item.total_blocks)],
          ['Remaining blocks', Number.isFinite(item.total_blocks) && Number.isFinite(item.completed_blocks) ?
            number(Math.max(0, item.total_blocks - item.completed_blocks)) : 'Unavailable'],
          ['Checkpoint batch size', number(item.batch_blocks) + ' blocks'],
        ]} />
        <div className="actions">
          {canStart && <Button icon={Play} variant="primary" disabled={locked || jobs.isError} onClick={() => { setFeedback(null); setConfirm('start'); }}>Start rebuild</Button>}
          {canPause && <Button icon={Pause} disabled={locked || jobs.isError} onClick={() => change('pause')}>
            {busy === 'Requesting pause' ? 'Requesting pause…' : 'Pause rebuild'}</Button>}
          {canResume && <Button icon={Play} variant="primary" disabled={locked || jobs.isError} onClick={() => { setFeedback(null); setConfirm('resume'); }}>Resume rebuild</Button>}
        </div>
      </div>}</QueryState>
    </Panel>
    {job?.scheduling && <Panel title="Rebuild scheduling"><QueryState query={scheduler}>{(data) => <>
      <Fields items={[
        ['Policy', policyNames[data.configuration.policy]],
        ['Current inter-batch delay', data.current ? ms(data.current.delay_us) : 'Unavailable'],
        ['Foreground pressure', data.current?.pressure ?? 'Unavailable'],
        ['Outstanding foreground reads', data.current?.outstanding == null ? 'Unavailable' : number(data.current.outstanding)],
        ['Latest foreground p95', data.current?.p95_ms == null ? 'Unavailable' : `${number(data.current.p95_ms)} ms`],
      ]} />
      {Boolean(data.decisions.length) && <Table label="Recent scheduler decisions"
        rows={data.decisions.slice(-8).reverse().map((item, id) => ({ ...item, id }))} columns={[
          { key: 'elapsed_ms', title: 'Elapsed', render: (item) => `${number(item.elapsed_ms)} ms` },
          { key: 'pressure', title: 'Pressure' },
          { key: 'delay_us', title: 'Delay', render: (item) => ms(item.delay_us) },
          { key: 'completed_blocks', title: 'Checkpoint blocks' },
          { key: 'reason', title: 'Reason', render: (item) => item.reason.replaceAll('_', ' ') },
        ]} />}
    </>}</QueryState></Panel>}
    <Panel title="Job details"><QueryState query={{ ...detail, data: job }}>{(item) => <Fields items={[
      ['RAID array', <TextLink to={'/raids/' + item.array_id}>{item.array_id}</TextLink>],
      ['Failed member', <TextLink to={'/disks/' + item.failed_disk_id}>{item.failed_disk_id}</TextLink>],
      ['Replacement spare', <TextLink to={'/disks/' + item.spare_disk_id}>{item.spare_disk_id}</TextLink>],
      ['Started', date(item.started_at)], ['Last checkpoint', date(item.checkpoint_at)],
      ['Finished at', 'Unavailable'], ['Worker error', item.worker_error === true ? 'Reported by the engine' : item.worker_error === false ? 'None reported' : 'Unavailable'],
    ]} />}</QueryState></Panel>
    {confirm && <OperationForm title={button} submit={button} description={'Reconstruct ' + (job?.failed_disk_id || 'the failed member') +
      ' using spare ' + (job?.spare_disk_id || 'assigned to this job') + '.'}
      onClose={() => setConfirm('')} disabled={confirm === 'start' ? !canStart : !canResume}
      onSubmit={async ({ checkpoint_limit }) => {
        if (checkpoint_limit && !integer(checkpoint_limit, 1, maxLba)) return 'Enter a positive whole number of checkpoint batches.';
        const result = await change(confirm, checkpoint_limit ? { checkpoint_limit: Number(checkpoint_limit) } : {});
        if (result) setConfirm('');
      }}>
      <Notice tone="warning">The engine writes reconstructed blocks onto the assigned replacement disk.</Notice>
      <Field label="Pause after checkpoint batches (optional)" hint="Leave blank to continue until completion or a pause request.">
        <input name="checkpoint_limit" type="number" min="1" step="1" />
      </Field>
    </OperationForm>}
  </Page>;
}
