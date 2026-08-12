import React, { useEffect, useMemo, useState } from "react";
import { PLUGIN_CONFIG } from "./pluginConfig";
import { onHostFrame, setParam, subscribeParams } from "./bridge";
import { WebGLKnob } from "./components/WebGLKnob";
import { SpectrumCanvas } from "./components/SpectrumCanvas";
import { GainReductionMeter } from "./components/GainReductionMeter";
import { bindingRange, valueToNormalised } from "./paramNormalize";

const MAX_VISIBLE = 7;

function plainEnglishLabel(name: string, id: string) {
  const raw = (name || id || "").trim();
  if (!raw) return id;
  if (/\s/.test(raw)) return raw;
  return raw
    .replace(/([a-z])([A-Z])/g, "$1 $2")
    .replace(/[_-]+/g, " ")
    .replace(/\b\w/g, (c) => c.toUpperCase());
}

export default function App() {
  const { name, accent, params, features } = PLUGIN_CONFIG;

  const ordered = useMemo(() => {
    const list = [...params];
    // Destructive plugins: promote an existing wet/dry param when present (never inject).
    if (features.destructive) {
      const mixIdx = list.findIndex((p) =>
        /^(mix|blend|wet|drywet|wetdry)$/i.test(p.id),
      );
      if (mixIdx > 0) {
        const [mix] = list.splice(mixIdx, 1);
        list.unshift(mix);
      }
    }
    const mainIdx = list.findIndex((p) => p.id === features.mainParamId);
    if (mainIdx > 0) {
      const [main] = list.splice(mainIdx, 1);
      list.unshift(main);
    }
    return list;
  }, [params, features.destructive, features.mainParamId]);

  const visible = ordered.slice(0, MAX_VISIBLE);
  const advanced = ordered.slice(MAX_VISIBLE);

  const [values, setValues] = useState<Record<string, number>>(() =>
    Object.fromEntries(
      params.map((p) => [
        p.id,
        typeof p.defNormalised === "number"
          ? p.defNormalised
          : valueToNormalised(Number(p.def) || 0, bindingRange(p)),
      ]),
    ),
  );
  const [meterDb, setMeterDb] = useState(-100);
  const [glow, setGlow] = useState(0);
  const [spectrumBins, setSpectrumBins] = useState<Float32Array | null>(null);
  const [grCurrent, setGrCurrent] = useState(0);
  const [grPeak, setGrPeak] = useState(0);
  const [expert, setExpert] = useState(false);
  const [cloudMessage, setCloudMessage] = useState<string | null>(null);
  const [showWhatsNew, setShowWhatsNew] = useState(false);

  useEffect(() => {
    document.documentElement.style.setProperty("--accent", accent);
    document.documentElement.style.setProperty("--bg0", "#1A1A1A");
  }, [accent]);

  // Host automation / preset push → same normalised domain as setParam.
  useEffect(
    () =>
      subscribeParams((detail) => {
        if (!detail?.id) return;
        if (!params.some((p) => p.id === detail.id)) return;
        setValues((prev) => ({ ...prev, [detail.id]: detail.value }));
      }),
    [params],
  );

  useEffect(
    () =>
      onHostFrame((msg) => {
        if (msg.type === "meter" && typeof msg.level === "number") {
          setMeterDb(msg.level);
          const g = Math.min(1, Math.max(0, (msg.level + 60) / 60));
          setGlow(g);
        }
        if (msg.type === "spectrum") {
          const arr = msg.bins || msg.magnitudes;
          if (arr?.length) setSpectrumBins(Float32Array.from(arr));
        }
        if (msg.type === "gr") {
          if (typeof msg.current === "number") setGrCurrent(msg.current);
          if (typeof msg.peak === "number") setGrPeak(msg.peak);
        }
        if (msg.type === "cloudUpdate" && typeof msg.message === "string" && msg.message.trim()) {
          setCloudMessage(msg.message.trim());
        }
      }),
    [],
  );

  const update = (id: string, norm: number) => {
    setValues((v) => ({ ...v, [id]: norm }));
    void setParam(id, norm);
    window.__woManusParamChanged?.(id, norm);
  };

  const renderKnob = (p: (typeof params)[number]) => {
    const defNorm =
      typeof p.defNormalised === "number"
        ? p.defNormalised
        : valueToNormalised(Number(p.def) || 0, bindingRange(p));
    const isMain = p.id === features.mainParamId;
    return (
      <WebGLKnob
        key={p.id}
        id={p.id}
        label={plainEnglishLabel(p.name, p.id)}
        value={values[p.id] ?? defNorm}
        defaultValue={defNorm}
        min={p.min}
        max={p.max}
        unit={p.unit || ""}
        accent={accent}
        glow={isMain ? glow : 0}
        onChange={(v) => update(p.id, v)}
      />
    );
  };

  return (
    <div className="shell">
      <header className="plugin-header">
        <h1>{name}</h1>
        {cloudMessage && (
          <button
            type="button"
            className="whats-new-badge"
            title="What's New"
            onClick={() => setShowWhatsNew((v) => !v)}
          >
            What&apos;s New
          </button>
        )}
        <div className="meter" title="Output level">
          <div
            className="meter-fill"
            style={{
              width: `${Math.min(100, Math.max(0, (meterDb + 60) * 1.4))}%`,
            }}
          />
        </div>
      </header>

      {showWhatsNew && cloudMessage && (
        <div className="whats-new-panel" role="status">
          {cloudMessage.split(" · ").map((line) => (
            <p key={line}>{line}</p>
          ))}
        </div>
      )}

      {features.spectrum && (
        <SpectrumCanvas bins={spectrumBins} accent={accent} active />
      )}

      {features.gainReduction && (
        <GainReductionMeter
          currentDb={grCurrent}
          peakDb={grPeak}
          accent={accent}
        />
      )}

      <main className="knob-row">{visible.map(renderKnob)}</main>

      {advanced.length > 0 && (
        <section className="expert">
          <button type="button" onClick={() => setExpert((e) => !e)}>
            {expert ? "Hide advanced" : "Expand advanced"}
          </button>
          {expert && <div className="knob-row">{advanced.map(renderKnob)}</div>}
        </section>
      )}
    </div>
  );
}
