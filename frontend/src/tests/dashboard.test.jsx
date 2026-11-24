import { QueryClientProvider } from '@tanstack/react-query';
import { act, fireEvent, render, screen, waitFor, within } from '@testing-library/react';
import { MemoryRouter } from 'react-router-dom';
import { describe, expect, it, vi } from 'vitest';
import { DashboardRoutes } from '../App';
import { makeClient, OperationsProvider } from '../api/state';
import { bytes, rawCapacity, total, usedPercent } from '../storage';

const ready = { ready: true, engine_available: true, catalog_accessible: true, version: '1.0', process_id: 71, array_health: { HEALTHY: 1 } };
const pool = { pool_id: 'pool41', array_id: 'array41', health: 'HEALTHY', physical_capacity_bytes: 2097152,
  allocated_bytes: 524288, reserved_bytes: 524288, free_bytes: 1048576, provisioned_bytes: 4194304, chunk_bytes: 65536,
  capacity_state: 'WARNING', total_chunks: 32, warning_percent: 40, critical_percent: 90 };
const disk = (id) => ({ disk_id: 'disk' + id, capacity_bytes: 1048576, block_size: 4096, block_count: 256 });
const raid = (id, health = 'HEALTHY') => ({ array_id: 'array' + id, raid_level: 6, health, members: ['disk41', 'disk42', 'disk43', 'disk44'],
  stripe_unit_blocks: 1, capacity_blocks: 512, capacity_bytes: 2097152, block_size: 4096, layout: 'test layout' });
const member = (id, state = 'ONLINE') => ({ disk_id: 'disk' + id, state, available: state === 'ONLINE', present: true, cause: state === 'ONLINE' ? 'NONE' : 'ADMIN' });
const lun = { lun_id: 'lun41', pool_id: 'pool41', mode: 'file', capacity_bytes: 8388608, block_count: 2048, chunk_blocks: 16, allocated_chunks: 2 };
const record = { file_id: 'file41', lun_id: 'lun41', name: 'binary.dat', size_bytes: 3, start_lba: 0, block_count: 1,
  sha256: 'a'.repeat(64), status: 'COMPLETED', created_at: '2026-11-24T10:20:30', updated_at: '2026-11-24T10:20:30' };
const responses = {
  'GET /health': { status: 'ok', service: 'raid-simulator-backend' },
  'GET /api/engine/status': ready,
  'GET /api/version': { version: '0.1.0' },
  'GET /api/disks': { disks: [disk(41), disk(42), disk(43), disk(44)] },
  'GET /api/spares': { spares: [] },
  'GET /api/raids': { arrays: [] },
  'GET /api/pools': { pools: [] },
  'GET /api/rebuilds': { jobs: [] },
  'GET /api/luns': { luns: [lun] },
  'GET /api/files': { files: [] },
};
const json = (value, status = 200) => new Response(JSON.stringify(value), {
  status, headers: { 'content-type': 'application/json' },
});
const error = (code, status) => json({ error: { code, message: 'C:/private/internal traceback', request_id: 'req-123' } }, status);

function setup(path, overrides = {}) {
  const calls = [];
  vi.stubGlobal('fetch', vi.fn(async (url, options) => {
    const key = options.method + ' ' + url;
    const payload = options.body ? JSON.parse(options.body) : undefined;
    calls.push({ key, payload });
    const handler = key in overrides ? overrides[key] : responses[key];
    if (handler === undefined) return error('MISSING_TEST_ROUTE', 404);
    if (typeof handler === 'function') return handler(payload, options);
    return json(handler);
  }));
  render(<QueryClientProvider client={makeClient()}><OperationsProvider>
    <MemoryRouter initialEntries={[path]}><DashboardRoutes /></MemoryRouter>
  </OperationsProvider></QueryClientProvider>);
  return calls;
}
const tap = (name, root = screen) => fireEvent.click(root.getByRole('button', { name }));
const input = (label, value, root = screen) => fireEvent.change(root.getByLabelText(label), { target: { value } });
const online = () => screen.findByText('Engine ready');
const states = (calls, key) => calls.filter((call) => call.key === key);
const dialog = () => within(screen.getByRole('dialog', { name: /./ }));

