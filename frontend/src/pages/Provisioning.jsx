import { Plus, ShieldCheck } from 'lucide-react';
import { useState } from 'react';
import { useNavigate, useParams } from 'react-router-dom';
import { post, useOperations, useResource, verifyStorage } from '../api/state';
import { Badge, Button, Capacity, Field, Fields, LbaForm, Notice, OperationForm, Page, Panel, QueryState, RowLink, Table, TextLink } from '../components/UI';
import { bytes, integer, maxLba, number, validId } from '../storage';

const poolAddressSpace = 'Allocation describes RAID address space reserved by the pool. Host disk usage is separate.';
const thinProvisioning = 'Thin provisioning allocates physical chunks as data is written. Logical capacity can exceed currently available physical capacity.';
const poolCapacity = (pool) => ({
  capacity: pool.physical_capacity_bytes, allocated: pool.allocated_bytes, reserved: pool.reserved_bytes, free: pool.free_bytes,
});

function CreatePool({ onClose, pools }) {
  const arrays = useResource('raids', { field: 'arrays' });
  const { run } = useOperations();
  const navigate = useNavigate();
  const ready = arrays.data && pools.data && !arrays.isError && !pools.isError;
  const eligible = ready ? arrays.data.filter((array) => array.health === 'HEALTHY' &&
    !pools.data.some((pool) => pool.array_id === array.array_id)) : undefined;
  return <OperationForm title="Create storage pool" description="Claim a healthy RAID array as the pool’s backing storage."
    onClose={onClose} submit="Create pool" disabled={!eligible?.length}
    onSubmit={async ({ pool_id, array_id, chunk_blocks, max_ratio, warning_percent, critical_percent }) => {
      pool_id = pool_id.trim();
      if (!validId(pool_id, 'pool')) return 'Enter a pool identifier such as pool12.';
      if (!eligible?.some((array) => array.array_id === array_id)) return 'Choose an eligible backing array.';
      if (!integer(chunk_blocks, 1, 1024) || !integer(max_ratio, 0, maxLba)) return 'Enter a chunk size from 1–1024 blocks and a valid whole provisioning ratio.';
      if (!integer(warning_percent, 1, 99) || !integer(critical_percent, 2, 100) || Number(warning_percent) >= Number(critical_percent))
        return 'Enter percentages with a warning threshold below the critical threshold.';
      const result = await run({ label: 'Creating pool', task: () => post(['pools'], {
        pool_id, array_id, chunk_blocks: Number(chunk_blocks), max_ratio: Number(max_ratio),
        warning_percent: Number(warning_percent), critical_percent: Number(critical_percent),
      }), success: (pool) => pool.pool_id + ' created on ' + pool.array_id + '.', roots: ['pools', 'raids'], uncertainTarget: pool_id + ' and ' + array_id });
      if (result) { onClose(); navigate('/pools/' + result.pool_id); }
    }}>
    <Notice tone="warning">The selected array becomes pool-owned. Direct RAID writes are then blocked.</Notice>
    <div className="form-grid">
      <Field label="Pool identifier"><input name="pool_id" placeholder="pool12" /></Field>
      <Field label="Backing RAID array"><select name="array_id" defaultValue=""><option value="">Choose a healthy array</option>
        {eligible?.map((array) => <option key={array.array_id} value={array.array_id}>{array.array_id} · RAID {array.raid_level}</option>)}
      </select></Field>
      <Field label="Chunk size (blocks)"><input name="chunk_blocks" type="number" min="1" max="1024" defaultValue="16" /></Field>
      <Field label="Maximum provisioning ratio" hint="0 lets the engine use its unlimited-ratio setting."><input name="max_ratio" type="number" min="0" defaultValue="0" /></Field>
      <Field label="Warning threshold (%)"><input name="warning_percent" type="number" min="1" max="99" defaultValue="70" /></Field>
      <Field label="Critical threshold (%)"><input name="critical_percent" type="number" min="2" max="100" defaultValue="90" /></Field>
    </div>
    {!eligible?.length && <Notice>{eligible ? 'No unassigned healthy arrays are eligible.' : 'Waiting for array ownership information.'}</Notice>}
    {(arrays.isError || pools.isError) && <Notice tone="warning">Array eligibility could not be confirmed. Refresh the dashboard.</Notice>}
  </OperationForm>;
}

