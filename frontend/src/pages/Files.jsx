import { Download, ShieldCheck, Upload } from 'lucide-react';
import { skipToken, useQuery, useQueryClient } from '@tanstack/react-query';
import { useEffect, useRef, useState } from 'react';
import { useParams } from 'react-router-dom';
import { downloadFile, request, uploadFile } from '../api/client';
import { apiPath, useOperations, useResource } from '../api/state';
import { Badge, Button, Field, Fields, Notice, OperationForm, Page, Panel, QueryState, RowLink, Table, TextLink } from '../components/UI';
import { bytes, date, number } from '../storage';

const verificationKey = ['file-verifications'];
function useVerificationResults() {
  return useQuery({ queryKey: verificationKey, queryFn: skipToken, gcTime: Infinity }).data;
}

function UploadDialog({ close }) {
  const luns = useResource('luns', { field: 'luns' });
  const eligible = luns.isError ? undefined : luns.data?.filter((lun) => lun.mode === 'file');
  const [file, setFile] = useState(null);
  const [lun, setLun] = useState('');
  const [progress, setProgress] = useState(null);
  const { run, busy } = useOperations();
  return <OperationForm title="Upload file" description="Import binary content into a file-managed logical volume."
    onClose={close} submit="Upload file" disabled={!eligible?.length || !file || !lun}
    onSubmit={async () => {
      if (!file) return 'Choose a file.';
      if (!eligible?.some((volume) => volume.lun_id === lun)) return 'Choose a file-managed logical volume.';
      setProgress(null);
      const result = await run({
        label: 'Importing file', task: () => uploadFile(lun, file, setProgress),
        success: (record) => record.name + ' imported as ' + record.file_id + ' · ' + bytes(record.size_bytes) + '.',
        roots: ['files', 'luns', 'pools'], uncertainTarget: file.name + ' in ' + lun,
      });
      if (result) close();
    }}>
    <Field label="File-managed volume"><select value={lun} onChange={(event) => setLun(event.target.value)} disabled={Boolean(busy)}>
      <option value="">Choose a volume</option>{eligible?.map((volume) => <option key={volume.lun_id}>{volume.lun_id}</option>)}
    </select></Field>
    <Field label="File"><input type="file" onChange={(event) => setFile(event.target.files[0] || null)} disabled={Boolean(busy)} /></Field>
    {file && <Fields items={[[ 'Filename', file.name ], [ 'Size', bytes(file.size) ], [ 'Target volume', lun || 'Choose a volume' ]]} />}
    {busy === 'Importing file' && <div className="upload-progress" role="status">
      {progress === 'processing' ? 'Processing and verifying…' : progress == null ? 'Uploading…' : 'Transfer · ' + progress + '%'}
      {typeof progress === 'number' && <progress max="100" value={progress} aria-label="HTTP upload transfer progress" />}
    </div>}
    {eligible?.length === 0 && <Notice>Create a file-managed logical volume before uploading.</Notice>}
    {luns.isError && <Notice tone="warning">File-managed volumes could not be loaded. Refresh the dashboard.</Notice>}
  </OperationForm>;
}

export function Files() {
  const files = useResource('files', { field: 'files' });
  const results = useVerificationResults();
  const [open, setOpen] = useState(false);
  const [filter, setFilter] = useState('');
  const { locked, setFeedback } = useOperations();
  const volumes = [...new Set(files.data?.map((file) => file.lun_id))].sort();
  return <Page title="File explorer" description="Import, inspect, verify, and download files stored on logical volumes."
    action={<Button variant="primary" icon={Upload} disabled={locked} onClick={() => { setFeedback(null); setOpen(true); }}>Upload file</Button>}>
    <Panel title="File inventory" action={<Field label="Filter by volume"><select value={filter} onChange={(event) => setFilter(event.target.value)}>
      <option value="">All volumes</option>{volumes.map((id) => <option key={id}>{id}</option>)}
    </select></Field>}>
      <QueryState query={files} empty="No files have been imported. Upload a file to a file-managed logical volume.">{(items) => {
        const rows = items.filter((file) => !filter || file.lun_id === filter).map((file) => ({ ...file, id: file.file_id }));
        return rows.length ? <Table label="File inventory" rows={rows} columns={[
          { key: 'id', title: 'File ID', render: (row) => <RowLink to={'/files/' + row.id}>{row.id}</RowLink> },
          { key: 'name', title: 'Filename' }, { key: 'size', title: 'Size', render: (row) => bytes(row.size_bytes) },
          { key: 'lun_id', title: 'Volume', render: (row) => <TextLink to={'/luns/' + row.lun_id}>{row.lun_id}</TextLink> },
          { key: 'status', title: 'Import status', render: (row) => <Badge value={row.status} /> },
          { key: 'verification', title: 'SHA-256', render: (row) => {
            const result = results?.[row.id];
            return result ? <Badge value={result.passed === true ? 'PASSED' : result.passed === false ? 'FAILED' : undefined} /> : 'Unchecked';
          } },
          { key: 'created_at', title: 'Imported', render: (row) => date(row.created_at) },
        ]} /> : <div className="empty">No files match this volume.</div>;
      }}</QueryState>
    </Panel>
    {open && <UploadDialog close={() => setOpen(false)} />}
  </Page>;
}