it('selects adaptive pacing and displays the recorded scheduler decision', async () => {
  const scheduling = { policy: 'ADAPTIVE', target_read_p95_us: 12000, maximum_delay_us: 7000 };
  const job = { job_id: 'rebuild41', array_id: 'array41', failed_disk_id: 'disk41', spare_disk_id: 'disk90',
    status: 'PREPARED', completed_blocks: 0, total_blocks: 256, progress_percent: 0, scheduling };
  const decision = { elapsed_ms: 90, pressure: 'HIGH', reason: 'sustained_foreground_pressure', delay_us: 1000,
    outstanding: 1, p95_ms: 13.2, completed_blocks: 16 };
  const calls = setup('/rebuilds', {
    'GET /api/raids': { arrays: [raid(41, 'DEGRADED')] },
    'GET /api/raids/array41/members': { members: [member(41, 'FAILED')] },
    'GET /api/spares': { spares: [{ disk_id: 'disk90', capacity_bytes: 1048576, available: true }] },
    'POST /api/rebuilds': () => json(job, 201),
    'GET /api/rebuilds/rebuild41': job,
    'GET /api/rebuilds/rebuild41/scheduler': { configuration: scheduling, current: decision, decisions: [decision] },
  });
  await online();
  tap('Prepare rebuild');
  await dialog().findByRole('option', { name: 'array41' });
  input('Degraded RAID array', 'array41', dialog());
  await dialog().findByRole('option', { name: 'disk41' });
  input('Failed member', 'disk41', dialog());
  input('Replacement spare', 'disk90', dialog());
  input('Scheduling policy', 'ADAPTIVE', dialog());
  input('Read p95 target (ms)', '12', dialog());
  input('Maximum inter-batch delay (ms)', '7', dialog());
  tap('Prepare rebuild', dialog());
  expect(await screen.findByRole('table', { name: 'Recent scheduler decisions' })).toBeVisible();
  expect(screen.getByText('sustained foreground pressure')).toBeVisible();
  expect(states(calls, 'POST /api/rebuilds')[0].payload.scheduling).toEqual(scheduling);
});

describe('layout and connection', () => {
  it('navigates routes, marks the active link and handles the compact drawer', async () => {
    setup('/');
    await online();
    const nav = within(screen.getByRole('navigation', { name: 'Main navigation' }));
    expect(nav.getByRole('link', { name: 'Overview' })).toHaveAttribute('aria-current', 'page');
    fireEvent.click(nav.getByRole('link', { name: 'Virtual Disks' }));
    expect(await screen.findByRole('heading', { name: 'Virtual disks' })).toBeVisible();
    expect(nav.getByRole('link', { name: 'Virtual Disks' })).toHaveAttribute('aria-current', 'page');
    tap('Open navigation');
    const drawer = within(screen.getByRole('dialog', { name: 'Navigation menu' }));
    fireEvent.click(drawer.getByRole('link', { name: 'File Explorer' }));
    expect(await screen.findByRole('heading', { name: 'File explorer' })).toBeVisible();
    expect(screen.queryByRole('dialog', { name: 'Navigation menu' })).toBeNull();
  });

  it.each([
    ['offline', { 'GET /health': () => { throw new TypeError('disconnected'); } }, 'Backend unreachable'],
    ['engine', { 'GET /api/engine/status': () => json({ ready: false, engine_available: false, catalog_accessible: false }, 503) }, 'Engine unavailable'],
    ['catalog', { 'GET /api/engine/status': () => json({ ready: false, engine_available: true, catalog_accessible: false }, 503) }, 'Catalog unavailable'],
  ])('reports %s and disables writes', async (_kind, overrides, label) => {
    const calls = setup('/disks', overrides);
    expect(await screen.findByText(label)).toBeVisible();
    expect(screen.getByRole('button', { name: 'Create disk' })).toBeDisabled();
    expect(states(calls, 'GET /api/disks')).toHaveLength(0);
  });
});