export function Pools() {
  const pools = useResource('pools', { field: 'pools' });
  const [open, setOpen] = useState(false);
  const { locked, setFeedback } = useOperations();
  return <Page title="Storage pools" description="Manage RAID-backed capacity and thin-provisioning thresholds."
    action={<Button variant="primary" icon={Plus} disabled={locked} onClick={() => { setFeedback(null); setOpen(true); }}>Create pool</Button>}>
    <Panel title="Pool inventory" description={poolAddressSpace}>
      <QueryState query={pools} empty="No storage pools yet. Create a pool from an eligible RAID array.">{(items) => <Table label="Storage pool inventory"
        rows={items.map((pool) => ({ ...pool, id: pool.pool_id }))} columns={[
          { key: 'id', title: 'Pool', render: (row) => <RowLink to={'/pools/' + row.id}>{row.id}</RowLink> },
          { key: 'array_id', title: 'Backing array', render: (row) => <TextLink to={'/raids/' + row.array_id}>{row.array_id}</TextLink> },
          { key: 'physical', title: 'Physical', render: (row) => bytes(row.physical_capacity_bytes) },
          { key: 'allocated', title: 'Allocated', render: (row) => bytes(row.allocated_bytes) },
          { key: 'free', title: 'Free', render: (row) => bytes(row.free_bytes) },
          { key: 'provisioned', title: 'Provisioned', render: (row) => bytes(row.provisioned_bytes) },
          { key: 'utilization', title: 'Utilization', render: (row) => <Capacity summary {...poolCapacity(row)} /> },
          { key: 'state', title: 'Capacity state', render: (row) => <Badge value={row.capacity_state} /> },
        ]} />}</QueryState>
    </Panel>
    {open && <CreatePool pools={pools} onClose={() => setOpen(false)} />}
  </Page>;
}

export function Pool() {
  const { id } = useParams();
  return <PoolDetails key={id} id={id} />;
}

function PoolDetails({ id }) {
  const pool = useResource(['pools', id]);
  const [verified, setVerified] = useState(null);
  const { locked, busy, run } = useOperations();
  async function verify() {
    setVerified(null);
    const result = await run({ label: 'Verifying pool', task: () => verifyStorage(['pools', id]), roots: ['pools', 'raids'],
      uncertainTarget: id + ' and its backing array' });
    if (result) setVerified(result.passed ? 'PASSED' : 'FAILED');
  }
  return <Page title={id} description={poolAddressSpace} back={['/pools', 'Storage Pools']}
    action={<Button icon={ShieldCheck} disabled={locked || !pool.data} onClick={verify}>{busy === 'Verifying pool' ? 'Verifying…' : 'Verify pool'}</Button>}>
    {verified && <div className="verification-result" role="status">Last verification <Badge value={verified} /></div>}
    <div className="two-column detail-grid">
      <Panel title="Capacity and allocation"><QueryState query={pool}>{(data) => <div className="panel-body">
        <Capacity {...poolCapacity(data)} />
        <Fields items={[
          ['Physical capacity', bytes(data.physical_capacity_bytes)], ['Logically provisioned', bytes(data.provisioned_bytes)],
          ['Capacity state', <Badge value={data.capacity_state} />],
        ]} />
      </div>}</QueryState></Panel>
      <Panel title="Pool configuration"><QueryState query={pool}>{(data) => <Fields items={[
        ['Backing array', <TextLink to={'/raids/' + data.array_id}>{data.array_id}</TextLink>],
        ['Array health', <Badge value={data.health} />], ['Chunk size', bytes(data.chunk_bytes)], ['Total chunks', number(data.total_chunks)],
        ['Warning threshold', data.warning_percent == null ? null : data.warning_percent + '%'],
        ['Critical threshold', data.critical_percent == null ? null : data.critical_percent + '%'],
      ]} />}</QueryState></Panel>
    </div>
  </Page>;
}

function CreateLun({ onClose, pools }) {
  const { run } = useOperations();
  const navigate = useNavigate();
  const choices = pools.isError ? undefined : pools.data;
  return <OperationForm title="Create logical volume" description={thinProvisioning} onClose={onClose} submit="Create volume"
    disabled={!choices?.length} onSubmit={async ({ lun_id, pool_id, capacity_mib, mode }) => {
      lun_id = lun_id.trim();
      if (!validId(lun_id, 'lun')) return 'Enter a volume identifier such as lun12.';
      if (!choices?.some((pool) => pool.pool_id === pool_id)) return 'Choose an available parent pool.';
      if (!integer(capacity_mib, 1, 17592186044415)) return 'Enter a positive whole capacity in MiB within the supported range.';
      const result = await run({ label: 'Creating volume', task: () => post(['luns'], { lun_id, pool_id, capacity_mib: Number(capacity_mib), mode }),
        success: (lun) => lun.lun_id + ' created with ' + bytes(lun.capacity_bytes) + ' provisioned capacity.',
        roots: ['luns', 'pools'], uncertainTarget: lun_id + ' in Logical Volumes' });
      if (result) { onClose(); navigate('/luns/' + result.lun_id); }
    }}>
    <div className="form-grid">
      <Field label="Volume identifier"><input name="lun_id" placeholder="lun12" /></Field>
      <Field label="Parent storage pool"><select name="pool_id" defaultValue=""><option value="">Choose a pool</option>
        {choices?.map((pool) => <option key={pool.pool_id}>{pool.pool_id}</option>)}
      </select></Field>
      <Field label="Provisioned capacity (MiB)"><input name="capacity_mib" type="number" min="1" step="1" /></Field>
      <Field label="Volume mode"><select name="mode" defaultValue="file"><option value="file">File-managed</option><option value="raw">Raw blocks</option></select></Field>
    </div>
    {!choices?.length && <Notice>{choices ? 'Create a storage pool before creating a volume.' : 'Parent pool inventory is unavailable.'}</Notice>}
  </OperationForm>;
}

