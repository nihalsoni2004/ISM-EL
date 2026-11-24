import { AlertTriangle, Plus, ShieldCheck } from 'lucide-react';
import { useState } from 'react';
import { useNavigate, useParams, useSearchParams } from 'react-router-dom';
import { post, useOperations, useResource, verifyStorage } from '../api/state';
import { Badge, Button, Field, Fields, LbaForm, Notice, OperationForm, Page, Panel, QueryState, RowLink, Table } from '../components/UI';
import { bytes, integer, maxLba, number, rawCapacity, unusedDisks, validId } from '../storage';

const memberMinimums = { 0: 2, 1: 2, 5: 3, 6: 4 };
const roleNames = { data: 'Data', mirror: 'Mirror', p: 'P parity', q: 'Q parity' };

function CreateRaid({ onClose }) {
  const [level, setLevel] = useState('5');
  const [members, setMembers] = useState([]);
  const disks = useResource('disks', { field: 'disks' });
  const arrays = useResource('raids', { field: 'arrays' });
  const spares = useResource('spares', { field: 'spares' });
  const hasError = disks.isError || arrays.isError || spares.isError;
  const choices = !hasError ? unusedDisks(disks.data, arrays.data, spares.data) : undefined;
  const { run } = useOperations();
  const navigate = useNavigate();
  const minimum = memberMinimums[level];
  return <OperationForm title="Create RAID array" description="Select existing unassigned disks. The order you select them determines member order."
    onClose={onClose} submit="Create array" disabled={!choices?.length}
    onSubmit={async ({ array_id, stripe_unit_blocks }) => {
      array_id = array_id.trim();
      if (!validId(array_id, 'array')) return 'Enter an array identifier such as array12.';
      if (members.length < minimum || (level === '1' && members.length !== 2))
        return 'RAID ' + level + ' requires ' + (level === '1' ? 'exactly' : 'at least') + ' ' + minimum + ' disks.';
      if (members.length > 257 || !members.every((id) => choices?.some((disk) => disk.disk_id === id))) return 'Select eligible disks from the current inventory.';
      if (level !== '1' && stripe_unit_blocks && !integer(stripe_unit_blocks, 1, maxLba)) return 'Enter a positive whole stripe unit in blocks.';
      const body = { array_id, raid_level: Number(level), members };
      if (level !== '1' && stripe_unit_blocks) body.stripe_unit_blocks = Number(stripe_unit_blocks);
      const result = await run({ label: 'Creating array', task: () => post(['raids'], body),
        success: (array) => array.array_id + ' created as RAID ' + array.raid_level + '.',
        roots: ['raids'], uncertainTarget: array_id + ' in RAID Arrays' });
      if (result) { onClose(); navigate('/raids/' + result.array_id); }
    }}>
    <div className="form-grid">
      <Field label="Array identifier"><input name="array_id" placeholder="array12" /></Field>
      <Field label="RAID level"><select value={level} onChange={(event) => setLevel(event.target.value)}>
        {[0, 1, 5, 6].map((item) => <option key={item} value={item}>RAID {item}</option>)}
      </select></Field>
    </div>
    {level !== '1' && <Field label="Stripe unit (blocks)" hint="Leave blank for the engine default.">
      <input name="stripe_unit_blocks" type="number" step="1" min="1" />
    </Field>}
    <fieldset className="picker"><legend>Member disks</legend>
      <div className="picker-list">
        {choices?.map((disk) => <label key={disk.disk_id}>
          <input type="checkbox" checked={members.includes(disk.disk_id)} onChange={(event) =>
            setMembers((current) => event.target.checked ? [...current, disk.disk_id] : current.filter((id) => id !== disk.disk_id))} />
          <strong>{disk.disk_id}</strong><span>{bytes(disk.capacity_bytes)}</span>
        </label>)}
        {!choices?.length && <p>{hasError ? 'Disk eligibility is unavailable. Refresh the inventory.' : choices ? 'No unassigned disks are eligible.' : 'Checking disk ownership…'}</p>}
      </div>
      <small>{members.length ? 'Member order · ' + members.join(' → ') : 'Select ' + (level === '1' ? 'exactly ' : 'at least ') + minimum + ' disks.'}</small>
    </fieldset>
  </OperationForm>;
}

