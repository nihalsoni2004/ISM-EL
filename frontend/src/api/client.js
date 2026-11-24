const base = (import.meta.env.VITE_API_BASE_URL || '').replace(/\/+$/, '');
export const UNKNOWN = 'OPERATION_OUTCOME_UNKNOWN';

const messages = {
  [UNKNOWN]: 'The operation may have committed. Refresh and inspect the affected resource before sending another request.',
  ENGINE_UNAVAILABLE: 'The backend is reachable, but the storage engine is unavailable.',
  CATALOG_UNAVAILABLE: 'The engine could not read the storage catalog.',
  NETWORK: 'Could not reach the backend. Check the connection and refresh.',
  TIMEOUT: 'The request expired. Refresh to check the latest state.',
  CHECKSUM_MISMATCH: 'The file failed SHA-256 verification.',
  DUPLICATE_RESOURCE: 'The identifier already exists or the requested worker is already active.',
  INVALID_MEMBER_STATE: 'This disk cannot perform that action in its current state.',
  INVALID_RAID_CONFIGURATION: 'The engine rejected the RAID geometry. Check the RAID level, member sizes, and stripe unit.',
  ARRAY_DEGRADED: 'The array is degraded. This action requires a healthy array.',
  ARRAY_FAILED: 'The array has exceeded its failure tolerance.',
  ARRAY_INCONSISTENT: 'The array has a pending consistency issue. Inspect its status.',
  OUT_OF_SPACE: 'The operation requires more storage than is available.',
  STAGING_REJECTED: 'The backend could not accept this filename or stage the file.',
  INVALID_PARAMS: 'Check the supplied values against the supported identifiers and limits.',
  INVALID_ARGUMENT: 'The engine rejected the supplied values. Check the identifiers and limits.',
  INVALID_REQUEST: 'Check the form values and submit again.',
  INVALID_RESPONSE: 'The server returned an unexpected response.',
  VERIFICATION_FAILED: 'The engine reported a verification failure.',
};

export class ApiError extends Error {
  constructor(code, status = 0, requestId) {
    super(messages[code] || ({
      400: 'The engine rejected this request.',
      403: 'The backend denied this request.',
      404: 'The resource could not be found.',
      409: 'The resource already exists or its current state conflicts with this operation.',
      413: 'The file exceeds the server upload limit.',
      422: 'Check the input values and try again.',
      423: 'The resource is in use by another operation.',
      503: 'The storage engine or catalog is temporarily unavailable.',
      504: 'The request expired while waiting for the storage engine.',
      507: 'The storage pool has insufficient space.',
    }[status] || 'The backend could not complete the request.'));
    this.code = code;
    this.status = status;
    this.requestId = requestId;
  }
}

function responseError(data, status, write) {
  const error = data?.error;
  return new ApiError(error?.code || (write && status >= 500 ? UNKNOWN : 'HTTP_ERROR'), status, error?.request_id);
}

export async function request(path, { signal, method = 'GET', body, timeout = 40000, health = false, binary = false,
  mutates = method !== 'GET' } = {}) {
  const deadline = AbortSignal.timeout(timeout);
  try {
    const response = await fetch(base + path, {
      method, cache: 'no-store', signal: signal ? AbortSignal.any([signal, deadline]) : deadline,
      headers: body === undefined ? undefined : { 'Content-Type': 'application/json' },
      body: body === undefined ? undefined : JSON.stringify(body),
    });
    if (response.ok && binary) return { blob: await response.blob(), headers: response.headers };
    const data = await response.json().catch((error) => {
      if (signal?.aborted || deadline.aborted) throw error;
      return null;
    });
    if (!response.ok && !(health && response.status === 503 && typeof data?.ready === 'boolean')) {
      throw responseError(data, response.status, mutates);
    }
    if (!data || typeof data !== 'object') throw new ApiError(mutates ? UNKNOWN : 'INVALID_RESPONSE');
    return data;
  } catch (error) {
    if (error instanceof ApiError || signal?.aborted) throw error;
    throw new ApiError(mutates ? UNKNOWN : deadline.aborted ? 'TIMEOUT' : 'NETWORK');
  }
}

export function uploadFile(lun, file, onProgress) {
  return new Promise((resolve, reject) => {
    const xhr = new XMLHttpRequest();
    xhr.open('POST', base + '/api/luns/' + encodeURIComponent(lun) + '/files');
    xhr.responseType = 'json';
    xhr.timeout = 180000;
    xhr.upload.onprogress = (event) => onProgress(event.lengthComputable ? Math.round(100 * event.loaded / event.total) : null);
    xhr.upload.onload = () => onProgress('processing');
    xhr.onload = () => {
      if (xhr.status >= 200 && xhr.status < 300) {
        if (xhr.response?.file_id) resolve(xhr.response);
        else reject(new ApiError(UNKNOWN));
      } else reject(responseError(xhr.response, xhr.status, true));
    };
    xhr.onerror = xhr.ontimeout = xhr.onabort = () => reject(new ApiError(UNKNOWN));
    const body = new FormData();
    body.append('file', file);
    xhr.send(body);
  });
}

export async function downloadFile(file, signal) {
  const { blob, headers } = await request('/api/files/' + encodeURIComponent(file.file_id) + '/download', {
    binary: true, timeout: 180000, signal,
  });
  const disposition = headers.get('Content-Disposition') || '';
  const encoded = disposition.match(/filename\*=UTF-8''([^;]+)/i);
  const plain = disposition.match(/filename="?([^";]+)/i);
  let name = plain?.[1] || file.name || file.file_id;
  if (encoded) {
    try { name = decodeURIComponent(encoded[1]); } catch { /* Keep the plain filename. */ }
  }
  const url = URL.createObjectURL(blob);
  const link = document.createElement('a');
  link.href = url;
  link.download = name.replace(/[\\/]/g, '_');
  document.body.append(link);
  link.click();
  link.remove();
  setTimeout(() => URL.revokeObjectURL(url), 1000);
  return headers.get('X-Content-SHA256');
}
