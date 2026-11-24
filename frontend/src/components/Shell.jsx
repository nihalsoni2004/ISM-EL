import { useEffect, useRef, useState } from 'react';
import { Activity, Boxes, ChartNoAxesCombined, CircleHelp, Database, FolderOpen, HardDrive, LayoutDashboard, Layers3, Menu, RefreshCw, Server, Settings2, X } from 'lucide-react';
import { Link, NavLink, Outlet, useLocation } from 'react-router-dom';
import { useOperations } from '../api/state';
import { Badge, Button, Notice } from './UI';

const groups = [
  ['Workspace', [['/', 'Overview', LayoutDashboard]]],
  ['Storage', [['/disks', 'Virtual Disks', HardDrive], ['/raids', 'RAID Arrays', Server],
    ['/spares', 'Hot Spares', Boxes], ['/rebuilds', 'Rebuild Monitor', Activity]]],
  ['Provisioning', [['/pools', 'Storage Pools', Layers3], ['/luns', 'Logical Volumes', Database], ['/files', 'File Explorer', FolderOpen]]],
  ['Administration', [['/telemetry', 'Telemetry', ChartNoAxesCombined], ['/system', 'System', Settings2]]],
];
const connectionLabels = {
  checking: ['Checking backend', 'Checking engine', 'neutral'],
  offline: ['Backend unreachable', 'Engine unavailable', 'danger'],
  engine: ['Backend connected', 'Engine unavailable', 'warning'],
  catalog: ['Backend connected', 'Catalog unavailable', 'warning'],
  ready: ['Backend connected', 'Engine ready', 'success'],
};

export function ConnectionBadges() {
  const { connection } = useOperations();
  const [backend, engine, color] = connectionLabels[connection.kind];
  const backendColor = ['ready', 'engine', 'catalog'].includes(connection.kind) ? 'success' : color;
  return <div className="connections" role="status" aria-label="Connection status">
    <Badge label={backend} color={backendColor} /><Badge label={engine} color={color} />
  </div>;
}

function Navigation({ close }) {
  return <nav aria-label="Main navigation">{groups.map(([title, links]) =>
    <div className="nav-group" key={title}><div className="nav-heading">{title}</div>
      {links.map(([href, label, Icon]) => <NavLink key={href} to={href} end={href === '/'} onClick={close}>
        <Icon size={17} aria-hidden="true" />{label}
      </NavLink>)}
    </div>)}</nav>;
}

function Sidebar({ mobile = false, close }) {
  return <div className="sidebar-inner">
    <div className="brand">
      <Link to="/" onClick={close}><span className="brand-logo"><Layers3 aria-hidden="true" size={22} /></span>
        <span><strong>RAID Console</strong><small>Software-defined storage</small></span></Link>
      {mobile && <Button className="icon-button" icon={X} aria-label="Close navigation" onClick={close} />}
    </div>
    <Navigation close={close} />
    <div className="sidebar-footer"><CircleHelp size={16} aria-hidden="true" /><span>Local simulator</span></div>
  </div>;
}

export default function Shell() {
  const { pathname } = useLocation();
  const section = pathname.split('/')[1];
  const title = groups.flatMap(([, links]) => links).find(([path]) => path === '/' + section)?.[1] || 'Storage';
  const [mobileOpen, setMobileOpen] = useState(false);
  const mobile = useRef(null);
  const toggle = useRef(null);
  const previousPath = useRef(pathname);
  const content = useRef(null);
  const { busy, feedback, setFeedback, uncertain, refreshed, refreshing, refresh, acknowledge, connection } = useOperations();
  const close = () => setMobileOpen(false);

  useEffect(() => {
    const dialog = mobile.current;
    if (mobileOpen) {
      if (typeof dialog.showModal === 'function') dialog.showModal();
      else dialog.setAttribute('open', '');
    }
    const narrow = window.matchMedia?.('(max-width: 810px)');
    const onResize = (event) => { if (!event.matches) setMobileOpen(false); };
    if (mobileOpen) narrow?.addEventListener('change', onResize);
    return () => {
      if (!mobileOpen) return;
      narrow?.removeEventListener('change', onResize);
      if (typeof dialog.close === 'function') dialog.close();
      else dialog.removeAttribute('open');
      toggle.current?.focus();
    };
  }, [mobileOpen]);

  useEffect(() => {
    document.title = title + ' · RAID Console';
    if (previousPath.current !== pathname) {
      setMobileOpen(false);
      window.scrollTo(0, 0);
      content.current?.focus({ preventScroll: true });
    }
    previousPath.current = pathname;
  }, [pathname, title]);

  return <div className="app-shell">
    <a className="skip-link" href="#content" onClick={(event) => { event.preventDefault(); content.current?.focus(); }}>Skip to content</a>
    <aside className="sidebar"><Sidebar /></aside>
    <dialog ref={mobile} className="mobile-drawer" aria-label="Navigation menu"
      onCancel={(event) => { event.preventDefault(); close(); }}>
      <Sidebar mobile close={close} />
    </dialog>
    <div className="workspace">
      <header className="topbar">
        <div className="breadcrumb">
          <button ref={toggle} type="button" className="button secondary icon-button menu-button" onClick={() => setMobileOpen(true)}
            aria-label="Open navigation" aria-expanded={mobileOpen}><Menu size={21} aria-hidden="true" /></button>
          <span className="breadcrumb-root">Console</span><span className="breadcrumb-divider">/</span><strong>{title}</strong>
        </div>
        <div className="top-actions"><ConnectionBadges />
          <Button icon={RefreshCw} className="refresh-button" onClick={refresh} disabled={refreshing || Boolean(busy)}
            aria-label="Refresh dashboard"><span>{refreshing ? 'Refreshing…' : 'Refresh'}</span></Button>
        </div>
      </header>
      <main id="content" ref={content} tabIndex={-1}>
        {['offline', 'engine', 'catalog'].includes(connection.kind) && <Notice tone="warning">
          {connection.kind === 'offline' ? 'The backend is unreachable.' : connection.kind === 'catalog' ?
            'The storage catalog is unavailable.' : 'The backend is online. The storage engine is unavailable.'}{' '}
          Cached data may be out of date. Refresh after the service recovers.
        </Notice>}
        {uncertain && <Notice tone="warning" role="alert">
          <strong>{uncertain.label} has an unknown outcome.</strong> The operation may have committed. Refresh and inspect{' '}
          {uncertain.target || 'the affected resource'} before sending another request.
          <div className="notice-actions">
            <Button icon={RefreshCw} onClick={refresh} disabled={refreshing}>{refreshing ? 'Refreshing…' : 'Refresh state'}</Button>
            <Button onClick={acknowledge} disabled={!refreshed}>I inspected the resource</Button>
          </div>
        </Notice>}
        {feedback && <div className="feedback"><Notice tone={feedback.tone} role={feedback.tone === 'danger' ? 'alert' : 'status'}>
          {feedback.text}{feedback.reference && <small className="request-reference">Request {feedback.reference}</small>}
        </Notice><Button className="icon-button" icon={X} onClick={() => setFeedback(null)} aria-label="Dismiss notification" /></div>}
        <Outlet />
      </main>
    </div>
  </div>;
}
