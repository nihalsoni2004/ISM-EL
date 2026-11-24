export const unavailable = 'Unavailable';
export const healthStates = ['HEALTHY', 'DEGRADED', 'FAILED', 'INCONSISTENT'];
export const validId = (id, prefix) => new RegExp('^' + prefix + '(0|[1-9][0-9]{0,19})$').test(id);
export const integer = (value, min = 0, max = Number.MAX_SAFE_INTEGER) =>
  /^\d+$/.test(String(value)) && Number.isSafeInteger(Number(value)) && Number(value) >= min && Number(value) <= max;
export const maxLba = 4503599627370495;
export const number = (value) => Number.isFinite(value) ? new Intl.NumberFormat().format(value) : '—';
export const percent = (value, decimals = 1) => Number.isFinite(value) ? new Intl.NumberFormat(undefined, { maximumFractionDigits: decimals }).format(value) + '%' : '—';
export const bytes = (value) => {
  if (!Number.isFinite(value) || value < 0) return '—';
  const units = ['B', 'KiB', 'MiB', 'GiB', 'TiB', 'PiB', 'EiB'];
  const exponent = value ? Math.min(Math.floor(Math.log(value) / Math.log(1024)), units.length - 1) : 0;
  return new Intl.NumberFormat(undefined, { maximumFractionDigits: 2 }).format(value / 1024 ** exponent) + ' ' + units[exponent];
};
export function total(rows, field) {
  return Array.isArray(rows) && rows.every((row) => Number.isFinite(row[field]))
    ? rows.reduce((sum, row) => sum + row[field], 0) : null;
}
export function usedPercent(capacity, allocated, reserved) {
  if (![capacity, allocated, reserved].every(Number.isFinite)) return null;
  return capacity > 0 ? 100 * (allocated + reserved) / capacity : 0;
}
export function date(value) {
  if (!value) return '—';
  const timestamp = typeof value === 'number' ? value * 1000 : /[zZ]|[+-]\d\d:\d\d$/.test(value) ? value : value + 'Z';
  const d = new Date(timestamp);
  return Number.isNaN(d.valueOf()) ? '—' : d.toLocaleString();
}
export const tone = (value) => {
  if (['HEALTHY', 'ONLINE', 'NORMAL', 'COMPLETED', 'PASSED', 'AVAILABLE', 'READY', 'CONNECTED'].includes(value)) return 'success';
  if (['DEGRADED', 'WARNING', 'PAUSED', 'INTERRUPTED', 'RESERVED'].includes(value)) return 'warning';
  if (['FAILED', 'INCONSISTENT', 'CRITICAL', 'FULL', 'OFFLINE'].includes(value)) return 'danger';
  if (['RUNNING', 'WRITING', 'VERIFYING', 'PREPARED'].includes(value)) return 'accent';
  return 'neutral';
};
export function rawCapacity(array, disks) {
  if (!disks || !Array.isArray(array.members) || !array.members.length) return null;
  const byId = new Map(disks.map((disk) => [disk.disk_id, disk.capacity_bytes]));
  const capacities = array.members.map((id) => byId.get(id));
  return capacities.every(Number.isFinite) ? capacities.reduce((sum, value) => sum + value, 0) : null;
}
export function unusedDisks(disks, arrays, spares) {
  if (!disks || !arrays || !spares) return undefined;
  const assigned = new Set([...arrays.flatMap((array) => array.members), ...spares.map((spare) => spare.disk_id)]);
  return disks.filter((disk) => !assigned.has(disk.disk_id));
}
