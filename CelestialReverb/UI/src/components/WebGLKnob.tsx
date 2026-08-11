import React, { useCallback, useEffect, useRef, useState } from "react";

export type WebGLKnobProps = {
  id: string;
  label: string;
  /** Normalized 0..1 */
  value: number;
  defaultValue?: number;
  min?: number;
  max?: number;
  unit?: string;
  accent?: string;
  /** 0..1 glow intensity from audio level */
  glow?: number;
  onChange: (normalized: number) => void;
};

/**
 * Canvas knob — vertical drag (not circular), neon value arc, audio-reactive glow.
 * Named WebGLKnob per product contract; draws via Canvas2D for RT-safe UI threads.
 */
export function WebGLKnob({
  id,
  label,
  value,
  defaultValue = 0.5,
  min = 0,
  max = 1,
  unit = "",
  accent = "#00FFAA",
  glow = 0,
  onChange,
}: WebGLKnobProps) {
  const canvasRef = useRef<HTMLCanvasElement | null>(null);
  const [hover, setHover] = useState(false);
  const [dragging, setDragging] = useState(false);

  const displayValue = min + value * (max - min);

  const paint = useCallback(() => {
    const canvas = canvasRef.current;
    if (!canvas) return;
    const ctx = canvas.getContext("2d");
    if (!ctx) return;

    const dpr = window.devicePixelRatio || 1;
    const css = 96;
    if (canvas.width !== css * dpr) {
      canvas.width = css * dpr;
      canvas.height = css * dpr;
      canvas.style.width = `${css}px`;
      canvas.style.height = `${css}px`;
    }
    ctx.setTransform(dpr, 0, 0, dpr, 0, 0);

    const w = css;
    const h = css;
    const cx = w * 0.5;
    const cy = h * 0.5;
    const radius = Math.min(w, h) * 0.38;
    const start = -Math.PI * 0.75;
    const end = Math.PI * 0.75;
    const angle = start + value * (end - start);
    const glowAmt = Math.max(0, Math.min(1, glow));

    ctx.clearRect(0, 0, w, h);

    if (glowAmt > 0.01) {
      const g = ctx.createRadialGradient(cx, cy, radius * 0.4, cx, cy, radius + 10);
      g.addColorStop(0, hexAlpha(accent, glowAmt * 0.35));
      g.addColorStop(1, hexAlpha(accent, 0));
      ctx.fillStyle = g;
      ctx.beginPath();
      ctx.arc(cx, cy, radius + 10, 0, Math.PI * 2);
      ctx.fill();
    }

    ctx.fillStyle = "#1A1A1A";
    ctx.beginPath();
    ctx.arc(cx, cy, radius, 0, Math.PI * 2);
    ctx.fill();

    ctx.strokeStyle = "#2A2A2A";
    ctx.lineWidth = 4;
    ctx.beginPath();
    ctx.arc(cx, cy, radius - 4, start, end);
    ctx.stroke();

    ctx.strokeStyle = accent;
    ctx.lineWidth = 3;
    ctx.lineCap = "round";
    ctx.beginPath();
    ctx.arc(cx, cy, radius - 4, start, angle);
    ctx.stroke();

    const px = cx + Math.cos(angle) * (radius - 10);
    const py = cy + Math.sin(angle) * (radius - 10);
    ctx.fillStyle = accent;
    ctx.beginPath();
    ctx.arc(px, py, 3.5, 0, Math.PI * 2);
    ctx.fill();
  }, [accent, glow, value]);

  useEffect(() => {
    paint();
  }, [paint]);

  const beginDrag = (e: React.PointerEvent) => {
    e.preventDefault();
    const startY = e.clientY;
    const startVal = value;
    const fine = e.shiftKey;
    setDragging(true);

    const move = (ev: PointerEvent) => {
      const sens = fine || ev.shiftKey ? 400 : 120;
      const next = clamp01(startVal + (startY - ev.clientY) / sens);
      onChange(next);
    };
    const up = () => {
      setDragging(false);
      window.removeEventListener("pointermove", move);
      window.removeEventListener("pointerup", up);
    };
    window.addEventListener("pointermove", move);
    window.addEventListener("pointerup", up);
  };

  return (
    <label className="knob" data-param={id} title={label}>
      <div className="knob-wrap">
        <canvas
          ref={canvasRef}
          className="knob-canvas"
          onPointerDown={beginDrag}
          onDoubleClick={() => onChange(clamp01(defaultValue))}
          onMouseEnter={() => setHover(true)}
          onMouseLeave={() => setHover(false)}
        />
        {hover && (
          <div className="knob-tooltip">
            {formatValue(displayValue, unit)}
          </div>
        )}
      </div>
      <span className="knob-label">{label}</span>
    </label>
  );
}

function clamp01(v: number) {
  return Math.min(1, Math.max(0, v));
}

function formatValue(v: number, unit: string) {
  const abs = Math.abs(v);
  const digits = abs >= 100 ? 0 : abs >= 10 ? 1 : 2;
  return `${v.toFixed(digits)}${unit ? ` ${unit}` : ""}`;
}

function hexAlpha(hex: string, a: number) {
  const h = hex.replace("#", "");
  const full = h.length === 3 ? h.split("").map((c) => c + c).join("") : h;
  const n = parseInt(full.slice(0, 6), 16);
  const r = (n >> 16) & 255;
  const g = (n >> 8) & 255;
  const b = n & 255;
  return `rgba(${r},${g},${b},${a})`;
}

export default WebGLKnob;
