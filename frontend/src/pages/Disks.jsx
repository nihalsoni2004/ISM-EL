import { Plus } from 'lucide-react';
import { useState } from 'react';
import { useParams } from 'react-router-dom';
import { post, useOperations, useResource } from '../api/state';
import { Badge, Button, Field, Fields, Notice, OperationForm, Page, Panel, QueryState, RowLink, Table, TextLink } from '../components/UI';
import { bytes, integer, number, unusedDisks, validId } from '../storage';

function useOwnership() {
  const arrays = useResource('raids', { field: 'arrays' });
  const spares = useResource('spares', { field: 'spares' });
  return { arrays, spares, loaded: Boolean(arrays.data && spares.data && !arrays.isError && !spares.isError) };
}
function owner(id, ownership) {
  const array = ownership.arrays.data?.find((item) => item.members.includes(id));
  const spare = ownership.spares.data?.find((item) => item.disk_id === id);
  return { array, spare, label: array ? 'RAID member' : spare ? 'Hot spare' : ownership.loaded ? 'Unassigned' : 'Unavailable' };
}

export function Disks() {
  const inventory = useResource('disks', { field: 'disks' });
  const ownership = useOwnership();
  const [open, setOpen] = useState(false);
  const { locked, run, setFeedback } = useOperations();
  const close = () => setOpen(false);
  return <Page title="Virtual disks" description="Manage the file-backed disks used by RAID arrays and hot spares."
    action={<Button variant="primary" icon={Plus} disabled={locked} onClick={() => { setFeedback(null); setOpen(true); }}>Create disk</Button>}>
    <Panel title="Disk inventory">
      {(ownership.arrays.isError || ownership.spares.isError) && <Notice tone="warning">Some disk ownership information is unavailable.</Notice>}
      <QueryState query={inventory} empty="No virtual disks yet. Create a disk to begin.">{(disks) =>
        <Table label="Virtual disk inventory" rows={disks.map((disk) => ({ ...disk, id: disk.disk_id, owner: owner(disk.disk_id, ownership) }))}
          columns={[
            { key: 'id', title: 'Disk', render: (row) => <RowLink to={'/disks/' + row.id}>{row.id}</RowLink> },
            { key: 'capacity', title: 'Capacity', render: (row) => bytes(row.capacity_bytes) },
            { key: 'blocks', title: 'Blocks', render: (row) => number(row.block_count) },
            { key: 'ownership', title: 'Ownership', render: (row) => row.owner.label },
            { key: 'array', title: 'RAID array', render: (row) => row.owner.array && <TextLink to={'/raids/' + row.owner.array.array_id}>{row.owner.array.array_id}</TextLink> },
            { key: 'state', title: 'State', render: (row) => <Badge value={row.owner.spare?.state} label={row.owner.label === 'Unassigned' ? 'Unassigned' : row.owner.array ? 'See RAID member' : undefined} /> },
          ]} />}
      </QueryState>
    </Panel>
    {open && <OperationForm title="Create virtual disk" description="Create a file-backed image for storage operations." onClose={close} submit="Create disk"
      onSubmit={async ({ disk_id, capacity_mib }) => {
        disk_id = disk_id.trim();
        if (!validId(disk_id, 'disk')) return 'Enter a disk identifier such as disk12.';
        if (!integer(capacity_mib, 1, 8796093022207)) return 'Enter a positive whole capacity in MiB within the supported range.';
        const result = await run({ label: 'Creating disk', task: () => post(['disks'], { disk_id, capacity_mib: Number(capacity_mib) }),
          success: (disk) => disk.disk_id + ' created with ' + bytes(disk.capacity_bytes) + ' capacity.', roots: ['disks'], uncertainTarget: disk_id + ' in Virtual Disks' });
        if (result) close();
      }}>
      <Field label="Disk identifier"><input name="disk_id" placeholder="disk12" required /></Field>
      <Field label="Capacity (MiB)"><input type="number" name="capacity_mib" min="1" step="1" required /></Field>
    </OperationForm>}
  </Page>;
}

