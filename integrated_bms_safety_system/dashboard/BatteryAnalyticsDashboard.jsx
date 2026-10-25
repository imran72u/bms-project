import React, { useState, useEffect, useMemo, useRef } from "react";
import {
  BatteryCharging,
  Zap,
  GitMerge,
  PauseCircle,
  AlertTriangle,
  Clock3,
  ShieldAlert,
  Activity,
  Play,
  Pause,
  RotateCcw,
  Thermometer,
  Gauge,
} from "lucide-react";
import {
  ResponsiveContainer,
  AreaChart,
  Area,
  LineChart,
  Line,
  XAxis,
  YAxis,
  CartesianGrid,
  Tooltip,
  ReferenceLine,
} from "recharts";

/* ---------------------------------------------------------------
   State machine config — matches Task 4 (Idle / Charging /
   Discharging / Balancing / Fault) with dedicated colour + icon
   per state so severity/status reads identically everywhere.
------------------------------------------------------------------ */
const STATES = {
  Idle: { color: "#7C8CA3", glow: "rgba(124,140,163,0.18)", Icon: PauseCircle },
  Charging: { color: "#38BDF8", glow: "rgba(56,189,248,0.18)", Icon: BatteryCharging },
  Discharging: { color: "#FBBF24", glow: "rgba(251,191,36,0.18)", Icon: Zap },
  Balancing: { color: "#A78BFA", glow: "rgba(167,139,250,0.18)", Icon: GitMerge },
  Fault: { color: "#F87171", glow: "rgba(248,113,113,0.22)", Icon: AlertTriangle },
};

const SEVERITY_COLOR = { Warning: "#FBBF24", Critical: "#F87171" };

/* Scripted demonstration scenario: each segment is one state held
   for N ticks. Fault segments carry a fault type/severity that is
   logged once, on entry, to the fault history. */
const SCENARIO = [
  { state: "Idle", ticks: 4 },
  { state: "Charging", ticks: 8 },
  { state: "Balancing", ticks: 5 },
  { state: "Charging", ticks: 4 },
  { state: "Discharging", ticks: 7 },
  { state: "Fault", ticks: 3, faultType: "Overtemperature", severity: "Critical" },
  { state: "Discharging", ticks: 5 },
  { state: "Idle", ticks: 3 },
  { state: "Charging", ticks: 6 },
  { state: "Fault", ticks: 2, faultType: "Cell Imbalance", severity: "Warning" },
  { state: "Balancing", ticks: 5 },
  { state: "Discharging", ticks: 6 },
  { state: "Idle", ticks: 4 },
];
const TOTAL_TICKS = SCENARIO.reduce((s, seg) => s + seg.ticks, 0);
const TICK_SIM_SECONDS = 20; // each tick = 20s of simulated pack time
const TICK_INTERVAL_MS = 480;

function fmtClock(totalSeconds) {
  const h = Math.floor(totalSeconds / 3600);
  const m = Math.floor((totalSeconds % 3600) / 60);
  const s = Math.floor(totalSeconds % 60);
  const pad = (n) => String(n).padStart(2, "0");
  return h > 0 ? `${h}:${pad(m)}:${pad(s)}` : `${pad(m)}:${pad(s)}`;
}

function clamp(v, lo, hi) {
  return Math.max(lo, Math.min(hi, v));
}

/* Advance the underlying pack metrics by one tick given the active
   scenario segment. Pure-ish: reads/writes a metrics object. */
