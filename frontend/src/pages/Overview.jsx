import { Activity, ArrowRight, FileText, HardDrive, Layers3, Plus, Server } from 'lucide-react';
import { Link } from 'react-router-dom';
import { activeJob, useOperations, useRebuildJobs, useResource } from '../api/state';
import { Badge, Capacity, Metric, Notice, Page, Panel, Progress, QueryState, RowLink, Table, TextLink } from '../components/UI';
import { bytes, healthStates, number, rawCapacity, total } from '../storage';

const display = (value) => value == null ? null : number(value);
const heading = { HEALTHY: 'Healthy', DEGRADED: 'Degraded', FAILED: 'Failed', INCONSISTENT: 'Inconsistent' };

export default function Overview() {
  const disks = useResource('disks', { field: 'disks' });
  const arrays = useResource('raids', { field: 'arrays' });
  const pools = useResource('pools', { field: 'pools' });
  const jobs = useRebuildJobs();
  const files = useResource('files', { field: 'files' });
  const { locked } = useOperations();
  const hasHealth = arrays.data && arrays.data.every((array) => healthStates.includes(array.health));
  const counts = hasHealth ? Object.fromEntries(healthStates.map((state) => [state, arrays.data.filter((array) => array.health === state).length])) : null;
  const capacity = total(pools.data, 'physical_capacity_bytes');
  const provisioned = total(pools.data, 'provisioned_bytes');
  const usable = total(arrays.data, 'capacity_bytes');
  const active = jobs.data?.filter(activeJob);
  const paused = jobs.data?.filter((job) => job.status === 'PAUSED').length;
  const completed = jobs.data?.filter((job) => job.status === 'COMPLETED').length;
  const allocated = total(pools.data, 'allocated_bytes');
  const raw = disks.data && total(arrays.data?.map((array) => ({ capacity: rawCapacity(array, disks.data) })), 'capacity');
  const unavailable = [disks, arrays, pools, jobs, files].filter((query) => query.isError).length;

  return <Page title="Storage overview" description="Monitor RAID health, capacity allocation, and rebuild activity across your storage system."
    action={locked ? <Link className="button secondary" to="/raids">View arrays <ArrowRight size={16} /></Link> :
      <Link className="button primary" to="/raids?create=1"><Plus size={17} /> Create array</Link>}>
    {unavailable > 0 && <Notice tone="warning">Some dashboard data could not be refreshed. Unavailable metrics are labeled below.</Notice>}
    <div className="metrics-grid">
      <Metric title="Virtual disks" value={display(disks.data?.length)} icon={HardDrive} subtitle={disks.data ? bytes(total(disks.data, 'capacity_bytes')) + ' raw capacity' : 'Disk inventory'} />
      <Metric title="RAID arrays" value={display(arrays.data?.length)} icon={Server} accent="blue" subtitle={usable == null ? 'Usable capacity unavailable' : bytes(usable) + ' usable capacity'} />
      <Metric title="Pool capacity" value={capacity == null ? null : bytes(capacity)} icon={Layers3} accent="purple" subtitle={provisioned == null ? 'Provisioned capacity unavailable' : bytes(provisioned) + ' logically provisioned'} />
      <Metric title="Active rebuilds" value={display(active?.length)} icon={Activity} accent="amber" subtitle={paused == null ? 'Rebuild status' : number(paused) + ' paused · ' + number(completed) + ' completed'} />
    </div>
    <div className="two-column">
      <Panel title="RAID health" description="Current state of registered arrays" action={<TextLink to="/raids" arrow>View arrays</TextLink>}>
        <QueryState query={arrays} empty="No RAID arrays yet. Create an array to see its health.">{(items) => counts ?
          <div className="health-summary">
            <div className="health-count"><strong>{number(items.length)}</strong><span>Total arrays</span></div>
            <div className="health-bar" role="img" aria-label={healthStates.map((state) => counts[state] + ' ' + heading[state]).join(', ')}>
              {healthStates.map((state) => <span key={state} className={state.toLowerCase()} style={{ flexGrow: counts[state] }} />)}
            </div>
            <div className="health-legend">{healthStates.map((state) =>
              <div key={state}><span><i className={state.toLowerCase()} />{heading[state]}</span><strong>{counts[state]}</strong></div>)}</div>
          </div> : <div className="no-data">Array health unavailable</div>}</QueryState>
      </Panel>
      <Panel title="Storage utilization" description="Allocation across all storage pools" action={<TextLink to="/pools" arrow>View pools</TextLink>}>
        <QueryState query={pools} empty="No storage pools yet. Pool allocation will appear here.">
          {() => <Capacity donut capacity={capacity} allocated={allocated} reserved={total(pools.data, 'reserved_bytes')} free={total(pools.data, 'free_bytes')} />}
        </QueryState>
      </Panel>
    </div>
    <Panel title="Rebuild activity" description="Checkpoint progress for current and completed jobs"
      action={<TextLink to="/rebuilds" arrow>Open monitor</TextLink>}>
      <QueryState query={jobs} empty="No rebuild jobs have been prepared.">{(items) =>
        <Table label="Rebuild activity" rows={[...items].sort((a, b) => Number(activeJob(b)) - Number(activeJob(a))).slice(0, 5).map((job) => ({ ...job, id: job.job_id }))}
          columns={[
            { key: 'id', title: 'Job', render: (job) => <RowLink to={'/rebuilds/' + job.id}>{job.id}</RowLink> },
            { key: 'array_id', title: 'Array' }, { key: 'spare_disk_id', title: 'Replacement' },
            { key: 'status', title: 'State', render: (job) => <Badge value={job.status} /> },
            { key: 'progress', title: 'Progress', render: (job) => <Progress job={job} /> },
          ]} />}
      </QueryState>
    </Panel>
    <Panel className="inventory-strip">
      <div className="inventory-item"><FileText aria-hidden="true" size={19} /><div><strong>{files.data ? number(files.data.filter((file) => file.status === 'COMPLETED').length) : 'Unavailable'}</strong><span>Imported files</span></div></div>
      <div className="inventory-item"><Layers3 aria-hidden="true" size={19} /><div><strong>{allocated == null ? 'Unavailable' : bytes(allocated)}</strong><span>Pool space allocated</span></div></div>
      <div className="inventory-item"><Server aria-hidden="true" size={19} /><div><strong>{raw == null ? 'Unavailable' : bytes(raw)}</strong><span>Raw RAID capacity</span></div></div>
      <TextLink to="/files" arrow>Open File Explorer</TextLink>
    </Panel>
  </Page>;
}