describe('overview', () => {
  it('uses actual health, binary capacity and usage, and isolates an independent failure', async () => {
    setup('/', {
      'GET /api/raids': { arrays: [raid(41), raid(42, 'DEGRADED'), raid(43, 'FAILED'), raid(44, 'INCONSISTENT')] },
      'GET /api/pools': { pools: [pool] },
      'GET /api/rebuilds': () => error('ENGINE_UNAVAILABLE', 503),
    });
    expect(await screen.findByRole('img', { name: '1 Healthy, 1 Degraded, 1 Failed, 1 Inconsistent' })).toBeVisible();
    expect(await screen.findByText('50%')).toBeVisible();
    expect(screen.getByText('4 MiB logically provisioned')).toBeVisible();
    expect(screen.getAllByText('512 KiB', { selector: '.capacity strong' })).toHaveLength(2);
    expect(await screen.findByText('The backend is reachable, but the storage engine is unavailable.')).toBeVisible();
    expect(within(screen.getByText('Active rebuilds').closest('.metric')).getByText('Unavailable')).toBeVisible();
  });

  it('does not treat a missing capacity field as zero', async () => {
    setup('/', { 'GET /api/pools': { pools: [{ pool_id: 'pool41' }] } });
    expect(await screen.findByText('Capacity breakdown unavailable')).toBeVisible();
    expect(within(screen.getByText('Pool capacity').closest('.metric')).getByText('Unavailable')).toBeVisible();
    expect(await screen.findByText(/No RAID arrays yet/)).toBeVisible();
    expect(bytes(1073741824)).toBe('1 GiB');
    expect(total([{ value: 1 }, {}], 'value')).toBeNull();
    expect(rawCapacity(raid(41), [disk(41)])).toBeNull();
    expect(usedPercent(200, 40, 10)).toBe(25);
  });
});

describe('disk and RAID safety', () => {
  it('validates disk input, suppresses duplicate writes and renders a safe error', async () => {
    let complete;
    const calls = setup('/disks', { 'POST /api/disks': () => new Promise((resolve) => { complete = resolve; }) });
    await online();
    tap('Create disk');
    const form = dialog();
    input('Disk identifier', 'invalid', form);
    input('Capacity (MiB)', '4', form);
    tap('Create disk', form);
    expect(await form.findByText(/Enter a disk identifier/)).toBeVisible();
    expect(states(calls, 'POST /api/disks')).toHaveLength(0);
    input('Disk identifier', 'disk50', form);
    const button = form.getByRole('button', { name: 'Create disk' });
    act(() => { fireEvent.click(button); fireEvent.click(button); });
    expect(states(calls, 'POST /api/disks')).toEqual([{ key: 'POST /api/disks', payload: { disk_id: 'disk50', capacity_mib: 4 } }]);
    await act(async () => complete(error('DISK_EXISTS', 409)));
    expect(await form.findByText(/resource already exists/)).toBeVisible();
    expect(screen.queryByText(/private\/internal/)).toBeNull();
  });

  it('validates the RAID member minimum before requesting creation', async () => {
    const calls = setup('/raids');
    await online();
    tap('Create array');
    const form = dialog();
    input('Array identifier', 'array50', form);
    input('RAID level', '6', form);
    fireEvent.click(await form.findByRole('checkbox', { name: /disk41/ }));
    tap('Create array', form);
    expect(await form.findByText('RAID 6 requires at least 4 disks.')).toBeVisible();
    expect(states(calls, 'POST /api/raids')).toHaveLength(0);
  });

  it('shows member inspection and real parity roles, and waits for keyboard confirmation', async () => {
    const calls = setup('/raids/array41', {
      'GET /api/raids/array41': raid(41),
      'GET /api/raids/array41/status': { health: 'DEGRADED', tolerated_failures: 2, pending_write: false },
      'GET /api/raids/array41/members': { members: [member(41, 'FAILED'), member(42), member(43), member(44)] },
      'GET /api/raids/array41/map/9': { array_id: 'array41', lba: 9, locations: [
        { disk_id: 'disk41', role: 'q', physical_lba: 4 }, { disk_id: 'disk42', role: 'data', physical_lba: 5 },
        { disk_id: 'disk44', role: 'p', physical_lba: 4 },
      ] },
      'POST /api/raids/array41/fail': () => json({ health: 'DEGRADED' }),
    });
    const members = await screen.findByRole('table', { name: 'Array member inspection' });
    expect(within(members).getByText('FAILED')).toBeVisible();
    await screen.findByText('DEGRADED');
    input('Logical block address (LBA)', '9');
    tap('Map block');
    expect(await screen.findByText('Logical block 9')).toBeVisible();
    const stripe = screen.getByText('Logical block 9').closest('.mapping');
    expect(within(stripe).getAllByText('P parity')).toHaveLength(2);
    expect(within(stripe).getAllByText('Q parity')).toHaveLength(2);
    expect(within(stripe.querySelector('.failed-cell')).getByText('disk41')).toBeVisible();
    expect(within(stripe).getByText('Physical LBA 5')).toBeVisible();
    input('Member to fail', 'disk42');
    tap('Simulate failure');
    expect(dialog().getByText(/persistent logical failure state/)).toBeVisible();
    expect(states(calls, 'POST /api/raids/array41/fail')).toHaveLength(0);
    const form = screen.getByRole('dialog', { name: 'Confirm persistent member failure' });
    fireEvent(form, new Event('cancel', { cancelable: true }));
    expect(form).not.toBeInTheDocument();
    tap('Simulate failure');
    const confirm = dialog().getByRole('button', { name: 'Confirm failure' });
    confirm.focus();
    expect(confirm).toHaveFocus();
    fireEvent.click(confirm);
    await waitFor(() => expect(states(calls, 'POST /api/raids/array41/fail')).toHaveLength(1));
  });

  it('locks uncertain writes until an explicit refresh and inspection', async () => {
    const calls = setup('/disks', { 'POST /api/disks': () => error('OPERATION_OUTCOME_UNKNOWN', 504) });
    await online();
    tap('Create disk');
    const form = dialog();
    input('Disk identifier', 'disk50', form);
    input('Capacity (MiB)', '4', form);
    tap('Create disk', form);
    expect(await form.findByText(/may have committed/)).toBeVisible();
    expect(form.getByRole('button', { name: 'Create disk' })).toBeDisabled();
    tap('Cancel', form);
    expect(screen.getByRole('button', { name: 'I inspected the resource' })).toBeDisabled();
    tap('Refresh state');
    await waitFor(() => expect(screen.getByRole('button', { name: 'I inspected the resource' })).toBeEnabled());
    tap('I inspected the resource');
    expect(screen.getByRole('button', { name: 'Create disk' })).toBeEnabled();
    expect(states(calls, 'POST /api/disks')).toHaveLength(1);
  });
});

