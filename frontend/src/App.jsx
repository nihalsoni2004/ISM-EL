import { QueryClientProvider } from '@tanstack/react-query';
import { HashRouter, Navigate, Route, Routes } from 'react-router-dom';
import { makeClient, OperationsProvider } from './api/state';
import Shell from './components/Shell';
import Overview from './pages/Overview';
import { Disk, Disks, Spares } from './pages/Disks';
import { Raid, Raids } from './pages/Raids';
import { Rebuild, Rebuilds } from './pages/Rebuilds';
import { Lun, Luns, Pool, Pools } from './pages/Provisioning';
import { FileDetails, Files } from './pages/Files';
import System from './pages/System';
import Telemetry from './pages/Telemetry';

const client = makeClient();

export function DashboardRoutes() {
  return <Routes>
    <Route element={<Shell />}>
      <Route index element={<Overview />} />
      <Route path="disks" element={<Disks />} />
      <Route path="disks/:id" element={<Disk />} />
      <Route path="raids" element={<Raids />} />
      <Route path="raids/:id" element={<Raid />} />
      <Route path="spares" element={<Spares />} />
      <Route path="rebuilds" element={<Rebuilds />} />
      <Route path="rebuilds/:id" element={<Rebuild />} />
      <Route path="pools" element={<Pools />} />
      <Route path="pools/:id" element={<Pool />} />
      <Route path="luns" element={<Luns />} />
      <Route path="luns/:id" element={<Lun />} />
      <Route path="files" element={<Files />} />
      <Route path="files/:id" element={<FileDetails />} />
      <Route path="system" element={<System />} />
      <Route path="telemetry" element={<Telemetry />} />
      <Route path="*" element={<Navigate to="/" replace />} />
    </Route>
  </Routes>;
}

export default function App() {
  return <QueryClientProvider client={client}>
    <OperationsProvider><HashRouter><DashboardRoutes /></HashRouter></OperationsProvider>
  </QueryClientProvider>;
}