const modeLabel = (mode) => mode === 'file' ? 'File-managed' : mode === 'raw' ? 'Raw blocks' : 'Unavailable';
function physical(lun, pool) {
  const size = pool?.chunk_bytes;
  return Number.isFinite(size) && Number.isFinite(lun.allocated_chunks) ? bytes(size * lun.allocated_chunks) : 'Unavailable';
}

export function Luns() {
  const luns = useResource('luns', { field: 'luns' });
  const pools = useResource('pools', { field: 'pools' });
  const [open, setOpen] = useState(false);
  const { locked, setFeedback } = useOperations();
  return <Page title="Logical volumes" description={thinProvisioning}
    action={<Button variant="primary" icon={Plus} disabled={locked} onClick={() => { setFeedback(null); setOpen(true); }}>Create volume</Button>}>
    <Panel title="Volume inventory">
      {pools.isError && <Notice tone="warning">Parent pool geometry is unavailable. Allocated physical sizes could not be refreshed.</Notice>}
      <QueryState query={luns} empty="No logical volumes yet. Create a volume in a storage pool.">{(items) => <Table label="Logical volume inventory"
        rows={items.map((lun) => ({ ...lun, id: lun.lun_id }))} columns={[
          { key: 'id', title: 'Volume', render: (row) => <RowLink to={'/luns/' + row.id}>{row.id}</RowLink> },
          { key: 'pool_id', title: 'Pool', render: (row) => <TextLink to={'/pools/' + row.pool_id}>{row.pool_id}</TextLink> },
          { key: 'logical', title: 'Provisioned', render: (row) => bytes(row.capacity_bytes) },
          { key: 'physical', title: 'Allocated physical', render: (row) => physical(row, pools.data?.find((pool) => pool.pool_id === row.pool_id)) },
          { key: 'chunks', title: 'Allocated chunks', render: (row) => number(row.allocated_chunks) },
          { key: 'mode', title: 'Mode', render: (row) => modeLabel(row.mode) },
        ]} />}</QueryState>
    </Panel>
    {open && <CreateLun pools={pools} onClose={() => setOpen(false)} />}
  </Page>;
}

function LunMapping({ lun }) {
  const [lba, setLba] = useState(null);
  const map = useResource(['luns', lun.lun_id, 'map', lba ?? 0], { enabled: lba != null });
  return <Panel title="Logical block mapping" description="Inspect the physical chunk behind a logical block.">
    <LbaForm capacity={lun.block_count} fetching={map.isFetching}
      onMap={(next) => { if (next === lba) map.refetch({ cancelRefetch: false }); else setLba(next); }} />
    {lba != null && <QueryState query={map}>{(data) => <div className="mapping">
      {data.physical_chunk === null && <Notice>This logical block is unallocated.</Notice>}
      <Fields items={[
        ['Logical LBA', number(data.lba)], ['Logical chunk', number(data.logical_chunk)], ['Offset in chunk', number(data.offset)],
        ['Physical chunk', data.physical_chunk === null ? 'Unallocated' : number(data.physical_chunk)],
        ['Backing RAID LBA', data.raid_lba === null ? 'Unallocated' : number(data.raid_lba)],
      ]} />
    </div>}</QueryState>}
  </Panel>;
}

export function Lun() {
  const { id } = useParams();
  const lun = useResource(['luns', id]);
  const pool = useResource(['pools', lun.data?.pool_id || ''], { enabled: Boolean(lun.data?.pool_id) });
  return <Page title={id} description="Inspect thin-provisioned capacity and logical block allocation." back={['/luns', 'Logical Volumes']}>
    <Panel title="Volume details"><QueryState query={lun}>{(data) => <Fields items={[
      ['Parent pool', <TextLink to={'/pools/' + data.pool_id}>{data.pool_id}</TextLink>],
      ['Mode', modeLabel(data.mode)], ['Provisioned capacity', bytes(data.capacity_bytes)],
      ['Allocated physical', physical(data, pool.data)], ['Logical blocks', number(data.block_count)],
      ['Allocated chunks', number(data.allocated_chunks)], ['Chunk size', number(data.chunk_blocks) + ' blocks'],
    ]} />}</QueryState></Panel>
    {lun.data && <LunMapping key={id} lun={lun.data} />}
  </Page>;
}
