import { useCallback, useEffect, useState } from 'react'
import './App.css'

const api = (import.meta.env.VITE_API_BASE_URL || '/api').replace(/\/$/, '')
const allPhases = ['L1', 'L2', 'L3', 'LN']
const deviceKey = 'smart-motor-auto-device-id'
const legacyDeviceKey = 'nl-voltage-device-id'
const emptyPhase = () => ({ isVol: false, minVol: 0, maxVol: 0, isA: false, minA: 0, maxA: 0 })
const emptyLimits = () => Object.fromEntries(allPhases.map((phase) => [phase, emptyPhase()]))
const limitAliases = { L1: ['L1', 'l1', 'L', 'l'], L2: ['L2', 'l2'], L3: ['L3', 'l3'], LN: ['LN', 'ln', 'N', 'n'] }
const hasStatus = (value) => value !== null && value !== undefined && Number.isFinite(Number(value)) && Number(value) >= 0
const normalizeSinglePhase = (value) => typeof value?.isAutoSinglePhase === 'boolean'
  ? value.isAutoSinglePhase
  : value?.mode?.toUpperCase() === 'LN'
const activePhases = (isAutoSinglePhase) => isAutoSinglePhase ? ['LN'] : allPhases.slice(0, 3)
// The API serializes readings as VName/VValue/...; accept any key casing.
const normalizeMeasurements = (value) => (Array.isArray(value) ? value : []).map((item) => ({
  vName: item.vName ?? item.VName,
  vValue: item.vValue ?? item.VValue,
  vStatus: item.vStatus ?? item.VStatus,
  aValue: item.aValue ?? item.AValue,
  aStatus: item.aStatus ?? item.AStatus,
}))
const normalizeLimits = (value) => Object.fromEntries(allPhases.map((phase) => [
  phase,
  limitAliases[phase].map((key) => value?.[key]).find(Boolean) ?? emptyPhase(),
]))

async function request(path, options) {
  const response = await fetch(`${api}${path}`, {
    ...options,
    headers: { 'Content-Type': 'application/json', ...options?.headers },
  })
  if (!response.ok) throw new Error(await response.text() || `Request failed (${response.status})`)
  return response.status === 204 ? null : response.json()
}

function Brand() {
  return <div className="brand"><span className="brand-icon">S</span><span>SmartMotorAuto<span className="brand-sub">MOTOR CONTROL SYSTEM</span></span></div>
}

function Login({ onLogin }) {
  const [value, setValue] = useState('')
  const [error, setError] = useState('')
  function submit(event) {
    event.preventDefault()
    const id = Number(value.trim())
    if (!Number.isSafeInteger(id) || id <= 0) return setError('Enter a valid numeric device ID.')
    onLogin(String(id))
  }

  return <main className="login-page">
    <div className="login-brand"><Brand /></div>
    <section className="login-panel">
      <p className="eyebrow">DEVICE ACCESS</p>
      <h1>Good to have<br />you back.</h1>
      <p className="login-copy">Enter the device ID to connect to its live motor and phase readings.</p>
      <form onSubmit={submit}>
        <label htmlFor="device-id">Device ID</label>
        <input id="device-id" autoFocus inputMode="numeric" autoComplete="off" placeholder="e.g. 101" value={value} onChange={(event) => { setValue(event.target.value); setError('') }} />
        {error && <p className="form-error" role="alert">{error}</p>}
        <button className="primary-button" type="submit">Connect device <span aria-hidden="true">&#8599;</span></button>
      </form>
      <p className="login-footnote">Your device ID stays on this device.</p>
    </section>
    <div className="login-side-note"><span className="pulse-dot" /> MONITORING SYSTEMS / 01</div>
  </main>
}