function stepMetrics(prev, segment, isFaultEntryTick) {
  let { soc, imbalance, temp, current } = prev;
  const r = (a, b) => a + Math.random() * (b - a);

  switch (segment.state) {
    case "Idle":
      soc += r(-0.08, 0.08);
      temp += (26 - temp) * 0.08 + r(-0.05, 0.05);
      imbalance += (12 - imbalance) * 0.06 + r(-0.3, 0.3);
      current = r(-0.3, 0.3);
      break;
    case "Charging":
      soc += r(0.55, 0.8);
      temp += r(0.15, 0.35);
      imbalance += r(-0.4, 1.3);
      current = r(17, 19.5);
      break;
    case "Discharging":
      soc -= r(0.5, 0.75);
      temp += r(0.1, 0.3);
      imbalance += r(-0.3, 1.0);
      current = -r(14, 17);
      break;
    case "Balancing":
      imbalance -= r(1.6, 2.6);
      soc += r(-0.05, 0.05);
      temp += (30 - temp) * 0.05 + r(-0.05, 0.1);
      current = r(1.2, 2.4);
      break;
    case "Fault":
      current = r(-0.2, 0.2);
      if (isFaultEntryTick) {
        if (segment.faultType === "Overtemperature") temp += r(4, 6.5);
        if (segment.faultType === "Cell Imbalance") imbalance += r(18, 30);
      } else {
        temp += r(-0.1, 0.3);
        imbalance += r(-0.2, 0.6);
      }
      break;
    default:
      break;
  }

  soc = clamp(soc, 0, 100);
  imbalance = clamp(imbalance, 3, 160);
  temp = clamp(temp, 18, 58);

  return { soc, imbalance, temp, current };
}

function computeRisk(imbalance, recentFaultCount, soc) {
  const imbalanceRisk = clamp(((imbalance - 20) / 90) * 100, 0, 100);
  const faultFreqRisk = clamp((recentFaultCount / 3) * 100, 0, 100);
  const socDistance = soc < 20 ? (20 - soc) / 20 : soc > 92 ? (soc - 92) / 8 : 0;
  const socRisk = clamp(socDistance * 100, 0, 100);
  const composite = 0.35 * imbalanceRisk + 0.35 * faultFreqRisk + 0.3 * socRisk;
  return {
    composite: Math.round(composite),
    imbalanceRisk: Math.round(imbalanceRisk),
    faultFreqRisk: Math.round(faultFreqRisk),
    socRisk: Math.round(socRisk),
  };
}

function riskLabel(score) {
  if (score < 25) return { label: "Low", color: "#4ADE80" };
  if (score < 55) return { label: "Moderate", color: "#FBBF24" };
  return { label: "Elevated", color: "#F87171" };
}

function buildRecommendations({ imbalance, temp, soc, riskFactors, recentFaults }) {
  const recs = [];
  if (imbalance > 75)
    recs.push({
      severity: "Critical",
      text: `Cell imbalance is ${imbalance.toFixed(0)} mV — schedule an active balancing cycle before the next charge.`,
    });
  else if (imbalance > 40)
    recs.push({
      severity: "Warning",
      text: `Imbalance trending up (${imbalance.toFixed(0)} mV). Monitor; balance if it stays above 40 mV for another cycle.`,
    });

  if (temp > 45)
    recs.push({
      severity: "Critical",
      text: `Pack temperature at ${temp.toFixed(1)}°C. Check cooling airflow and reduce load until it drops below 40°C.`,
    });

  if (recentFaults >= 2)
    recs.push({
      severity: "Warning",
      text: `${recentFaults} faults logged in the recent window — run a full BMS diagnostic before the pack's next deployment.`,
    });

  if (soc < 15)
    recs.push({
      severity: "Warning",
      text: `SoC is low (${soc.toFixed(0)}%). Recommend charging soon to avoid a deep-discharge event.`,
    });
  if (soc > 96)
    recs.push({
      severity: "Info",
      text: `SoC near full (${soc.toFixed(0)}%). Consider tapering charge current to reduce cell stress.`,
    });

  if (recs.length === 0)
    recs.push({
      severity: "Info",
      text: "All monitored factors are within nominal range — no action required right now.",
    });

  return recs.slice(0, 4);
}

