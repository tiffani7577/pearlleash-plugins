import React from "react";

export type GainReductionMeterProps = {
  /** Current GR in dB (typically ≤ 0) */
  currentDb?: number;
  /** Peak-held GR in dB */
  peakDb?: number;
  accent?: string;
};

/** Horizontal gain-reduction meter for dynamics plugins. */
export function GainReductionMeter({
  currentDb = 0,
  peakDb = 0,
  accent = "#00FFAA",
}: GainReductionMeterProps) {
  const toPct = (db: number) =>
    Math.min(100, Math.max(0, (-db / 24) * 100));

  return (
    <div className="gr-meter" title="Gain reduction">
      <span className="gr-label">GR</span>
      <div className="gr-track">
        <div
          className="gr-fill"
          style={{ width: `${toPct(currentDb)}%`, background: accent }}
        />
        <div
          className="gr-peak"
          style={{ left: `${toPct(peakDb)}%`, background: accent }}
        />
      </div>
      <span className="gr-value">{currentDb.toFixed(1)} dB</span>
    </div>
  );
}

export default GainReductionMeter;