function Dashboard({ deviceId, onSettings, onLogout }) {
  const [data, setData] = useState({ online: null, motorOn: null, reason: '', autoOn: false, measurements: [], isAutoSinglePhase: false, limits: emptyLimits(), updatedAt: null })
  const [refreshing, setRefreshing] = useState(false)
  const [switching, setSwitching] = useState(false)
  const [error, setError] = useState('')

  const refresh = useCallback(async (showIndicator = true) => {
    if (showIndicator) setRefreshing(true)
    const results = await Promise.allSettled([
      request(`/Device/online?deviceId=${deviceId}`),
      request(`/Device/motor/status?autoId=${deviceId}`),
      request(`/Device/measurements?deviceId=${deviceId}`),
      request(`/Device/auto/status?autoId=${deviceId}`),
      request(`/Device/limit?deviceId=${deviceId}`),
    ])
    setData((current) => ({
      online: results[0].status === 'fulfilled' ? results[0].value : current.online,
      motorOn: results[1].status === 'fulfilled' ? results[1].value.motorStatus : current.motorOn,
      reason: results[1].status === 'fulfilled' ? results[1].value.reason : current.reason,
      measurements: results[2].status === 'fulfilled' ? normalizeMeasurements(results[2].value) : current.measurements,
      autoOn: results[3].status === 'fulfilled' ? results[3].value === 1 : current.autoOn,
      limits: results[4].status === 'fulfilled' ? normalizeLimits(results[4].value) : current.limits,
      isAutoSinglePhase: results[4].status === 'fulfilled' ? normalizeSinglePhase(results[4].value) : current.isAutoSinglePhase,
      updatedAt: new Date(),
    }))
    setError(results.every((result) => result.status === 'rejected') ? 'Unable to reach the device API. Check the connection and try again.' : '')
    setRefreshing(false)
  }, [deviceId])

  useEffect(() => {
    const initialRefresh = window.setTimeout(() => refresh(), 0)
    const timer = window.setInterval(() => refresh(), 30_000)
    return () => {
      window.clearTimeout(initialRefresh)
      window.clearInterval(timer)
    }
  }, [refresh])

  async function toggleAuto() {
    setSwitching(true)
    setError('')
    const next = data.autoOn ? 2 : 1
    try {
      await request(`/Device/auto/status?autoId=${deviceId}&status=${next}`, { method: 'PUT' })
      setData((current) => ({ ...current, autoOn: next === 1 }))
      window.setTimeout(refresh, 800)
    } catch {
      setError('Could not update automatic control. Please try again.')
    } finally {
      setSwitching(false)
    }
  }

  const visiblePhases = activePhases(data.isAutoSinglePhase)
  const measurements = [...data.measurements].sort((left, right) => visiblePhases.indexOf(left.vName?.toUpperCase()) - visiblePhases.indexOf(right.vName?.toUpperCase()))
  const measuredPhaseCount = visiblePhases.filter((phase) => {
    const reading = measurements.find((item) => item.vName?.toUpperCase() === phase)
    return reading && (hasStatus(reading.vStatus) || hasStatus(reading.aStatus))
  }).length
  const updated = data.updatedAt ? data.updatedAt.toLocaleTimeString([], { hour: '2-digit', minute: '2-digit' }) : 'Waiting for first reading'
  const motorLabel = data.motorOn === null ? 'Awaiting status' : data.motorOn ? 'Motor running' : 'Motor stopped'
  const onlineLabel = data.online === null ? 'Checking connection' : data.online ? 'Device online' : 'Device offline'

  return <main className="app-shell">
    <header className="topbar">
      <Brand />
      <div className="topbar-actions">
        <span className={`connection-pill ${data.online === true ? 'is-online' : data.online === false ? 'is-offline' : ''}`}><span />{onlineLabel}</span>
        <button className="icon-button" type="button" onClick={onSettings} aria-label="Open limit settings" title="Limit settings">
          <svg viewBox="0 0 24 24" aria-hidden="true"><path d="M12 8.7a3.3 3.3 0 1 0 0 6.6 3.3 3.3 0 0 0 0-6.6Z" /><path d="m19.4 13.5 1.1.9-1.1 1.9-1.4-.5a7.6 7.6 0 0 1-1.5.9l-.2 1.5h-2.2l-.4-1.4a7.6 7.6 0 0 1-1.7 0l-.7 1.3-2.1-.8.2-1.5a7.6 7.6 0 0 1-1.2-1.3l-1.5.2-.8-2.1 1.3-.7a7.6 7.6 0 0 1 0-1.7l-1.3-.7.8-2.1 1.5.2a7.6 7.6 0 0 1 1.3-1.2l-.2-1.5 2.1-.8.7 1.3a7.6 7.6 0 0 1 1.7 0l.7-1.3 2.1.8-.2 1.5a7.6 7.6 0 0 1 1.2 1.3l1.5-.2.8 2.1-1.3.7a7.6 7.6 0 0 1 0 1.7Z" /></svg>
        </button>
        <button className="text-button logout-button" type="button" onClick={onLogout}>Change device</button>
      </div>
    </header>
    <div className="content-wrap">
      <section className="page-heading">
        <div><p className="eyebrow">LIVE MONITOR <span className="heading-divider">/</span> DEVICE {deviceId}</p><h1>System overview</h1></div>
        <div className="refresh-meta"><span className={`refresh-indicator ${refreshing ? 'is-refreshing' : ''}`} /><span>{refreshing ? 'Updating readings' : `Updated ${updated}`}</span><button className="text-button refresh-button" type="button" onClick={refresh} disabled={refreshing}>Refresh</button></div>
      </section>
      {error && <div className="alert-banner" role="alert">{error}</div>}
      <section className={`motor-panel ${data.motorOn === true ? 'motor-running' : data.motorOn === false ? 'motor-stopped' : 'motor-unknown'}`}>
        <div className="motor-copy">
          <p className="eyebrow">MOTOR STATUS</p>
          <div className="motor-state-line"><span className="motor-indicator" /><h2>{motorLabel}</h2></div>
          {data.motorOn === false && <p className="motor-reason">{data.reason || 'Motor is off.'}</p>}
          {data.motorOn === null && <p className="motor-reason">Status will appear when the device responds.</p>}
          <div className={`motor-online ${data.online === true ? 'online' : data.online === false ? 'offline' : ''}`}><span />{onlineLabel}</div>
        </div>
        <div className="motor-control">
          <div className="control-label"><span>Automatic control</span><strong>{data.autoOn ? 'ON' : 'OFF'}</strong></div>
          <button className={`switch ${data.autoOn ? 'switch-on' : ''}`} type="button" role="switch" aria-checked={data.autoOn} aria-label="Automatic motor control" onClick={toggleAuto} disabled={switching || data.online === false}><span /></button>
          <span className="control-hint">{switching ? 'Sending command...' : data.online === false ? 'Device offline' : `Tap to turn auto ${data.autoOn ? 'off' : 'on'}`}</span>
        </div>
      </section>
      <section className="measurements-section" aria-labelledby="measurements-title">
        <div className="section-heading"><div><p className="eyebrow">ELECTRICAL INPUT</p><h2 id="measurements-title">Measurements</h2></div><span className="reading-count">{measuredPhaseCount} / {visiblePhases.length} phases</span></div>
        <div className="measurement-table-wrap">
          <table className="measurement-table"><thead><tr><th scope="col">Phase</th><th scope="col">Voltage</th><th scope="col">Current</th><th scope="col">Phase status</th></tr></thead>
            <tbody>{visiblePhases.map((phase) => {
              const reading = measurements.find((item) => item.vName?.toUpperCase() === phase)
              const limit = data.limits[phase]
              const checks = [
                ...(limit.isVol ? [reading?.vStatus] : []),
                ...(limit.isA ? [reading?.aStatus] : []),
              ]
              const known = checks.length > 0 && checks.every(hasStatus)
              const ok = known && checks.every((value) => Number(value) === 1)
              return <tr key={phase}><th scope="row"><span className="phase-marker">{phase}</span><span className="phase-name">Phase {phase}</span></th>
                <td>{reading ? <>{Number(reading.vValue).toLocaleString(undefined, { maximumFractionDigits: 1 })}<span className="unit"> V</span></> : <span className="unavailable">-</span>}</td>
                <td>{reading ? <>{Number(reading.aValue).toLocaleString(undefined, { maximumFractionDigits: 2 })}<span className="unit"> A</span></> : <span className="unavailable">-</span>}</td>
                <td><span className={`phase-status ${known ? ok ? 'phase-ok' : 'phase-bad' : 'phase-pending'}`}><span />{checks.length === 0 ? 'Not monitored' : known ? ok ? 'OK' : 'Not OK' : 'No status'}</span></td></tr>
            })}</tbody>
          </table>
          {measuredPhaseCount === 0 && <p className="empty-readings">No measurements received yet.</p>}
        </div>
        <p className="table-footnote">Values refresh automatically every 30 seconds.</p>
      </section>
      <footer className="page-footer"><span>DEVICE ID <strong>{deviceId}</strong></span><span>SMARTMOTORAUTO <span className="footer-separator">/</span> LIVE SYSTEMS</span></footer>
    </div>
  </main>
}