export function FileDetails() {
  const { id } = useParams();
  return <FileInfo id={id} key={id} />;
}
function FileInfo({ id }) {
  const file = useResource(['files', id]);
  const result = useVerificationResults()?.[id];
  const client = useQueryClient();
  const controller = useRef(null);
  const { run, busy, locked } = useOperations();
  useEffect(() => () => controller.current?.abort(), []);
  const setResult = (value) => client.setQueryData(verificationKey, (current) => ({ ...current, [id]: value }));
  async function verify() {
    if (locked) return;
    const requestController = new AbortController();
    controller.current = requestController;
    setResult(null);
    const verified = await run({ label: 'Verifying file', uncertainTarget: id + ' in File Explorer', task: async () => {
      try {
        return await request(apiPath('files', id, 'verify'), { signal: requestController.signal, timeout: 90000 });
      } catch (error) {
        if (error.code === 'CHECKSUM_MISMATCH') return { passed: false };
        throw error;
      }
    } });
    if (!requestController.signal.aborted) setResult(verified || { unavailable: true });
  }
  async function download() {
    if (locked || !file.data) return;
    const requestController = new AbortController();
    controller.current = requestController;
    await run({ label: 'Preparing download', task: () => downloadFile(file.data, requestController.signal),
      success: (checksum) => checksum ? 'Verified download sent to the browser. SHA-256 ' + checksum : 'Download sent to the browser.',
    });
  }
  const disabled = locked || file.isError || file.data?.status !== 'COMPLETED';
  return <Page title={file.data?.name || id} description="File metadata, extent allocation, and SHA-256 integrity." back={['/files', 'File Explorer']}
    action={<div className="actions">
      <Button icon={ShieldCheck} disabled={disabled} onClick={verify}>{busy === 'Verifying file' ? 'Verifying…' : 'Verify SHA-256'}</Button>
      <Button icon={Download} variant="primary" disabled={disabled} onClick={download}>{busy === 'Preparing download' ? 'Preparing download…' : 'Download'}</Button>
    </div>}>
    <Panel title="File details"><QueryState query={file}>{(data) => <Fields items={[
      ['File ID', data.file_id], ['Original filename', data.name], ['Size', bytes(data.size_bytes)],
      ['Logical volume', <TextLink to={'/luns/' + data.lun_id}>{data.lun_id}</TextLink>],
      ['Import state', <Badge value={data.status} />], ['Imported', date(data.created_at)],
      ['Start LBA', number(data.start_lba)], ['Extent blocks', number(data.block_count)],
      ['Stored SHA-256', data.sha256 ? <code className="digest">{data.sha256}</code> : 'Unavailable'],
    ]} />}</QueryState></Panel>
    <Panel title="Integrity verification">
      {!result ? <div className="panel-body muted">{busy === 'Verifying file' ? 'Checking the stored bytes…' : 'Run Verify SHA-256 to check the stored bytes.'}</div> :
        result.unavailable || typeof result.passed !== 'boolean' ? <Notice tone="warning">Verification result unavailable.</Notice> :
          <Fields items={[
            ['Result', <Badge value={result.passed === true ? 'PASSED' : 'FAILED'} />],
            ['Expected SHA-256', result.expected_sha256 ? <code className="digest">{result.expected_sha256}</code> : 'Unavailable'],
            ['Actual SHA-256', result.actual_sha256 ? <code className="digest">{result.actual_sha256}</code> : 'Unavailable'],
          ]} />}
    </Panel>
  </Page>;
}