export function Raids() {
  const arrays = useResource('raids', { field: 'arrays' });
  const disks = useResource('disks', { field: 'disks' });
  const [search, setSearch] = useSearchParams();
  const { locked, setFeedback } = useOperations();
  return <Page title="RAID arrays" description="Inspect array geometry, redundancy, member disks, and parity placement."
    action={<Button variant="primary" icon={Plus} disabled={locked} onClick={() => { setFeedback(null); setSearch({ create: '1' }); }}>Create array</Button>}>
    <Panel title="Array inventory">
      {disks.isError && <Notice tone="warning">Raw member capacity is unavailable.</Notice>}
      <QueryState query={arrays} empty="No RAID arrays yet. Create an array from unassigned disks.">{(items) =>
        <Table label="RAID array inventory" rows={items.map((array) => ({ ...array, id: array.array_id }))}
          columns={[
            { key: 'id', title: 'Array', render: (row) => <RowLink to={'/raids/' + row.id}>{row.id}</RowLink> },
            { key: 'raid_level', title: 'Level', render: (row) => 'RAID ' + row.raid_level },
            { key: 'members', title: 'Members', render: (row) => <><strong>{row.members.length}</strong><span className="cell-detail">{row.members.join(', ')}</span></> },
            { key: 'raw', title: 'Raw capacity', render: (row) => bytes(rawCapacity(row, disks.data)) },
            { key: 'usable', title: 'Usable capacity', render: (row) => bytes(row.capacity_bytes) },
            { key: 'stripe', title: 'Stripe unit', render: (row) => number(row.stripe_unit_blocks) + ' blocks' },
            { key: 'health', title: 'Health and consistency', render: (row) => <Badge value={row.health} /> },
          ]} />}
      </QueryState>
    </Panel>
    {search.get('create') === '1' && <CreateRaid onClose={() => setSearch({}, { replace: true })} />}
  </Page>;
}

function StripeMap({ array, members }) {
  const [lba, setLba] = useState(null);
  const map = useResource(['raids', array.array_id, 'map', lba ?? 0], { enabled: lba != null });
  return <Panel title="RAID block mapping" description="Inspect the member locations returned for one logical block.">
    <LbaForm capacity={array.capacity_blocks} fetching={map.isFetching}
      onMap={(next) => { if (lba === next) map.refetch({ cancelRefetch: false }); else setLba(next); }} />
    {lba != null && <QueryState query={map}>{(mapping) => <div className="mapping">
      <div className="mapping-heading"><strong>Logical block {number(mapping.lba)}</strong><span>Backend-reported member locations</span></div>
      <div className="stripe-grid">
        {[...new Set([...array.members, ...mapping.locations.map((item) => item.disk_id)])].map((id) => {
          const state = members?.find((item) => item.disk_id === id);
          const locations = mapping.locations.filter((item) => item.disk_id === id);
          return <div className={'stripe-cell' + (state?.state === 'FAILED' ? ' failed-cell' : '')} key={id}>
            <header><strong>{id}</strong>{state && <Badge value={state.state} />}</header>
            {locations.length ? locations.map((item) => <div className={'stripe-role ' + item.role} key={item.role}>
              <strong>{roleNames[item.role]}</strong><span>Physical LBA {number(item.physical_lba)}</span>
            </div>) : <div className="stripe-role none">No location for this LBA</div>}
          </div>;
        })}
      </div>
      <div className="stripe-legend" aria-label="Mapping legend">{Object.entries(roleNames).filter(([role]) =>
        mapping.locations.some((item) => item.role === role)).map(([role, label]) => <span key={role}><i className={role} />{label}</span>)}
        {members?.some((item) => item.state === 'FAILED') && <span><i className="failed" /> Failed member</span>}
      </div>
    </div>}</QueryState>}
  </Panel>;
}