function Settings({ deviceId, onBack }) {
  const [limits, setLimits] = useState(emptyLimits)
  const [isAutoSinglePhase, setIsAutoSinglePhase] = useState(false)
  const [loading, setLoading] = useState(true)
  const [saving, setSaving] = useState(false)
  const [message, setMessage] = useState('')
  const [error, setError] = useState('')

  useEffect(() => {
    let active = true
    request(`/Device/limit?deviceId=${deviceId}`)
      .then((value) => {
        if (!active) return
        setLimits(normalizeLimits(value))
        setIsAutoSinglePhase(normalizeSinglePhase(value))
      })
      .catch(() => { if (active) setMessage('No saved limits found. Set values below to configure this device.') })
      .finally(() => { if (active) setLoading(false) })
    return () => { active = false }
  }, [deviceId])

  function update(phase, key, value) {
    setLimits((current) => ({ ...current, [phase]: { ...current[phase], [key]: value } }))
  }

  async function save(event) {
    event.preventDefault()
    setSaving(true)
    setMessage('')
    setError('')
    const payload = Object.fromEntries(allPhases.map((phase) => [phase, {
      ...limits[phase], minVol: Number(limits[phase].minVol) || 0, maxVol: Number(limits[phase].maxVol) || 0,
      minA: Number(limits[phase].minA) || 0, maxA: Number(limits[phase].maxA) || 0,
    }]))
    payload.isAutoSinglePhase = isAutoSinglePhase
    try {
      await request(`/Device/limit?deviceId=${deviceId}`, { method: 'PUT', body: JSON.stringify(payload) })
      setMessage('Limits sent to the device.')
    } catch {
      setError('Could not save limits. Check the API connection and try again.')
    } finally {
      setSaving(false)
    }
  }

  return <main className="app-shell settings-shell">
    <header className="topbar"><Brand /><button className="icon-button back-button" type="button" onClick={onBack} aria-label="Back to overview" title="Back to overview"><svg viewBox="0 0 24 24" aria-hidden="true"><path d="m15 18-6-6 6-6" /></svg></button></header>
    <div className="content-wrap settings-content">
      <section className="page-heading settings-heading"><div><p className="eyebrow">DEVICE {deviceId} <span className="heading-divider">/</span> CONFIGURATION</p><h1>Phase limits</h1><p className="settings-intro">Enable Auto Single Phase to use LN. When disabled, the device uses L1, L2, and L3.</p></div></section>
      {loading ? <div className="settings-loading">Loading saved limits...</div> : <form onSubmit={save}>
        <label className="phase-mode-toggle"><input type="checkbox" checked={isAutoSinglePhase} onChange={(event) => setIsAutoSinglePhase(event.target.checked)} /><span>Auto Single Phase</span><small>{isAutoSinglePhase ? 'Using LN phase' : 'Using L1, L2, and L3 phases'}</small></label>
        <div className="limits-grid">{activePhases(isAutoSinglePhase).map((phase) => <section className="limit-row" key={phase}>
          <div className="limit-phase"><span className="phase-marker">{phase}</span><div><strong>Phase {phase}</strong><span>Voltage and current</span></div></div>
          <div className="limit-group"><label className="limit-toggle"><input type="checkbox" checked={Boolean(limits[phase]?.isVol)} onChange={(event) => update(phase, 'isVol', event.target.checked)} /><span>Voltage</span></label>
            <div className="limit-inputs"><label><span>Min V</span><input type="number" step="0.1" value={limits[phase]?.minVol ?? 0} onChange={(event) => update(phase, 'minVol', event.target.value)} disabled={!limits[phase]?.isVol} /></label><span className="range-dash">to</span><label><span>Max V</span><input type="number" step="0.1" value={limits[phase]?.maxVol ?? 0} onChange={(event) => update(phase, 'maxVol', event.target.value)} disabled={!limits[phase]?.isVol} /></label></div>
          </div>
          <div className="limit-group"><label className="limit-toggle"><input type="checkbox" checked={Boolean(limits[phase]?.isA)} onChange={(event) => update(phase, 'isA', event.target.checked)} /><span>Current</span></label>
            <div className="limit-inputs"><label><span>Min A</span><input type="number" step="0.01" value={limits[phase]?.minA ?? 0} onChange={(event) => update(phase, 'minA', event.target.value)} disabled={!limits[phase]?.isA} /></label><span className="range-dash">to</span><label><span>Max A</span><input type="number" step="0.01" value={limits[phase]?.maxA ?? 0} onChange={(event) => update(phase, 'maxA', event.target.value)} disabled={!limits[phase]?.isA} /></label></div>
          </div>
        </section>)}</div>
        {message && <p className="save-message" role="status">{message}</p>}{error && <p className="form-error" role="alert">{error}</p>}
        <div className="settings-actions"><button className="text-button" type="button" onClick={onBack}>Cancel</button><button className="primary-button save-button" type="submit" disabled={saving}>{saving ? 'Sending limits...' : 'Save and push limits'} <span aria-hidden="true">&#8599;</span></button></div>
      </form>}
    </div>
  </main>
}

function App() {
  const [deviceId, setDeviceId] = useState(() => {
    const savedDeviceId = window.localStorage.getItem(deviceKey) || window.localStorage.getItem(legacyDeviceKey) || ''
    if (savedDeviceId) window.localStorage.setItem(deviceKey, savedDeviceId)
    window.localStorage.removeItem(legacyDeviceKey)
    return savedDeviceId
  })
  const [page, setPage] = useState('home')
  function login(id) { window.localStorage.setItem(deviceKey, id); window.localStorage.removeItem(legacyDeviceKey); setDeviceId(id) }
  function logout() { window.localStorage.removeItem(deviceKey); window.localStorage.removeItem(legacyDeviceKey); setDeviceId(''); setPage('home') }
  if (!deviceId) return <Login onLogin={login} />
  if (page === 'settings') return <Settings deviceId={deviceId} onBack={() => setPage('home')} />
  return <Dashboard deviceId={deviceId} onSettings={() => setPage('settings')} onLogout={logout} />
}

export default App