export function Disk() {
  const { id } = useParams();
  const disk = useResource(['disks', id]);
  const ownership = useOwnership();
  const entry = owner(id, ownership);
  const member = useResource(['raids', entry.array?.array_id || '', 'members'], { field: 'members', enabled: Boolean(entry.array) });
  const state = member.data?.find((item) => item.disk_id === id)?.state || entry.spare?.state;
  return <Page title={id} description="Virtual disk geometry and storage ownership." back={['/disks', 'Virtual Disks']}>
    <Panel title="Disk details"><QueryState query={disk}>{(data) => <Fields items={[
      ['Disk ID', data.disk_id], ['Capacity', bytes(data.capacity_bytes)], ['Block size', bytes(data.block_size)],
      ['Block count', number(data.block_count)], ['Ownership', entry.label],
      ['RAID array', entry.array ? <TextLink to={'/raids/' + entry.array.array_id}>{entry.array.array_id}</TextLink> : '—'],
      ['State', <Badge value={state} label={entry.label === 'Unassigned' ? 'Unassigned' : undefined} />],
    ]} />}</QueryState></Panel>
  </Page>;
}

export function Spares() {
  const ownership = useOwnership();
  const [open, setOpen] = useState(false);
  const disks = useResource('disks', { field: 'disks', enabled: open });
  const choices = ownership.loaded && !disks.isError ? unusedDisks(disks.data, ownership.arrays.data, ownership.spares.data) : undefined;
  const { locked, run, setFeedback } = useOperations();
  const close = () => setOpen(false);
  return <Page title="Hot spares" description="Register unassigned virtual disks for future RAID reconstruction."
    action={<Button variant="primary" icon={Plus} disabled={locked} onClick={() => { setFeedback(null); setOpen(true); }}>Register spare</Button>}>
    <Panel title="Spare inventory">
      <QueryState query={ownership.spares} empty="No hot spares are registered.">{(items) => <Table label="Hot spare inventory"
        rows={items.map((item) => ({ ...item, id: item.disk_id }))} columns={[
          { key: 'id', title: 'Spare', render: (row) => <RowLink to={'/disks/' + row.id}>{row.id}</RowLink> },
          { key: 'capacity', title: 'Capacity', render: (row) => bytes(row.capacity_bytes) },
          { key: 'state', title: 'State', render: (row) => <Badge value={row.state} /> },
          { key: 'available', title: 'Availability', render: (row) => row.available === true ? 'Available' : row.available === false ? 'Unavailable' : '—' },
          { key: 'assignment', title: 'Rebuild assignment', render: (row) => row.assignment ? <TextLink to={'/rebuilds/' + row.assignment}>{row.assignment}</TextLink> : 'Unassigned' },
        ]} />}</QueryState>
    </Panel>
    {open && <OperationForm title="Register hot spare" description="Reserve an unassigned blank virtual disk for use as a replacement."
      submit="Register spare" onClose={close} disabled={!choices?.length} onSubmit={async ({ disk_id }) => {
        if (!choices?.some((disk) => disk.disk_id === disk_id)) return 'Choose an eligible virtual disk.';
        const result = await run({ label: 'Registering spare', task: () => post(['spares'], { disk_id }),
          success: (spare) => spare.disk_id + ' registered as a hot spare.', roots: ['spares'], uncertainTarget: disk_id + ' in Hot Spares' });
        if (result) close();
      }}>
      <Field label="Virtual disk"><select name="disk_id" defaultValue=""><option value="">Choose an unassigned disk</option>
        {choices?.map((disk) => <option key={disk.disk_id} value={disk.disk_id}>{disk.disk_id} · {bytes(disk.capacity_bytes)}</option>)}
      </select></Field>
      {!choices?.length && <Notice>{choices ? 'No unassigned disks are eligible. Create a virtual disk first.' : 'Checking available disks and ownership…'}</Notice>}
      {(disks.isError || ownership.arrays.isError || ownership.spares.isError) && <Notice tone="warning">Eligibility could not be confirmed. Refresh the inventory.</Notice>}
    </OperationForm>}
  </Page>;
}