describe('rebuild monitor', () => {
  const job = { job_id: 'rebuild41', array_id: 'array41', failed_disk_id: 'disk41', spare_disk_id: 'disk42', status: 'RUNNING',
    worker_active: true, worker_error: false, pause_requested: false, started_at: 1795260000, checkpoint_at: 1795260010,
    progress_percent: 25, completed_blocks: 16, total_blocks: 64, batch_blocks: 16 };

  it('renders actual block progress, requests pause and resumes the durable job', async () => {
    let current = job;
    const calls = setup('/rebuilds/rebuild41', {
      'GET /api/rebuilds': () => json({ jobs: [current] }),
      'GET /api/rebuilds/rebuild41': () => json(current),
      'POST /api/rebuilds/rebuild41/pause': () => {
        current = { ...job, status: 'PAUSED', worker_active: false };
        return json(current, 202);
      },
      'POST /api/rebuilds/rebuild41/resume': () => {
        current = { ...job, status: 'COMPLETED', worker_active: false, progress_percent: 100, completed_blocks: 64 };
        return json(current, 202);
      },
    });
    expect(await screen.findByRole('progressbar', { name: 'Rebuild progress for rebuild41' })).toHaveAttribute('value', '25');
    expect(screen.getByText('48')).toBeVisible();
    tap('Pause rebuild');
    expect(await screen.findByText('PAUSED')).toBeVisible();
    tap('Resume rebuild');
    input('Pause after checkpoint batches (optional)', '3', dialog());
    tap('Resume rebuild', dialog());
    expect(await screen.findByText('100%')).toBeVisible();
    expect(states(calls, 'POST /api/rebuilds/rebuild41/resume')).toEqual([
      { key: 'POST /api/rebuilds/rebuild41/resume', payload: { checkpoint_limit: 3 } },
    ]);
  });

  it('polls active jobs and stops after the completed response', async () => {
    let reads = 0;
    const calls = setup('/rebuilds', {
      'GET /api/rebuilds': () => json({ jobs: [++reads === 1 ? job :
        { ...job, status: 'COMPLETED', worker_active: false, progress_percent: 100, completed_blocks: 64 }] }),
    });
    expect(await screen.findByText('25%')).toBeVisible();
    expect(await screen.findByText('100%', {}, { timeout: 3000 })).toBeVisible();
    await act(async () => new Promise((resolve) => setTimeout(resolve, 1750)));
    expect(states(calls, 'GET /api/rebuilds')).toHaveLength(2);
  }, 6500);
});