/* ---------------------------------------------------------------
   Small bespoke radial gauge for the composite risk score.
------------------------------------------------------------------ */
function RiskGauge({ score }) {
  const { label, color } = riskLabel(score);
  const r = 62;
  const circumference = Math.PI * r; // half circle
  const offset = circumference * (1 - score / 100);
  return (
    <div style={{ display: "flex", flexDirection: "column", alignItems: "center", gap: 6 }}>
      <svg width="160" height="98" viewBox="0 0 160 98">
        <path
          d="M 14 90 A 62 62 0 0 1 146 90"
          fill="none"
          stroke="#1E2E45"
          strokeWidth="12"
          strokeLinecap="round"
        />
        <path
          d="M 14 90 A 62 62 0 0 1 146 90"
          fill="none"
          stroke={color}
          strokeWidth="12"
          strokeLinecap="round"
          strokeDasharray={circumference}
          strokeDashoffset={offset}
          style={{ transition: "stroke-dashoffset 0.4s ease, stroke 0.4s ease" }}
        />
      </svg>
      <div style={{ marginTop: -46, textAlign: "center" }}>
        <div style={{ fontFamily: "'Space Grotesk', sans-serif", fontSize: 34, fontWeight: 600, color: "#E8EEF5" }}>
          {score}
        </div>
        <div style={{ fontSize: 12, color, fontWeight: 600 }}>{label} risk</div>
      </div>
    </div>
  );
}

function FactorBar({ label, value, color }) {
  return (
    <div style={{ marginBottom: 10 }}>
      <div style={{ display: "flex", justifyContent: "space-between", fontSize: 12.5, color: "#9FB0C3", marginBottom: 4 }}>
        <span>{label}</span>
        <span style={{ color: "#E8EEF5", fontFamily: "'Space Grotesk', sans-serif" }}>{value}</span>
      </div>
      <div style={{ height: 6, borderRadius: 4, background: "#1A2637", overflow: "hidden" }}>
        <div
          style={{
            width: `${value}%`,
            height: "100%",
            background: color,
            borderRadius: 4,
            transition: "width 0.4s ease",
          }}
        />
      </div>
    </div>
  );
}

function StateBadge({ state, size = "md" }) {
  const cfg = STATES[state];
  const Icon = cfg.Icon;
  const big = size === "lg";
  return (
    <div
      style={{
        display: "inline-flex",
        alignItems: "center",
        gap: 8,
        padding: big ? "8px 14px" : "5px 10px",
        borderRadius: 999,
        background: cfg.glow,
        border: `1px solid ${cfg.color}55`,
      }}
    >
      <Icon size={big ? 18 : 14} color={cfg.color} strokeWidth={2.2} />
      <span style={{ color: cfg.color, fontWeight: 600, fontSize: big ? 15 : 12.5, fontFamily: "'Space Grotesk', sans-serif" }}>
        {state}
      </span>
    </div>
  );
}

