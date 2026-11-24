import { createContext, useContext, useEffect, useRef, useState } from 'react';
import { QueryClient, useQuery, useQueryClient } from '@tanstack/react-query';
import { ApiError, request, UNKNOWN } from './client';

export function makeClient() {
  return new QueryClient({ defaultOptions: { queries: {
    staleTime: 10000, retry: false, refetchOnWindowFocus: false, networkMode: 'always',
  } } });
}

const Operations = createContext(null);
export const activeJob = (job) => !['COMPLETED', 'FAILED'].includes(job.status) && (job.worker_active || job.status === 'RUNNING');
export const apiPath = (...parts) => '/api/' + parts.map(encodeURIComponent).join('/');
const statusKey = ['connection'];
const pendingKey = 'raid-console-uncertain-operation';

function readUncertain() {
  try { return JSON.parse(sessionStorage.getItem(pendingKey)); }
  catch { return null; }
}

function saveUncertain(value) {
  try {
    if (value) sessionStorage.setItem(pendingKey, JSON.stringify(value));
    else sessionStorage.removeItem(pendingKey);
  } catch { /* React state still guards this session. */ }
}

async function checkConnection(signal) {
  try {
    const health = await request('/health', { signal, timeout: 4500 });
    if (health.status !== 'ok' || health.service !== 'raid-simulator-backend') throw new ApiError('INVALID_RESPONSE');
  } catch (error) {
    if (signal.aborted) throw error;
    return { kind: 'offline' };
  }
  try {
    const status = await request('/api/engine/status', { signal, timeout: 9000, health: true });
    if (typeof status.ready !== 'boolean' || typeof status.engine_available !== 'boolean' ||
      typeof status.catalog_accessible !== 'boolean') throw new ApiError('INVALID_RESPONSE');
    const kind = status.ready ? 'ready' : status.engine_available ? 'catalog' : 'engine';
    return { kind, status };
  } catch (error) {
    if (signal.aborted) throw error;
    return { kind: 'engine' };
  }
}

export function OperationsProvider({ children }) {
  const client = useQueryClient();
  const lock = useRef(false);
  const previousConnection = useRef(null);
  const [busy, setBusy] = useState('');
  const [feedback, setFeedback] = useState(null);
  const [uncertain, setUncertain] = useState(readUncertain);
  const [refreshed, setRefreshed] = useState(false);
  const [refreshing, setRefreshing] = useState(false);
  const connectionQuery = useQuery({
    queryKey: statusKey, queryFn: ({ signal }) => checkConnection(signal), refetchInterval: busy ? false : 12000,
  });
  const connection = connectionQuery.data ?? { kind: 'checking' };

  useEffect(() => {
    const last = previousConnection.current;
    const processId = connection.status?.process_id;
    previousConnection.current = { kind: connection.kind, processId };
    if (connection.kind === 'ready' && last && last.kind !== 'checking' &&
      (last.kind !== 'ready' || last.processId !== processId)) {
      client.invalidateQueries({ queryKey: ['storage'] });
    }
  }, [client, connection.kind, connection.status?.process_id]);

  async function refresh() {
    if (refreshing) return;
    setRefreshing(true);
    try {
      const result = await connectionQuery.refetch({ cancelRefetch: false });
      if (result.data?.kind === 'ready') {
        await client.refetchQueries({ queryKey: ['storage'], type: 'active' }, { cancelRefetch: false, throwOnError: true });
        setRefreshed(true);
      }
    } catch {
      // Resource-specific errors are rendered by their query observers.
    } finally {
      setRefreshing(false);
    }
  }

  async function run({ label, task, success, roots = [], uncertainTarget }) {
    if (lock.current || uncertain || connection.kind !== 'ready') return null;
    lock.current = true;
    setBusy(label);
    setFeedback(null);
    try {
      const result = await task();
      if (success) setFeedback({ tone: 'success', text: success(result) });
      await Promise.all(roots.map((root) => client.invalidateQueries({ queryKey: ['storage', root] })));
      if (roots.includes('raids')) client.invalidateQueries({ queryKey: statusKey });
      return result;
    } catch (error) {
      if (error.code === UNKNOWN) {
        const pending = { label, target: uncertainTarget };
        saveUncertain(pending);
        setUncertain(pending);
        setRefreshed(false);
      } else if (error.name !== 'AbortError') {
        const safe = error instanceof ApiError ? error : new ApiError('INVALID_RESPONSE');
        setFeedback({ tone: 'danger', text: safe.message, reference: safe.requestId, code: safe.code });
      }
      return null;
    } finally {
      lock.current = false;
      setBusy('');
    }
  }

  const value = {
    busy, feedback, setFeedback, uncertain, refreshed, refreshing, refresh,
    acknowledge: () => {
      if (refreshed) { saveUncertain(null); setUncertain(null); }
    },
    connection, run, locked: Boolean(busy || uncertain || connection.kind !== 'ready'),
  };
  return <Operations.Provider value={value}>{children}</Operations.Provider>;
}

export const useOperations = () => useContext(Operations);

export function useResource(path, { enabled = true, field, poll = false, interval = false, system = false } = {}) {
  const { connection, busy } = useOperations();
  const parts = Array.isArray(path) ? path : [path];
  const available = system ? ['ready', 'engine', 'catalog'].includes(connection.kind) : connection.kind === 'ready';
  return useQuery({
    queryKey: ['storage', ...parts],
    enabled: enabled && available,
    queryFn: async ({ signal }) => {
      const result = await request(apiPath(...parts), { signal });
      if (!field) return result;
      if (!Array.isArray(result[field])) throw new ApiError('INVALID_RESPONSE');
      return result[field];
    },
    refetchInterval: !available || busy ? false : !poll ? interval : (query) => {
      const hasWorker = query.state.data?.some(activeJob);
      return hasWorker ? (query.state.error ? 4000 : 1600) : false;
    },
  });
}

export function post(parts, body) {
  return request(apiPath(...parts), { method: 'POST', body, timeout: 65000 });
}

export async function verifyStorage(parts) {
  try {
    return await post([...parts, 'verify']);
  } catch (error) {
    if (error.code === 'VERIFICATION_FAILED') return { passed: false };
    throw error;
  }
}

export function useRebuildJobs() {
  const jobs = useResource('rebuilds', { field: 'jobs', poll: true });
  const client = useQueryClient();
  const lastActive = useRef(new Set());
  useEffect(() => {
    if (!jobs.data) return;
    if (jobs.data.some((job) => lastActive.current.has(job.job_id) && job.status === 'COMPLETED')) {
      client.invalidateQueries({ queryKey: statusKey });
      for (const root of ['raids', 'pools', 'spares', 'disks']) {
        client.invalidateQueries({ queryKey: ['storage', root] });
      }
    }
    lastActive.current = new Set(jobs.data.filter(activeJob).map((job) => job.job_id));
  }, [client, jobs.data]);
  return jobs;
}