describe('thin provisioning', () => {
  it('renders capacity units and capacity state for every pool', async () => {
    setup('/pools', { 'GET /api/pools': { pools: [pool, { ...pool, pool_id: 'pool42', capacity_state: 'CRITICAL' }] } });
    expect(await screen.findByText('WARNING')).toBeVisible();
    expect(screen.getByText('CRITICAL')).toBeVisible();
    expect(screen.getAllByText('2 MiB')).toHaveLength(2);
    expect(screen.getAllByText('50%')).toHaveLength(2);
  });

  it('creates an overprovisioned volume and maps an unallocated block', async () => {
    const calls = setup('/luns', {
      'GET /api/pools': { pools: [pool] },
      'GET /api/pools/pool41': pool,
      'POST /api/luns': () => json(lun, 201),
      'GET /api/luns/lun41': lun,
      'GET /api/luns/lun41/map/100': { lun_id: 'lun41', lba: 100, logical_chunk: 6, offset: 4, physical_chunk: null, raid_lba: null },
    });
    await online();
    tap('Create volume');
    const form = dialog();
    input('Volume identifier', 'lun41', form);
    await form.findByRole('option', { name: 'pool41' });
    input('Parent storage pool', 'pool41', form);
    input('Provisioned capacity (MiB)', '8', form);
    tap('Create volume', form);
    expect(await screen.findByRole('heading', { name: 'lun41' })).toBeVisible();
    expect(states(calls, 'POST /api/luns')[0].payload).toEqual({ lun_id: 'lun41', pool_id: 'pool41', capacity_mib: 8, mode: 'file' });
    await screen.findByText('128 KiB');
    input('Logical block address (LBA)', '100');
    tap('Map block');
    expect(await screen.findByText('This logical block is unallocated.')).toBeVisible();
    expect(screen.getAllByText('Unallocated')).toHaveLength(2);
  });
});

