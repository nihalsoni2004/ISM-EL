import { RefreshCw } from 'lucide-react';
import { useOperations, useResource } from '../api/state';
import { Badge, Button, Fields, Page, Panel, QueryState } from '../components/UI';
import { healthStates, number } from '../storage';

export default function System() {
  const { connection, refresh, refreshing, busy } = useOperations();
  const version = useResource('version', { system: true });
  const status = connection.status;
  const reachable = ['ready', 'engine', 'catalog'].includes(connection.kind);
  const boolStatus = (value) => value === true ? 'READY' : value === false ? 'OFFLINE' : undefined;
  return <Page title="System" description="Inspect API connectivity, engine readiness, and simulator capabilities."
    action={<Button icon={RefreshCw} onClick={refresh} disabled={refreshing || Boolean(busy)}>{refreshing ? 'Refreshing…' : 'Refresh status'}</Button>}>
    <div className="two-column detail-grid">
      <Panel title="Runtime status"><Fields items={[
        ['Backend', <Badge value={reachable ? 'CONNECTED' : connection.kind === 'offline' ? 'OFFLINE' : undefined} />],
        ['Engine availability', <Badge value={boolStatus(status?.engine_available)} />],
        ['Catalog accessibility', <Badge value={boolStatus(status?.catalog_accessible)} />],
        ['Engine ready', <Badge value={boolStatus(status?.ready)} />],
        ['Engine version', status?.version], ['Engine process ID', status?.process_id],
        ['Diagnostic code', status?.error_code || (status?.ready ? 'None reported' : 'Unavailable')],
      ]} /></Panel>
      <Panel title="API and capabilities">
        <QueryState query={version}>{(data) => <Fields items={[[ 'API version', data.version ]]} />}</QueryState>
        <Fields items={[
          ['IPC protocol version', 'Unavailable over REST'],
          ['Supported RAID levels', 'RAID 0, RAID 1, RAID 5, RAID 6'],
          ['Transport', 'FastAPI REST and persistent NDJSON IPC'],
          ['Access', 'Trusted localhost'],
        ]} />
      </Panel>
    </div>
    <Panel title="Catalog health counts"><Fields items={healthStates.map((state) => [
      state[0] + state.slice(1).toLowerCase(), status?.catalog_accessible && status.array_health ?
        number(status.array_health[state] ?? 0) : 'Unavailable',
    ])} /></Panel>
  </Page>;
}
