import { cloneElement, useEffect, useId, useRef, useState } from 'react';
import { AlertCircle, ArrowUpRight, ChevronRight, Database, LoaderCircle, X } from 'lucide-react';
import { Link } from 'react-router-dom';
import { useOperations } from '../api/state';
import { bytes, integer, number, percent, tone, usedPercent } from '../storage';

export function Page({ title, description, action, children, back }) {
  return <>
    {back && <Link className="back-link" to={back[0]}>{back[1]}</Link>}
    <div className="page-heading">
      <div><h1>{title}</h1><p>{description}</p></div>
      {action}
    </div>
    {children}
  </>;
}

export function Panel({ title, description, action, children, className = '' }) {
  return <section className={'panel ' + className}>
    {(title || action) && <div className="panel-heading">
      <div>{title && <h2>{title}</h2>}{description && <p>{description}</p>}</div>{action}
    </div>}
    {children}
  </section>;
}

export function Button({ icon: Icon, variant = 'secondary', children, className = '', ...props }) {
  return <button type="button" className={'button ' + variant + ' ' + className} {...props}>
    {Icon && <Icon size={16} aria-hidden="true" />}{children}
  </button>;
}

export function TextLink({ to, children, arrow = false }) {
  return <Link className="text-link" to={to}>{children}{arrow && <ArrowUpRight size={15} aria-hidden="true" />}</Link>;
}

export function RowLink({ to, children }) {
  return <Link className="row-link" to={to}>{children}<ChevronRight size={14} aria-hidden="true" /></Link>;
}

export function Badge({ value, label, color }) {
  return <span className={'badge ' + (color || tone(value))}><span aria-hidden="true" />{label || (value ? value.replaceAll('_', ' ') : 'Unavailable')}</span>;
}

export function Notice({ tone: color = 'neutral', children, role = 'status' }) {
  return <div className={'notice ' + color} role={role}><AlertCircle size={17} aria-hidden="true" /><div>{children}</div></div>;
}

export function QueryState({ query, empty = 'No resources yet.', children }) {
  const { connection } = useOperations();
  if (query.data === undefined) {
    if (query.isError) return <Notice tone="danger">{query.error.message}</Notice>;
    if (connection.kind !== 'ready' && connection.kind !== 'checking' && !query.isFetching) return <Notice>Data is unavailable while this service is disconnected.</Notice>;
    return <div className="loading" role="status"><LoaderCircle size={17} aria-hidden="true" /> Loading…</div>;
  }
  return <>
    {query.isError && <Notice tone="warning">Showing the last available data. {query.error.message}</Notice>}
    {Array.isArray(query.data) && query.data.length === 0 ?
      <div className="empty"><Database size={22} aria-hidden="true" /><p>{empty}</p></div> : children(query.data)}
  </>;
}

export function Table({ columns, rows, label }) {
  return <div className="table-wrap" tabIndex={0} role="region" aria-label={label}>
    <table aria-label={label}>
      <thead><tr>{columns.map(({ key, title }) => <th key={key} scope="col">{title}</th>)}</tr></thead>
      <tbody>{rows.map((row) => <tr key={row.id}>
        {columns.map(({ key, render }, index) => {
          const value = render ? render(row) : row[key];
          return index === 0 ? <th key={key} scope="row">{value ?? '—'}</th> : <td key={key}>{value ?? '—'}</td>;
        })}
      </tr>)}</tbody>
    </table>
  </div>;
}

export function Metric({ title, value, subtitle, icon: Icon, accent = 'cyan' }) {
  return <div className="metric">
    <div className={'metric-icon ' + accent}><Icon size={20} aria-hidden="true" /></div>
    <div><div className="metric-label">{title}</div><div className={'metric-value' + (value == null ? ' no-value' : '')}>{value ?? 'Unavailable'}</div>
      <div className="metric-note">{subtitle}</div>
    </div>
  </div>;
}

export function Fields({ items }) {
  return <dl className="fields">{items.map(([label, value]) => <div key={label}><dt>{label}</dt><dd>{value ?? 'Unavailable'}</dd></div>)}</dl>;
}

export function Field({ label, hint, children }) {
  const id = useId();
  return <div className="field">
    <label htmlFor={id}>{label}</label>
    {cloneElement(children, { id, 'aria-describedby': hint ? id + '-hint' : undefined })}
    {hint && <small id={id + '-hint'}>{hint}</small>}
  </div>;
}