describe('file inventory and binary exchange', () => {
  it('renders records and sends a multipart body with separate processing status', async () => {
    setup('/files', { 'GET /api/files': { files: [record] } });
    let transfer;
    class MockUpload {
      upload = {};
      open(method, url) { this.method = method; this.url = url; }
      send(body) { this.body = body; transfer = this; }
    }
    vi.stubGlobal('XMLHttpRequest', MockUpload);
    expect(await screen.findByText('binary.dat')).toBeVisible();
    expect(screen.queryByRole('button', { name: /delete/i })).toBeNull();
    tap('Upload file');
    const form = dialog();
    await form.findByRole('option', { name: 'lun41' });
    input('File-managed volume', 'lun41', form);
    const file = new File([new Uint8Array([0, 255, 7])], 'new.bin');
    fireEvent.change(form.getByLabelText('File'), { target: { files: [file] } });
    expect(form.getByText('new.bin')).toBeVisible();
    tap('Upload file', form);
    expect(transfer.method).toBe('POST');
    expect(transfer.url).toBe('/api/luns/lun41/files');
    expect(transfer.body.get('file')).toBe(file);
    act(() => transfer.upload.onprogress({ lengthComputable: true, loaded: 2, total: 4 }));
    expect(form.getByRole('progressbar', { name: 'HTTP upload transfer progress' })).toHaveAttribute('value', '50');
    act(() => transfer.upload.onload());
    expect(form.getByText('Processing and verifying…')).toBeVisible();
    await act(async () => { transfer.status = 413; transfer.response = { error: { code: 'REQUEST_TOO_LARGE' } }; transfer.onload(); });
    expect(form.getByText('The file exceeds the server upload limit.')).toBeVisible();
    expect(form.getByRole('button', { name: 'Upload file' })).toBeEnabled();
  });

  it('preserves binary content and filename, and displays both SHA-256 outcomes', async () => {
    let verification = 0;
    const blob = vi.fn(() => 'blob:download');
    const click = vi.spyOn(HTMLAnchorElement.prototype, 'click').mockImplementation(function triggerDownload() {
      expect(this.download).toBe('test binary.dat');
    });
    vi.stubGlobal('URL', Object.assign(class extends URL {}, { createObjectURL: blob, revokeObjectURL: vi.fn() }));
    setup('/files/file41', {
      'GET /api/files': { files: [record] },
      'GET /api/files/file41': record,
      'GET /api/files/file41/verify': () => json({ passed: ++verification === 1, expected_sha256: record.sha256,
        actual_sha256: verification === 1 ? record.sha256 : 'b'.repeat(64) }),
      'GET /api/files/file41/download': () => new Response(new Uint8Array([0, 255, 7]), {
        headers: { 'Content-Type': 'application/octet-stream', 'Content-Disposition': "attachment; filename*=UTF-8''test%20binary.dat", 'X-Content-SHA256': record.sha256 },
      }),
    });
    expect(await screen.findByRole('heading', { name: 'binary.dat' })).toBeVisible();
    tap('Verify SHA-256');
    expect(await screen.findByText('PASSED')).toBeVisible();
    tap('Verify SHA-256');
    expect(await screen.findByText('FAILED')).toBeVisible();
    tap('Download');
    await waitFor(() => expect(click).toHaveBeenCalledTimes(1));
    expect(blob.mock.calls[0][0].size).toBe(3);
    expect(new Uint8Array(await blob.mock.calls[0][0].arrayBuffer())).toEqual(new Uint8Array([0, 255, 7]));
    expect(await screen.findByText(/Verified download sent to the browser/)).toBeVisible();
    fireEvent.click(within(screen.getByRole('main')).getByRole('link', { name: 'File Explorer' }));
    expect(await within(await screen.findByRole('table', { name: 'File inventory' })).findByText('FAILED')).toBeVisible();
    click.mockRestore();
  });
});