/* ================================================================
   Main dashboard
================================================================= */
export default function BatteryAnalyticsDashboard() {
  const [tickIndex, setTickIndex] = useState(0); // global tick counter
  const [running, setRunning] = useState(false);
  const [finished, setFinished] = useState(false);
  const [series, setSeries] = useState([]); // time-series rows
  const [faults, setFaults] = useState([]);
  const metricsRef = useRef({ soc: 55, imbalance: 14, temp: 27, current: 0 });
  const intervalRef = useRef(null);

  // Resolve which scenario segment a given global tick falls in.
  const segmentInfo = (idx) => {
    let acc = 0;
    for (const seg of SCENARIO) {
      if (idx < acc + seg.ticks) return { seg, tickInSeg: idx - acc };
      acc += seg.ticks;
    }
    return { seg: SCENARIO[SCENARIO.length - 1], tickInSeg: 999 };
  };

  useEffect(() => {
    if (!running) return;
    intervalRef.current = setInterval(() => {
      setTickIndex((i) => {
        const next = i + 1;
        if (next > TOTAL_TICKS) {
          setRunning(false);
          setFinished(true);
          return i;
        }
        const { seg, tickInSeg } = segmentInfo(i);
        const isFaultEntry = seg.state === "Fault" && tickInSeg === 0;
        const m = stepMetrics(metricsRef.current, seg, isFaultEntry);
        metricsRef.current = m;

        if (isFaultEntry) {
          setFaults((f) => [
            ...f,
            {
              time: fmtClock(i * TICK_SIM_SECONDS),
              type: seg.faultType,
              severity: seg.severity,
              state: seg.state,
            },
          ]);
        }

        setSeries((s) => {
          const recentFaultCount = faultsWithinWindow(s, i);
          const risk = computeRisk(m.imbalance, recentFaultCount, m.soc);
          return [
            ...s,
            {
              t: i,
              timeLabel: fmtClock(i * TICK_SIM_SECONDS),
              state: seg.state,
              soc: Number(m.soc.toFixed(1)),
              temp: Number(m.temp.toFixed(1)),
              imbalance: Number(m.imbalance.toFixed(1)),
              current: Number(m.current.toFixed(1)),
              risk: risk.composite,
            },
          ];
        });
        return next;
      });
    }, TICK_INTERVAL_MS);
    return () => clearInterval(intervalRef.current);
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [running]);

  function faultsWithinWindow(currentSeries, atTick) {
    // faults are tracked separately; approximate recent window as last 15 ticks using `faults` state via closure below
    return recentFaultsCountRef.current(atTick);
  }

  // Keep a ref-based counter of faults within the last 15 ticks, computed from `faults` state.
  const recentFaultsCountRef = useRef(() => 0);
  useEffect(() => {
    recentFaultsCountRef.current = (atTick) => {
      const windowStart = (atTick - 15) * TICK_SIM_SECONDS;
      return faults.filter((f) => {
        const parts = f.time.split(":").map(Number);
        const secs = parts.length === 3 ? parts[0] * 3600 + parts[1] * 60 + parts[2] : parts[0] * 60 + parts[1];
        return secs >= windowStart;
      }).length;
    };
  }, [faults]);

  function handlePlayPause() {
    if (finished) return;
    setRunning((r) => !r);
  }
  function handleReset() {
    clearInterval(intervalRef.current);
    setRunning(false);
    setFinished(false);
    setTickIndex(0);
    setSeries([]);
    setFaults([]);
    metricsRef.current = { soc: 55, imbalance: 14, temp: 27, current: 0 };
  }

  const last = series[series.length - 1];
  const currentState = last ? last.state : "Idle";
  const uptimeSeconds = tickIndex * TICK_SIM_SECONDS;
  const recentFaultCount = faultsWithinWindow(series, tickIndex);
  const risk = last ? computeRisk(last.imbalance, recentFaultCount, last.soc) : { composite: 0, imbalanceRisk: 0, faultFreqRisk: 0, socRisk: 0 };
  const overallHealth = clamp(100 - risk.composite, 0, 100);
  const healthLabel = overallHealth >= 75 ? "Good" : overallHealth >= 50 ? "Fair" : "Poor";
  const healthColor = overallHealth >= 75 ? "#4ADE80" : overallHealth >= 50 ? "#FBBF24" : "#F87171";

  const recommendations = useMemo(
    () =>
      buildRecommendations({
        imbalance: last ? last.imbalance : 14,
        temp: last ? last.temp : 27,
        soc: last ? last.soc : 55,
        riskFactors: risk,
        recentFaults: recentFaultCount,
      }),
    // eslint-disable-next-line react-hooks/exhaustive-deps
    [last, recentFaultCount]
  );

  const progressPct = clamp((tickIndex / TOTAL_TICKS) * 100, 0, 100);

  return (
    <div style={{ background: "#0B1420", minHeight: "100%", padding: "28px 22px", fontFamily: "'Inter', sans-serif", color: "#E8EEF5" }}>
      <style>{`
        @import url('https://fonts.googleapis.com/css2?family=Space+Grotesk:wght@500;600;700&family=Inter:wght@400;500;600&display=swap');
        .pw-panel { background:#121D2E; border:1px solid #1E2E45; border-radius:10px; }
        .pw-scroll::-webkit-scrollbar { width:6px; height:6px; }
        .pw-scroll::-webkit-scrollbar-thumb { background:#2A3B54; border-radius:4px; }
        .pw-btn { display:inline-flex; align-items:center; gap:6px; border:1px solid #26374F; background:#16212F; color:#E8EEF5; padding:8px 14px; border-radius:7px; font-size:13px; font-weight:500; cursor:pointer; }
        .pw-btn:hover { border-color:#38BDF8; }
        .pw-btn-primary { background:#1C6E8C; border-color:#1C6E8C; }
        .pw-btn-primary:hover { border-color:#35D0C0; }
      `}</style>

      {/* Header */}
      <div style={{ display: "flex", alignItems: "center", justifyContent: "space-between", flexWrap: "wrap", gap: 14, marginBottom: 20 }}>
        <div style={{ display: "flex", alignItems: "center", gap: 12 }}>
          <div style={{ width: 38, height: 38, borderRadius: 9, background: "#12384A", display: "flex", alignItems: "center", justifyContent: "center" }}>
            <Activity size={20} color="#35D0C0" />
          </div>
          <div>
            <div style={{ fontFamily: "'Space Grotesk', sans-serif", fontSize: 19, fontWeight: 600 }}>PackWatch Analytics</div>
            <div style={{ fontSize: 12.5, color: "#7C8CA3" }}>BMS decision dashboard · simulated demonstration run</div>
          </div>
        </div>
        <div style={{ display: "flex", alignItems: "center", gap: 10 }}>
          <StateBadge state={currentState} size="lg" />
          <button className="pw-btn pw-btn-primary" onClick={handlePlayPause} disabled={finished}>
            {running ? <Pause size={14} /> : <Play size={14} />}
            {finished ? "Complete" : running ? "Pause" : tickIndex === 0 ? "Run demo" : "Resume"}
          </button>
          <button className="pw-btn" onClick={handleReset}>
            <RotateCcw size={14} />
            Reset
          </button>
        </div>
      </div>

      {/* Progress bar for the demonstration run */}
      <div style={{ marginBottom: 22 }}>
        <div style={{ height: 5, borderRadius: 3, background: "#1A2637", overflow: "hidden" }}>
          <div style={{ width: `${progressPct}%`, height: "100%", background: "#35D0C0", transition: "width 0.3s linear" }} />
        </div>
      </div>

      {/* Executive summary */}
      <div style={{ display: "grid", gridTemplateColumns: "repeat(auto-fit, minmax(190px, 1fr))", gap: 14, marginBottom: 18 }}>
        <SummaryCard icon={<Gauge size={16} color={healthColor} />} label="Overall health" value={`${overallHealth}%`} sub={healthLabel} subColor={healthColor} />
        <SummaryCard icon={<Clock3 size={16} color="#35D0C0" />} label="Simulated uptime" value={fmtClock(uptimeSeconds)} sub={finished ? "Demo complete" : running ? "Running" : "Paused"} subColor="#7C8CA3" />
        <SummaryCard icon={<ShieldAlert size={16} color="#F87171" />} label="Fault count" value={faults.length} sub={faults.length ? `Last: ${faults[faults.length - 1].type}` : "None logged"} subColor="#7C8CA3" />
        <SummaryCard icon={<Thermometer size={16} color="#FBBF24" />} label="Pack temperature" value={last ? `${last.temp.toFixed(1)}°C` : "—"} sub={last && last.temp > 45 ? "Above threshold" : "Nominal"} subColor={last && last.temp > 45 ? "#F87171" : "#7C8CA3"} />
      </div>

      {/* State timeline */}
      <div className="pw-panel" style={{ padding: 16, marginBottom: 18 }}>
        <PanelTitle>State transition timeline</PanelTitle>
        <div style={{ display: "flex", gap: 2, height: 26, marginTop: 10, borderRadius: 5, overflow: "hidden", background: "#0E1826" }}>
          {series.length === 0 && <div style={{ color: "#4C5C72", fontSize: 12.5, padding: "5px 8px" }}>Run the demo to populate the timeline…</div>}
          {series.map((row) => (
            <div key={row.t} title={`${row.timeLabel} — ${row.state}`} style={{ flex: 1, background: STATES[row.state].color, minWidth: 2 }} />
          ))}
        </div>
        <div style={{ display: "flex", gap: 16, marginTop: 12, flexWrap: "wrap" }}>
          {Object.entries(STATES).map(([name, cfg]) => (
            <div key={name} style={{ display: "flex", alignItems: "center", gap: 6, fontSize: 12, color: "#9FB0C3" }}>
              <span style={{ width: 10, height: 10, borderRadius: 3, background: cfg.color, display: "inline-block" }} />
              {name}
            </div>
          ))}
        </div>
      </div>

      {/* Charts + risk row */}
      <div style={{ display: "grid", gridTemplateColumns: "minmax(0,1.7fr) minmax(260px,1fr)", gap: 16, marginBottom: 18 }}>
        <div style={{ display: "flex", flexDirection: "column", gap: 16 }}>
          <div className="pw-panel" style={{ padding: 16 }}>
            <PanelTitle>State of charge &amp; temperature</PanelTitle>
            <ResponsiveContainer width="100%" height={190}>
              <LineChart data={series} margin={{ top: 10, right: 10, left: -18, bottom: 0 }}>
                <CartesianGrid stroke="#1A2637" vertical={false} />
                <XAxis dataKey="timeLabel" stroke="#4C5C72" fontSize={11} tickLine={false} minTickGap={40} />
                <YAxis yAxisId="soc" stroke="#4C5C72" fontSize={11} tickLine={false} domain={[0, 100]} />
                <YAxis yAxisId="temp" orientation="right" stroke="#4C5C72" fontSize={11} tickLine={false} domain={[15, 60]} />
                <Tooltip contentStyle={{ background: "#16212F", border: "1px solid #26374F", borderRadius: 8, fontSize: 12 }} labelStyle={{ color: "#9FB0C3" }} />
                <Line yAxisId="soc" type="monotone" dataKey="soc" name="SoC %" stroke="#38BDF8" strokeWidth={2} dot={false} />
                <Line yAxisId="temp" type="monotone" dataKey="temp" name="Temp °C" stroke="#FBBF24" strokeWidth={2} dot={false} />
              </LineChart>
            </ResponsiveContainer>
          </div>

          <div className="pw-panel" style={{ padding: 16 }}>
            <PanelTitle>Cell imbalance trend</PanelTitle>
            <ResponsiveContainer width="100%" height={170}>
              <AreaChart data={series} margin={{ top: 10, right: 10, left: -18, bottom: 0 }}>
                <defs>
                  <linearGradient id="imbFill" x1="0" y1="0" x2="0" y2="1">
                    <stop offset="0%" stopColor="#A78BFA" stopOpacity={0.5} />
                    <stop offset="100%" stopColor="#A78BFA" stopOpacity={0.03} />
                  </linearGradient>
                </defs>
                <CartesianGrid stroke="#1A2637" vertical={false} />
                <XAxis dataKey="timeLabel" stroke="#4C5C72" fontSize={11} tickLine={false} minTickGap={40} />
                <YAxis stroke="#4C5C72" fontSize={11} tickLine={false} domain={[0, 160]} />
                <Tooltip contentStyle={{ background: "#16212F", border: "1px solid #26374F", borderRadius: 8, fontSize: 12 }} labelStyle={{ color: "#9FB0C3" }} />
                <ReferenceLine y={80} stroke="#F87171" strokeDasharray="4 4" label={{ value: "Risk threshold", fill: "#F87171", fontSize: 11, position: "insideTopRight" }} />
                <Area type="monotone" dataKey="imbalance" name="Imbalance (mV)" stroke="#A78BFA" strokeWidth={2} fill="url(#imbFill)" />
              </AreaChart>
            </ResponsiveContainer>
          </div>
        </div>

        {/* Risk panel */}
        <div className="pw-panel" style={{ padding: 16, display: "flex", flexDirection: "column" }}>
          <PanelTitle>Composite risk score</PanelTitle>
          <div style={{ display: "flex", justifyContent: "center", marginTop: 4 }}>
            <RiskGauge score={risk.composite} />
          </div>
          <div style={{ marginTop: 8 }}>
            <FactorBar label="Imbalance trend" value={risk.imbalanceRisk} color="#A78BFA" />
            <FactorBar label="Fault frequency" value={risk.faultFreqRisk} color="#F87171" />
            <FactorBar label="SoC risk" value={risk.socRisk} color="#38BDF8" />
          </div>
        </div>
      </div>

      {/* Recommendations + fault history */}
      <div style={{ display: "grid", gridTemplateColumns: "minmax(260px,1fr) minmax(0,1.7fr)", gap: 16 }}>
        <div className="pw-panel" style={{ padding: 16 }}>
          <PanelTitle>Operator recommendations</PanelTitle>
          <div style={{ display: "flex", flexDirection: "column", gap: 10, marginTop: 10 }}>
            {recommendations.map((rec, i) => (
              <div key={i} style={{ display: "flex", gap: 10, alignItems: "flex-start", padding: "10px 12px", background: "#0E1826", borderRadius: 8, border: "1px solid #1E2E45" }}>
                <span
                  style={{
                    width: 8,
                    height: 8,
                    borderRadius: 999,
                    marginTop: 5,
                    flexShrink: 0,
                    background: rec.severity === "Critical" ? "#F87171" : rec.severity === "Warning" ? "#FBBF24" : "#35D0C0",
                  }}
                />
                <span style={{ fontSize: 13, color: "#C8D3E0", lineHeight: 1.5 }}>{rec.text}</span>
              </div>
            ))}
          </div>
        </div>

        <div className="pw-panel" style={{ padding: 16 }}>
          <PanelTitle>Fault history</PanelTitle>
          <div className="pw-scroll" style={{ marginTop: 10, maxHeight: 220, overflowY: "auto" }}>
            <table style={{ width: "100%", borderCollapse: "collapse", fontSize: 13 }}>
              <thead>
                <tr style={{ textAlign: "left", color: "#7C8CA3", fontSize: 11.5 }}>
                  <th style={{ padding: "6px 8px", fontWeight: 500 }}>Time</th>
                  <th style={{ padding: "6px 8px", fontWeight: 500 }}>Fault type</th>
                  <th style={{ padding: "6px 8px", fontWeight: 500 }}>Severity</th>
                  <th style={{ padding: "6px 8px", fontWeight: 500 }}>State at time</th>
                </tr>
              </thead>
              <tbody>
                {faults.length === 0 && (
                  <tr>
                    <td colSpan={4} style={{ padding: "14px 8px", color: "#4C5C72" }}>
                      No faults logged yet.
                    </td>
                  </tr>
                )}
                {faults.map((f, i) => (
                  <tr key={i} style={{ borderTop: "1px solid #1E2E45" }}>
                    <td style={{ padding: "8px", color: "#9FB0C3", fontFamily: "'Space Grotesk', sans-serif" }}>{f.time}</td>
                    <td style={{ padding: "8px", color: "#E8EEF5" }}>{f.type}</td>
                    <td style={{ padding: "8px" }}>
                      <span style={{ color: SEVERITY_COLOR[f.severity], fontWeight: 600 }}>{f.severity}</span>
                    </td>
                    <td style={{ padding: "8px" }}>
                      <StateBadge state={f.state} />
                    </td>
                  </tr>
                ))}
              </tbody>
            </table>
          </div>
        </div>
      </div>
    </div>
  );
}

function PanelTitle({ children }) {
  return <div style={{ fontSize: 13.5, fontWeight: 600, color: "#C8D3E0" }}>{children}</div>;
}

function SummaryCard({ icon, label, value, sub, subColor }) {
  return (
    <div className="pw-panel" style={{ padding: 14 }}>
      <div style={{ display: "flex", alignItems: "center", gap: 7, marginBottom: 8 }}>
        {icon}
        <span style={{ fontSize: 12, color: "#7C8CA3" }}>{label}</span>
      </div>
      <div style={{ fontFamily: "'Space Grotesk', sans-serif", fontSize: 24, fontWeight: 600 }}>{value}</div>
      <div style={{ fontSize: 11.5, color: subColor, marginTop: 3 }}>{sub}</div>
    </div>
  );
}
