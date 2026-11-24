import assert from 'node:assert/strict';
import { spawn } from 'node:child_process';
import { createHash } from 'node:crypto';
import { createWriteStream } from 'node:fs';
import { mkdir, mkdtemp, readFile, writeFile } from 'node:fs/promises';
import { resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { setTimeout as delay } from 'node:timers/promises';
import { chromium } from 'playwright-core';

const frontend = fileURLToPath(new URL('../', import.meta.url));
const project = resolve(frontend, '..');
const build = resolve(project, 'build');
const frontendUrl = 'http://127.0.0.1:5173';
const port = '18011';
const backendUrl = 'http://127.0.0.1:' + port;
const python = process.platform === 'win32' ? '.venv/Scripts/python.exe' : '.venv/bin/python';
const hash = (value) => createHash('sha256').update(value).digest('hex');
const services = [];
const backendRunner = [
  'import sys, threading, uvicorn',
  'from backend.app.config import Settings',
  'settings = Settings.from_environment()',
  'server = uvicorn.Server(uvicorn.Config("backend.app.main:app", host="127.0.0.1", port=settings.port))',
  'def stop():',
  '    sys.stdin.readline()',
  '    server.should_exit = True',
  'threading.Thread(target=stop, daemon=True).start()',
  'server.run()',
].join('\n');

async function reachable(address) {
  try { return (await fetch(address, { signal: AbortSignal.timeout(1000) })).ok; }
  catch { return false; }
}
async function waitForServer(address, child) {
  for (let attempt = 0; attempt < 80 && child.exitCode === null; attempt++) {
    if (await reachable(address)) return;
    await delay(250);
  }
  throw new Error('Service did not become ready at ' + address);
}
function launch(executable, args, cwd, env, logs, artifactDir, graceful = false) {
  const child = spawn(executable, args, { cwd, env: { ...process.env, ...env }, windowsHide: true, stdio: [graceful ? 'pipe' : 'ignore', 'pipe', 'pipe'] });
  const log = createWriteStream(resolve(artifactDir, logs));
  child.stdout.pipe(log, { end: false });
  child.stderr.pipe(log, { end: false });
  child.on('error', (error) => log.write(String(error)));
  child.on('close', () => log.end());
  const exited = new Promise((resolve) => child.once('close', resolve));
  services.push({ child, graceful, exited });
  return child;
}

const artifactParent = process.env.RAID_BROWSER_ARTIFACT_PARENT || build;
await mkdir(artifactParent, { recursive: true });
const artifacts = await mkdtemp(resolve(artifactParent, 'browser-demo-'));
const storage = resolve(artifacts, 'data');
await mkdir(storage);
console.log('Demo artifacts at ' + artifacts);
assert.equal(await reachable(frontendUrl), false, 'Stop an existing Vite instance to run a fresh browser session.');
assert.equal(await reachable(backendUrl + '/health'), false, 'The isolated test needs a free backend port.');

let browser;
let page;
try {
  const server = launch(resolve(project, python), ['-c', backendRunner], project,
    { APP_PORT: port, RAID_STORAGE_ROOT: storage, PYTHONUNBUFFERED: '1' }, 'backend.log', artifacts, true);
  await waitForServer(backendUrl + '/api/engine/status', server);
  const vite = launch(process.execPath, ['node_modules/vite/bin/vite.js'], frontend,
    { VITE_PROXY_TARGET: backendUrl, VITE_API_BASE_URL: '' }, 'vite.log', artifacts);
  await waitForServer(frontendUrl, vite);
  browser = await chromium.launch(process.env.RAID_BROWSER_EXECUTABLE ? { executablePath: process.env.RAID_BROWSER_EXECUTABLE } : { channel: 'msedge' });
  page = await browser.newPage({ viewport: { width: 1440, height: 1000 }, acceptDownloads: true, locale: 'en-US' });
  page.setDefaultTimeout(12000);
  const browserErrors = [];
  let checkingDisconnect = false;
  page.on('pageerror', (error) => browserErrors.push(String(error)));
  page.on('console', (message) => {
    if (message.type() !== 'error') return;
    if (checkingDisconnect && message.text().includes('ERR_INTERNET_DISCONNECTED')) return;
    browserErrors.push(message.text());
  });
  page.on('response', (response) => {
    if (response.url().startsWith(frontendUrl + '/api/') && response.status() >= 400) {
      browserErrors.push(response.status() + ' ' + new URL(response.url()).pathname);
    }
  });
  const main = page.getByRole('main');
  const button = (name, root = main) => root.getByRole('button', { name, exact: true });
  const modal = (name) => page.getByRole('dialog', { name, exact: true });
  const visible = (locator, timeout) => locator.waitFor({ state: 'visible', timeout });
  const nav = (name) => page.getByRole('navigation', { name: 'Main navigation' }).getByRole('link', { name, exact: true });
  async function api(method, route, data) {
    const response = await page.request.fetch(frontendUrl + '/api/' + route, { method, data, timeout: 15000 });
    assert.ok(response.ok(), method + ' ' + route + ' returned ' + response.status() + ' ' + await response.text());
    return response.json();
  }
  const snapshot = () => api('GET', 'telemetry?include_samples=true&event_limit=32');
  const category = (data, key) => data.categories.find((row) => row.category === key);
  async function fit(width, filename) {
    await page.setViewportSize({ width, height: width === 390 ? 844 : 950 });
    await page.evaluate(() => window.scrollTo(0, 0));
    const size = await page.evaluate(() => [document.documentElement.scrollWidth, window.innerWidth]);
    assert.ok(size[0] <= size[1], 'Horizontal page overflow at ' + width + ': ' + size);
    await page.screenshot({ path: resolve(artifacts, filename), fullPage: true });
  }

  await page.goto(frontendUrl);
  await visible(page.getByText('Engine ready', { exact: true }));
  await visible(main.getByText('No RAID arrays yet. Create an array to see its health.'));
  await nav('Telemetry').click();
  await visible(main.getByText('No measurements collected yet.', { exact: true }));
  const empty = await snapshot();
  assert.equal(empty.source, 'MEASURED');
  assert.ok(empty.categories.every((row) => row.attempted === 0 && row.latency.p99_ms === null));
  await nav('Virtual Disks').click();
  await visible(main.getByText('No virtual disks yet. Create a disk to begin.'));
  const diskIds = Array.from({ length: 13 }, (_, index) => 'disk' + (11001 + index));
  for (const [index, id] of diskIds.entries()) {
    await button('Create disk').click();
    const form = modal('Create virtual disk');
    await form.getByLabel('Disk identifier').fill(id);
    await form.getByLabel('Capacity (MiB)').fill(index >= 4 && index < 7 ? '24' : '4');
    await button('Create disk', form).click();
    await visible(main.getByRole('link', { name: id, exact: true }));
  }

  async function makeRaid(id, level, members) {
    await nav('RAID Arrays').click();
    await button('Create array').click();
    const form = modal('Create RAID array');
    await form.getByLabel('Array identifier').fill(id);
    await form.getByLabel('RAID level').selectOption(level);
    for (const disk of members) await form.getByRole('checkbox', { name: new RegExp(disk) }).check();
    await button('Create array', form).click();
    await visible(main.getByRole('heading', { name: id, exact: true }));
    await visible(main.getByText('HEALTHY', { exact: true }));
  }

  await makeRaid('array11001', '6', diskIds.slice(0, 4));
  await main.getByLabel('Logical block address (LBA)').fill('17');
  await button('Map block').click();
  await visible(main.getByText('Logical block 17', { exact: true }));
  await visible(main.locator('.stripe-role').getByText('P parity', { exact: true }));
  await visible(main.locator('.stripe-role').getByText('Q parity', { exact: true }));
  await button('Verify array').click();
  await visible(main.getByText('PASSED', { exact: true }));
  await fit(1440, 'raid-mapping.png');

  await nav('Storage Pools').click();
  await button('Create pool').click();
  const poolForm = modal('Create storage pool');
  await poolForm.getByLabel('Pool identifier').fill('pool11001');
  await poolForm.getByLabel('Backing RAID array').selectOption('array11001');
  await poolForm.getByLabel('Chunk size (blocks)').fill('32');
  await button('Create pool', poolForm).click();
  await visible(main.getByText('8 MiB', { exact: true }).first());
  await button('Verify pool').click();
  await visible(main.getByText('PASSED', { exact: true }));

  await nav('Logical Volumes').click();
  await button('Create volume').click();
  const lunForm = modal('Create logical volume');
  await lunForm.getByLabel('Volume identifier').fill('lun11001');
  await lunForm.getByLabel('Parent storage pool').selectOption('pool11001');
  await lunForm.getByLabel('Provisioned capacity (MiB)').fill('16');
  await button('Create volume', lunForm).click();
  await visible(main.getByText('16 MiB', { exact: true }));
  await main.getByLabel('Logical block address (LBA)').fill('17');
  await button('Map block').click();
  await visible(main.getByText('This logical block is unallocated.'));

  await nav('File Explorer').click();
  await button('Upload file').click();
  const upload = modal('Upload file');
  const filename = 'raid binary sample.bin';
  const payload = Buffer.from(Array.from({ length: 73729 }, (_, index) => (index * 71 + 239) % 256));
  const source = resolve(artifacts, filename);
  await writeFile(source, payload);
  await upload.getByLabel('File-managed volume').selectOption('lun11001');
  await upload.getByLabel('File', { exact: true }).setInputFiles(source);
  await button('Upload file', upload).click();
  await visible(main.getByRole('link', { name: 'file1', exact: true }));
  await main.getByRole('link', { name: 'file1', exact: true }).click();
  await visible(main.getByRole('heading', { name: filename, exact: true }));
  await button('Verify SHA-256').click();
  await visible(main.getByText('PASSED', { exact: true }));
  const downloadPromise = page.waitForEvent('download');
  await button('Download').click();
  const download = await downloadPromise;
  assert.equal(download.suggestedFilename(), filename);
  const copy = resolve(artifacts, 'downloaded.bin');
  await download.saveAs(copy);
  const checksum = hash(payload);
  assert.equal(hash(await readFile(copy)), checksum);
  assert.deepEqual(await main.locator('.digest').allTextContents(), [checksum, checksum, checksum]);
  await fit(1440, 'file-integrity.png');

  await makeRaid('array11002', '1', diskIds.slice(4, 6));
  await makeRaid('array11003', '0', diskIds.slice(7, 9));
  await makeRaid('array11004', '5', diskIds.slice(9, 13));
  const actual = [];
  const payloadBlock = 'a6'.repeat(4096);
  for (const item of [
    { level: 0, suffix: 11003, chunk: 32, fullWrites: 2, singleReads: 0, singleWrites: 1 },
    { level: 1, suffix: 11002, chunk: 1, fullWrites: 2, singleReads: 0, singleWrites: 2 },
    { level: 5, suffix: 11004, chunk: 48, fullWrites: 4, singleReads: 2, singleWrites: 2 },
    { level: 6, suffix: 11001, chunk: 32, fullWrites: 4, singleReads: 3, singleWrites: 3 },
  ]) {
    const { level, suffix, chunk, fullWrites, singleReads, singleWrites } = item;
    const pool = 'pool' + suffix;
    const lun = 'lun' + (suffix + 100);
    if (level !== 6) await api('POST', 'pools', { pool_id: pool, array_id: 'array' + suffix, chunk_blocks: chunk });
    await api('POST', 'luns', { lun_id: lun, pool_id: pool, capacity_mib: 1, mode: 'raw' });
    const route = 'luns/' + lun + '/blocks/0';
    await api('POST', 'telemetry/reset');
    await api('PUT', route, { data_hex: payloadBlock });
    const first = await snapshot();
    assert.equal(category(first, 'RAID_LOGICAL_WRITE').blocks, chunk);
    assert.equal(category(first, 'VIRTUAL_DISK_READ').completed, 0);
    assert.equal(category(first, 'VIRTUAL_DISK_WRITE').completed, fullWrites);
    const newWindow = await api('POST', 'telemetry/reset');
    assert.equal(newWindow.observation.generation, first.observation.generation + 1);
    assert.equal((await api('GET', route)).data_hex, payloadBlock);
    const read = await snapshot();
    assert.equal(category(read, 'RAID_LOGICAL_READ').bytes, 4096);
    await api('POST', 'telemetry/reset');
    await api('PUT', route, { data_hex: payloadBlock });
    const single = await snapshot();
    assert.equal(category(single, 'RAID_LOGICAL_WRITE').blocks, 1);
    assert.equal(category(single, 'VIRTUAL_DISK_READ').completed, singleReads);
    assert.equal(category(single, 'VIRTUAL_DISK_WRITE').completed, singleWrites);
    assert.equal(category(single, 'RAID_LOGICAL_WRITE').latency.sample_count, 1);
    actual.push({ level, first, read, single });
  }
  await nav('Hot Spares').click();
  await button('Register spare').click();
  const register = modal('Register hot spare');
  await register.getByLabel('Virtual disk').selectOption(diskIds[6]);
  await button('Register spare', register).click();
  await visible(main.getByRole('link', { name: diskIds[6], exact: true }));

  await nav('RAID Arrays').click();
  await main.getByRole('link', { name: 'array11002', exact: true }).click();
  await main.getByLabel('Member to fail').selectOption(diskIds[4]);
  await button('Simulate failure').click();
  const failure = modal('Confirm persistent member failure');
  await visible(failure.getByText(/persistent logical failure state/));
  await failure.press('Escape');
  await visible(button('Simulate failure'));
  await button('Simulate failure').click();
  await button('Confirm failure', failure).click();
  await visible(main.locator('.badge').getByText('DEGRADED', { exact: true }));
  await visible(main.getByRole('table', { name: 'Array member inspection' }).getByText('FAILED', { exact: true }));

  await nav('Rebuild Monitor').click();
  await button('Prepare rebuild').click();
  const prepare = modal('Prepare rebuild');
  await prepare.getByLabel('Degraded RAID array').selectOption('array11002');
  await prepare.getByLabel('Failed member').selectOption(diskIds[4]);
  await prepare.getByLabel('Replacement spare').selectOption(diskIds[6]);
  await prepare.getByLabel('Scheduling policy').selectOption('ADAPTIVE');
  await button('Prepare rebuild', prepare).click();
  await visible(main.getByRole('heading', { name: 'rebuild1', exact: true }));
  await api('POST', 'telemetry/reset');
  await button('Start rebuild').click();
  const start = modal('Start rebuild');
  await button('Start rebuild', start).click();
  await button('Pause rebuild').click();
  await visible(main.getByText('PAUSED', { exact: true }), 25000);
  const checkpoint = Number(await main.getByRole('progressbar').getAttribute('value'));
  assert.ok(checkpoint > 0 && checkpoint < 100, 'Expected a durable nonterminal checkpoint');
  await visible(main.getByRole('table', { name: 'Recent scheduler decisions' }));
  const schedule = await api('GET', 'rebuilds/rebuild1/scheduler');
  assert.equal(schedule.configuration.policy, 'ADAPTIVE');
  assert.equal(schedule.source, 'MEASURED');
  assert.ok(schedule.decisions.length > 0);
  await fit(1024, 'rebuild-paused.png');
  await button('Resume rebuild').click();
  const resume = modal('Resume rebuild');
  await button('Resume rebuild', resume).click();
  await visible(main.getByText('COMPLETED', { exact: true }), 90000);
  assert.equal(await main.getByRole('progressbar').getAttribute('value'), '100');
  const finishedBlocks = (await main.getByRole('progressbar').locator('..').innerText()).match(/([\d,]+) \/ ([\d,]+) blocks/);
  assert.ok(finishedBlocks && finishedBlocks[1] === finishedBlocks[2], 'All physical blocks completed');
  await fit(1440, 'rebuild-completed.png');

  const rebuild = await api('GET', 'telemetry/rebuilds/rebuild1');
  assert.equal(rebuild.source, 'MEASURED');
  assert.equal(rebuild.job.completed_blocks, 6144);
  assert.equal(category(rebuild, 'REBUILD_BATCH').blocks, 6144);
  assert.equal(category(rebuild, 'REBUILD_WRITE').origin_counts.REBUILD.bytes, 6144 * 4096);
  assert.equal((await api('GET', 'luns/lun11102/blocks/0')).data_hex, payloadBlock);
  const measured = await snapshot();
  assert.ok(measured.logical_rates.iops > 0 && measured.logical_rates.mib_per_second > 0);
  await nav('Telemetry').click();
  await visible(main.getByText('MEASURED', { exact: true }));
  await visible(main.getByRole('table', { name: 'Measured latency summaries' }));
  await main.getByLabel('Rebuild job').selectOption('rebuild1');
  await visible(main.getByRole('table', { name: 'Measured rebuild operations' }));
  await visible(main.getByText('COMPLETED', { exact: true }));
  const workload = [
    { device: 0, arrival_ms: 0, operation: 'READ', bytes: 4096, access: 'RANDOM' },
    { device: 0, arrival_ms: 0.01, operation: 'WRITE', bytes: 4096, access: 'RANDOM' },
    { device: 1, arrival_ms: 0, operation: 'READ', bytes: 4096, access: 'RANDOM' },
    { device: 1, arrival_ms: 0, operation: 'WRITE', bytes: 8192, access: 'SEQUENTIAL', depends_on: [0, 2] },
  ];
  await main.getByLabel('Scenario requests (JSON)').fill(JSON.stringify(workload, null, 2));
  const modeled = [];
  for (const profile of ['HDD_7200', 'SATA_SSD', 'NVME_SSD']) {
    await main.getByLabel('Device profile').selectOption(profile);
    const receiving = page.waitForResponse((response) =>
      response.url().endsWith('/api/model/simulate') && response.request().method() === 'POST');
    await button('Run model scenario').click();
    const response = await receiving;
    assert.equal(response.status(), 200);
    const prediction = await response.json();
    await visible(main.getByRole('table', { name: 'Modeled request results' }));
    assert.equal(prediction.source, 'MODELED');
    assert.deepEqual(prediction, await api('POST', 'model/simulate', { profile, devices: 2, workload }));
    assert.equal(prediction.requests[0].start_ms, prediction.requests[2].start_ms);
    assert.ok(prediction.requests[1].queue_wait_ms > 0);
    assert.equal(prediction.requests[3].effective_arrival_ms, prediction.requests[2].completion_ms);
    modeled.push(prediction);
  }
  await fit(1440, 'telemetry-1440.png');
  await fit(390, 'telemetry-390.png');
  await page.setViewportSize({ width: 1440, height: 950 });
  await button('Reset measurements').click();
  const resetDialog = modal('Reset measurements');
  await visible(resetDialog.getByText(/durable rebuild progress are preserved/));
  await button('Confirm reset', resetDialog).click();
  await visible(main.getByText('No measurements collected yet.', { exact: true }));
  const cleared = await api('GET', 'telemetry/rebuilds/rebuild1');
  assert.equal(cleared.retained, false);
  assert.equal(cleared.job.completed_blocks, 6144);
  await writeFile(resolve(artifacts, 'telemetry.json'), JSON.stringify({ empty, actual, measured, rebuild, modeled, cleared }, null, 2) + '\n');

  await button('Dismiss notification').click();
  await nav('Overview').click();
  await visible(main.getByRole('img', { name: '4 Healthy, 0 Degraded, 0 Failed, 0 Inconsistent' }));
  await visible(main.getByText('COMPLETED', { exact: true }));
  await fit(1440, 'overview-1440.png');
  await fit(1024, 'overview-1024.png');
  await fit(390, 'overview-390.png');
  await page.getByRole('button', { name: 'Open navigation' }).click();
  await visible(modal('Navigation menu'));
  await page.screenshot({ path: resolve(artifacts, 'navigation-390.png') });
  await nav('RAID Arrays').click();
  await main.getByRole('link', { name: 'array11002', exact: true }).click();
  await visible(main.getByRole('heading', { name: 'array11002', exact: true }));
  await visible(main.getByText('HEALTHY', { exact: true }));
  await fit(390, 'raid-detail-390.png');
  await page.getByRole('button', { name: 'Open navigation' }).click();
  await nav('Virtual Disks').click();
  await button('Create disk').click();
  const mobileForm = modal('Create virtual disk');
  const box = await mobileForm.boundingBox();
  assert.ok(box.x >= 0 && box.x + box.width <= 390 && box.y >= 0 && box.y + box.height <= 844, 'Mobile dialog fits the viewport');
  await page.screenshot({ path: resolve(artifacts, 'disk-dialog-390.png') });
  await mobileForm.press('Escape');
  await page.getByRole('button', { name: 'Open navigation' }).click();
  await nav('System').click();
  await visible(main.getByRole('heading', { name: 'Runtime status' }));
  await visible(main.getByText('CONNECTED', { exact: true }));
  checkingDisconnect = true;
  await page.route('**/health', (route) => route.abort('internetdisconnected'));
  await page.reload();
  await visible(main.getByText(/The backend is unreachable/));
  await fit(390, 'disconnected-390.png');
  await page.unroute('**/health');
  await page.getByRole('button', { name: 'Refresh dashboard' }).click();
  await visible(page.getByText('Engine ready', { exact: true }));
  checkingDisconnect = false;
  const matches = { bytes: payload.byteLength, sha256: checksum, downloadFilename: download.suggestedFilename(), pausedProgress: checkpoint,
    finalProgress: 100, rebuiltPhysicalBlocks: Number(finishedBlocks[1].replaceAll(',', '')), schedulerPolicy: schedule.configuration.policy,
    disconnectRecovered: true, browserErrors, viewports: [1440, 1024, 390] };
  await writeFile(resolve(artifacts, 'verification.json'), JSON.stringify(matches, null, 2) + '\n');
  assert.deepEqual(browserErrors, []);
  console.log(JSON.stringify(matches, null, 2));
} catch (error) {
  if (page) {
    await page.screenshot({ path: resolve(artifacts, 'failure.png') }).catch(() => {});
    const text = await page.locator('main').innerText({ timeout: 2000 }).catch(() => '');
    await writeFile(resolve(artifacts, 'failure.txt'), text);
  }
  throw error;
} finally {
  await browser?.close();
  for (const { child, graceful, exited } of services.reverse()) {
    if (child.exitCode !== null || !child.pid) continue;
    if (graceful) {
      child.stdin.on('error', () => {});
      child.stdin.end('stop\n');
      await Promise.race([exited, delay(18000, undefined, { ref: false })]);
    }
    if (child.exitCode === null) child.kill();
  }
}