describe('storage telemetry', () => {
  const latency = { count: 2, sample_count: 2, window: 'ENTIRE_OBSERVATION',
    min_ms: 0.4, max_ms: 1.4, mean_ms: 0.9, p50_ms: 0.4, p95_ms: 1.4, p99_ms: 1.4 };
  const category = (name, blocks = 2) => ({ category: name, attempted: 2, completed: 2, failed: 0, blocks, bytes: blocks * 4096,
    elapsed_ns: 1800000, latency });
  const snapshot = { source: 'MEASURED', scope: 'engine_process', observation: { generation: 1, seconds: 2 },
    latency_sample_limit_per_category: 10000, categories: [
      category('RAID_LOGICAL_READ'), category('RAID_LOGICAL_WRITE'), category('VIRTUAL_DISK_READ'),
      category('VIRTUAL_DISK_WRITE', 4), category('REBUILD_READ', 12), category('REBUILD_WRITE', 8),
    ], logical_rates: { completed_requests: 4, bytes: 16384, iops: 2, mib_per_second: 0.0078125 } };
  const empty = { ...snapshot, categories: [],
    logical_rates: { completed_requests: 0, bytes: 0, iops: null, mib_per_second: null } };
  const parameters = { sequential_read_ms: 0.08, sequential_write_ms: 0.11, random_read_ms: 0.16, random_write_ms: 0.23,
    read_mib_per_second: 520, write_mib_per_second: 460, queue_depth: 1 };
  const profiles = { source: 'MODELED', profiles: ['HDD_7200', 'SATA_SSD', 'NVME_SSD'].map((profile) => ({ profile, parameters })),
    limits: { max_devices: 64, max_requests: 64 } };

  it('navigates to measured data and shows actual counts, units and window', async () => {
    setup('/system', { 'GET /api/telemetry': snapshot, 'GET /api/model/profiles': profiles });
    const link = screen.getByRole('link', { name: 'Telemetry' });
    fireEvent.click(link);
    expect(await screen.findByRole('heading', { name: 'Storage telemetry' })).toBeVisible();
    expect(link).toHaveAttribute('aria-current', 'page');
    expect(screen.getByText('MEASURED')).toBeVisible();
    expect(await screen.findByText('2 IOPS')).toBeVisible();
    expect(screen.getByText('0.007813 MiB/s')).toBeVisible();
    expect(within(screen.getByText('Physical blocks written').closest('.metric')).getByText('12')).toBeVisible();
    const table = screen.getByRole('table', { name: 'Measured latency summaries' });
    expect(within(table).getByRole('columnheader', { name: 'p99 (ms)' })).toBeVisible();
    expect(within(table).getAllByText('Entire observation').length).toBeGreaterThan(0);
    expect(screen.queryByText(/historical trend/i)).toBeNull();
  });

  it('shows empty observations and runs the selected hypothetical profile', async () => {
    const calls = setup('/telemetry', { 'GET /api/telemetry': empty, 'GET /api/model/profiles': profiles,
      'POST /api/model/simulate': () => json({ source: 'MODELED', profile: 'NVME_SSD', total_modeled_elapsed_ms: 0.9,
        requests: [{ index: 0, device: 0, operation: 'READ', bytes: 4096, effective_arrival_ms: 0,
          start_ms: 0, service_ms: 0.9, queue_wait_ms: 0, completion_ms: 0.9 }] }),
    });
    expect(await screen.findByText('No measurements collected yet.')).toBeVisible();
    expect(screen.queryByRole('table', { name: 'Measured latency summaries' })).toBeNull();
    await screen.findByRole('option', { name: /NVMe SSD/ });
    input('Device profile', 'NVME_SSD');
    expect(screen.getByText('520 / 460 MiB/s')).toBeVisible();
    tap('Run model scenario');
    expect(await screen.findByText('0.9 ms')).toBeVisible();
    expect(screen.getByText('MODELED')).toBeVisible();
    expect(screen.getByRole('columnheader', { name: 'Queue wait (ms)' })).toBeVisible();
    expect(states(calls, 'POST /api/model/simulate')[0].payload.profile).toBe('NVME_SSD');
  });

  it('keeps model failures separate from the mutation outcome guard', async () => {
    setup('/telemetry', { 'GET /api/telemetry': snapshot, 'GET /api/model/profiles': profiles,
      'POST /api/model/simulate': () => { throw new TypeError('disconnected'); } });
    await screen.findByRole('option', { name: /NVMe SSD/ });
    tap('Run model scenario');
    expect(await screen.findByText(/Could not reach the backend/)).toBeVisible();
    expect(screen.queryByText(/may have committed/)).toBeNull();
    expect(screen.getByRole('button', { name: 'Reset measurements' })).toBeEnabled();
  });

  it('requires explicit reset confirmation and refreshes the measurement window', async () => {
    let current = snapshot;
    const route = 'POST /api/telemetry/reset';
    const calls = setup('/telemetry', { 'GET /api/model/profiles': profiles, 'GET /api/telemetry': () => json(current),
      [route]: () => { current = { ...empty, observation: { generation: 2, seconds: 0 } }; return json(current); },
    });
    await screen.findByText('2 IOPS');
    tap('Reset measurements');
    expect(dialog().getByText(/durable rebuild progress are preserved/)).toBeVisible();
    expect(states(calls, route)).toHaveLength(0);
    tap('Confirm reset', dialog());
    expect(await screen.findByText('No measurements collected yet.')).toBeVisible();
    expect(states(calls, route)).toHaveLength(1);
  });

  it('handles an unavailable engine without inventing measurements or offering reset', async () => {
    const calls = setup('/telemetry', {
      'GET /api/engine/status': () => json({ ready: false, engine_available: false, catalog_accessible: false }, 503),
    });
    expect(await screen.findByText('Engine unavailable')).toBeVisible();
    expect(screen.getByRole('button', { name: 'Reset measurements' })).toBeDisabled();
    expect(states(calls, 'GET /api/telemetry')).toHaveLength(0);
    expect(screen.queryByRole('table', { name: 'Measured latency summaries' })).toBeNull();
  });
});