export function Dialog({ title, description, onClose, children, pending = false }) {
  const ref = useRef(null);
  const id = useId();
  useEffect(() => {
    const dialog = ref.current;
    const prior = document.activeElement;
    if (typeof dialog.showModal === 'function') dialog.showModal();
    else dialog.setAttribute('open', '');
    return () => {
      if (typeof dialog.close === 'function') dialog.close();
      else dialog.removeAttribute('open');
      if (prior?.isConnected) prior.focus();
    };
  }, []);
  return <dialog ref={ref} className="dialog" aria-labelledby={id} aria-describedby={description ? id + '-body' : undefined}
    onCancel={(event) => { event.preventDefault(); if (!pending) onClose(); }}>
    <header><div><h2 id={id}>{title}</h2>{description && <p id={id + '-body'}>{description}</p>}</div>
      <Button className="icon-button" aria-label="Close dialog" icon={X} disabled={pending} onClick={onClose} />
    </header>
    <div className="dialog-content">{children}</div>
  </dialog>;
}

export function OperationForm({ title, description, onClose, submit, onSubmit, children, danger = false, disabled = false }) {
  const { busy, uncertain, feedback, locked } = useOperations();
  const [validation, setValidation] = useState('');
  return <Dialog title={title} description={description} onClose={onClose} pending={Boolean(busy)}>
    <form className="form" noValidate onSubmit={async (event) => {
      event.preventDefault();
      if (locked || disabled) return;
      setValidation('');
      const data = Object.fromEntries(new FormData(event.currentTarget));
      const error = await onSubmit(data);
      if (typeof error === 'string') setValidation(error);
    }}>
      {children}
      {validation && <Notice tone="danger" role="alert">{validation}</Notice>}
      {feedback?.tone === 'danger' && <Notice tone="danger" role="alert">{feedback.text}</Notice>}
      {uncertain && <Notice tone="warning">The operation may have committed. Close this dialog, refresh, and inspect the affected resource.</Notice>}
      <div className="dialog-actions">
        <Button onClick={onClose} disabled={Boolean(busy)}>Cancel</Button>
        <Button type="submit" variant={danger ? 'destructive' : 'primary'} disabled={locked || disabled} icon={busy ? LoaderCircle : undefined}>
          {busy || submit}
        </Button>
      </div>
    </form>
  </Dialog>;
}

export function LbaForm({ capacity, onMap, fetching }) {
  const [error, setError] = useState('');
  const { connection } = useOperations();
  return <form className="inline-form" noValidate onSubmit={(event) => {
    event.preventDefault();
    const value = new FormData(event.currentTarget).get('lba');
    if (!integer(value, 0, capacity - 1)) {
      setError('Enter a whole LBA from 0 through ' + number(capacity - 1) + '.');
      return;
    }
    setError('');
    onMap(Number(value));
  }}>
    <Field label="Logical block address (LBA)">
      <input name="lba" type="number" min="0" max={capacity - 1} step="1" defaultValue="0" />
    </Field>
    <Button type="submit" variant="primary" disabled={connection.kind !== 'ready' || !capacity}>{fetching ? 'Mapping…' : 'Map block'}</Button>
    {error && <Notice tone="danger" role="alert">{error}</Notice>}
  </form>;
}

export function Capacity({ capacity, allocated, reserved, free, donut = false, summary = false }) {
  const used = usedPercent(capacity, allocated, reserved);
  if (used == null || !Number.isFinite(free)) return <div className="no-data">Capacity breakdown unavailable</div>;
  const allocatedPercent = capacity ? Math.min(100, 100 * allocated / capacity) : 0;
  const reservedPercent = Math.min(100 - allocatedPercent, capacity ? 100 * reserved / capacity : 0);
  return <div className={'capacity ' + (donut ? 'capacity-donut' : '') + (summary ? ' compact' : '')}>
    {donut ? <div className="donut" style={{ '--allocated': allocatedPercent + '%', '--used': (allocatedPercent + reservedPercent) + '%' }}>
      <div><strong>{percent(used)}</strong><span>Used</span></div>
    </div> : <><div className="capacity-title"><span>Utilization</span><strong>{percent(used)}</strong></div>
      <div className="meter" role="img" aria-label={percent(used) + ' allocated or reserved'}>
        <span className="allocated" style={{ width: allocatedPercent + '%' }} /><span className="reserved" style={{ width: reservedPercent + '%' }} />
      </div></>}
    {!summary && <div className="capacity-breakdown">
      {[['Allocated', allocated, 'allocated'], ['Reserved', reserved, 'reserved'], ['Free', free, 'free']].map(([label, value, key]) =>
        <div key={key}><span><i className={key} />{label}</span><strong>{bytes(value)}</strong></div>)}
    </div>}
  </div>;
}

export function Progress({ job }) {
  const value = job.progress_percent;
  return <div className="progress">
    <div className="progress-label"><strong>{percent(value, 2)}</strong><span>{number(job.completed_blocks)} / {number(job.total_blocks)} blocks</span></div>
    <progress max="100" value={Number.isFinite(value) ? value : undefined} aria-label={'Rebuild progress for ' + job.job_id} />
  </div>;
}