export function Raid() {
  const { id } = useParams();
  return <RaidDetails key={id} id={id} />;
}
function RaidDetails({ id }) {
  const array = useResource(['raids', id]);
  const status = useResource(['raids', id, 'status']);
  const members = useResource(['raids', id, 'members'], { field: 'members' });
  const disks = useResource('disks', { field: 'disks' });
  const [target, setTarget] = useState('');
  const [confirm, setConfirm] = useState(false);
  const [verification, setVerification] = useState(null);
  const { locked, busy, run, setFeedback } = useOperations();
  const eligible = members.isError ? [] : members.data?.filter((member) => member.available && member.state === 'ONLINE') || [];
  const tolerance = status.data?.tolerated_failures;
  const failed = members.data?.filter((member) => !member.available).length;
  const verify = async () => {
    setVerification(null);
    const result = await run({ label: 'Verifying array', task: () => verifyStorage(['raids', id]), roots: ['raids'],
      uncertainTarget: id + ' and its consistency state' });
    if (result) setVerification(result.passed ? 'PASSED' : 'FAILED');
  };
  return <Page title={id} description={array.data ? 'RAID ' + array.data.raid_level + ' array diagnostics and member management.' : 'RAID array diagnostics and member management.'}
    back={['/raids', 'RAID Arrays']}>
    <div className="two-column detail-grid">
      <Panel title="Array geometry"><QueryState query={array}>{(data) => <Fields items={[
        ['Level', 'RAID ' + data.raid_level], ['Layout', data.layout], ['Raw capacity', bytes(rawCapacity(data, disks.data))],
        ['Usable capacity', bytes(data.capacity_bytes)], ['Logical blocks', number(data.capacity_blocks)],
        ['Block size', bytes(data.block_size)], ['Stripe unit', number(data.stripe_unit_blocks) + ' blocks'],
      ]} />}</QueryState></Panel>
      <Panel title="Health and consistency" action={<Button icon={ShieldCheck} disabled={locked || !array.data} onClick={verify}>
        {busy === 'Verifying array' ? 'Verifying…' : 'Verify array'}</Button>}>
        <QueryState query={status}>{(data) => <Fields items={[
          ['Array health', <Badge value={data.health} />], ['Pending write', data.pending_write === true ? 'Detected' : data.pending_write === false ? 'None reported' : 'Unavailable'],
          ['Read access', data.reads_permitted === true ? 'Permitted' : data.reads_permitted === false ? 'Blocked' : 'Unavailable'],
          ['Write access', data.writes_permitted === true ? 'Permitted' : data.writes_permitted === false ? 'Blocked' : 'Unavailable'],
          ['Pool ownership', data.pool_owned === true ? 'Owned by a storage pool' : data.pool_owned === false ? 'Unassigned' : 'Unavailable'],
          ['Tolerated member failures', number(data.tolerated_failures)],
        ]} />}</QueryState>
        {verification && <div className="verification-result" role="status">Last verification <Badge value={verification} /></div>}
      </Panel>
    </div>
    <Panel title="Array members" description="Persistent member state reported by the engine">
      <QueryState query={members} empty="No RAID members were returned.">{(items) => <Table label="Array member inspection"
        rows={items.map((item) => ({ ...item, id: item.disk_id }))} columns={[
          { key: 'id', title: 'Disk', render: (row) => <RowLink to={'/disks/' + row.id}>{row.id}</RowLink> },
          { key: 'state', title: 'State', render: (row) => <Badge value={row.state} /> },
          { key: 'cause', title: 'Cause' }, { key: 'present', title: 'Image present', render: (row) => row.present === true ? 'Yes' : row.present === false ? 'No' : '—' },
          { key: 'available', title: 'Available', render: (row) => row.available === true ? 'Yes' : row.available === false ? 'No' : '—' },
        ]} />}</QueryState>
      <div className="panel-body inline-form">
        <Field label="Member to fail"><select value={target} onChange={(event) => setTarget(event.target.value)} disabled={!eligible.length}>
          <option value="">Select an online member</option>
          {eligible.map((member) => <option key={member.disk_id}>{member.disk_id}</option>)}</select></Field>
        <Button icon={AlertTriangle} variant="danger-outline"
          disabled={locked || status.isError || !eligible.some((member) => member.disk_id === target) || tolerance == null}
          onClick={() => { setFeedback(null); setConfirm(true); }}>Simulate failure</Button>
      </div>
    </Panel>
    {array.data && <StripeMap array={array.data} members={members.data} />}
    {confirm && <OperationForm title="Confirm persistent member failure" description={target + ' will be marked failed in ' + id + '.'}
      submit="Confirm failure" danger onClose={() => setConfirm(false)} onSubmit={async () => {
        if (!eligible.some((member) => member.disk_id === target)) return 'This member is no longer eligible.';
        const result = await run({ label: 'Marking member failed', task: () => post(['raids', id, 'fail'], { disk_id: target }),
          success: (data) => target + ' marked failed. ' + id + ' is ' + data.health.toLowerCase() + '.',
          roots: ['raids', 'pools', 'spares', 'rebuilds'], uncertainTarget: target + ' in ' + id });
        if (result) { setConfirm(false); setTarget(''); setVerification(null); }
      }}>
      <Notice tone="warning">This is a persistent logical failure state. The disk image file is preserved.</Notice>
      <p>RAID 0 becomes unavailable after one member failure. RAID 1 and RAID 5 tolerate one member failure. RAID 6 tolerates two member failures.</p>
      {failed != null && <p>{failed + 1 > tolerance ? 'This change exceeds the array’s failure tolerance. Access may become unavailable.' :
        'This change consumes one unit of redundancy. Remaining tolerated failures after this action: ' + (tolerance - failed - 1) + '.'}</p>}
    </OperationForm>}
  </Page>;
}
