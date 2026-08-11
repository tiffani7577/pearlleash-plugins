import React, { useEffect, useRef } from "react";

export type SpectrumCanvasProps = {
  /** Bin magnitudes 0..1 (or raw FFT magnitudes) — from C++ postMessage or AnalyserNode */
  bins?: Float32Array | number[] | null;
  accent?: string;
  /** Show GR-style empty state when no data */
  active?: boolean;
};

/**
 * WebGL (with 2D fallback) spectrum — log frequency axis, 60fps, smooth decay.
 * Prefers host-pushed bins; falls back to Web Audio AnalyserNode in browser preview.
 */
export function SpectrumCanvas({
  bins = null,
  accent = "#00FFAA",
  active = true,
}: SpectrumCanvasProps) {
  const canvasRef = useRef<HTMLCanvasElement | null>(null);
  const smoothRef = useRef<Float32Array | null>(null);
  const binsRef = useRef<Float32Array | null>(null);
  const rafRef = useRef(0);

  useEffect(() => {
    if (bins && bins.length) {
      binsRef.current =
        bins instanceof Float32Array ? bins : Float32Array.from(bins);
    }
  }, [bins]);

  useEffect(() => {
    const canvas = canvasRef.current;
    if (!canvas || !active) return;

    let audioCtx: AudioContext | null = null;
    let analyser: AnalyserNode | null = null;
    let localBins: Uint8Array | null = null;
    let useWebAudio = !binsRef.current;

    const tryWebAudio = async () => {
      try {
        audioCtx = new AudioContext();
        analyser = audioCtx.createAnalyser();
        analyser.fftSize = 2048;
        analyser.smoothingTimeConstant = 0;
        localBins = new Uint8Array(analyser.frequencyBinCount);
        // Oscillator stub so preview has motion without mic permission.
        const osc = audioCtx.createOscillator();
        const gain = audioCtx.createGain();
        gain.gain.value = 0.0001;
        osc.connect(gain);
        gain.connect(analyser);
        osc.start();
        useWebAudio = true;
      } catch {
        useWebAudio = false;
      }
    };

    if (useWebAudio) void tryWebAudio();

    const gl = canvas.getContext("webgl", { antialias: true, alpha: true });
    const ctx2d = !gl ? canvas.getContext("2d") : null;

    const resize = () => {
      const parent = canvas.parentElement;
      const cssW = parent?.clientWidth || 480;
      const cssH = parent?.clientHeight || 120;
      const dpr = window.devicePixelRatio || 1;
      canvas.width = Math.max(2, Math.floor(cssW * dpr));
      canvas.height = Math.max(2, Math.floor(cssH * dpr));
      canvas.style.width = `${cssW}px`;
      canvas.style.height = `${cssH}px`;
      if (gl) gl.viewport(0, 0, canvas.width, canvas.height);
    };
    resize();
    window.addEventListener("resize", resize);

    let prog: WebGLProgram | null = null;
    let buf: WebGLBuffer | null = null;
    if (gl) {
      const vs = gl.createShader(gl.VERTEX_SHADER)!;
      gl.shaderSource(
        vs,
        `attribute vec2 a; void main(){ gl_Position = vec4(a,0.0,1.0); }`,
      );
      gl.compileShader(vs);
      const fs = gl.createShader(gl.FRAGMENT_SHADER)!;
      gl.shaderSource(
        fs,
        `precision mediump float; uniform vec3 uColor; void main(){ gl_FragColor = vec4(uColor,1.0); }`,
      );
      gl.compileShader(fs);
      prog = gl.createProgram()!;
      gl.attachShader(prog, vs);
      gl.attachShader(prog, fs);
      gl.linkProgram(prog);
      buf = gl.createBuffer();
    }

    const tick = () => {
      let raw: Float32Array;
      if (binsRef.current && binsRef.current.length) {
        raw = binsRef.current;
      } else if (analyser && localBins) {
        analyser.getByteFrequencyData(localBins);
        raw = new Float32Array(localBins.length);
        for (let i = 0; i < localBins.length; i++) raw[i] = localBins[i] / 255;
      } else {
        raw = new Float32Array(256);
      }

      if (!smoothRef.current || smoothRef.current.length !== raw.length) {
        smoothRef.current = new Float32Array(raw.length);
      }
      const sm = smoothRef.current;
      for (let i = 0; i < raw.length; i++) {
        sm[i] = sm[i] * 0.85 + raw[i] * 0.15;
      }

      if (gl && prog && buf) {
        drawWebGL(gl, prog, buf, sm, accent, canvas.width, canvas.height);
      } else if (ctx2d) {
        draw2D(ctx2d, sm, accent, canvas.width, canvas.height);
      }
      rafRef.current = requestAnimationFrame(tick);
    };
    rafRef.current = requestAnimationFrame(tick);

    return () => {
      cancelAnimationFrame(rafRef.current);
      window.removeEventListener("resize", resize);
      void audioCtx?.close();
    };
  }, [accent, active]);

  return (
    <div className="spectrum-wrap">
      <canvas ref={canvasRef} className="spectrum-canvas" />
    </div>
  );
}

function drawGrid(
  ctx: CanvasRenderingContext2D,
  w: number,
  h: number,
) {
  ctx.strokeStyle = "rgba(255,255,255,0.06)";
  ctx.lineWidth = 1;
  for (let i = 1; i < 4; i++) {
    const y = (h * i) / 4;
    ctx.beginPath();
    ctx.moveTo(0, y);
    ctx.lineTo(w, y);
    ctx.stroke();
  }
  for (let i = 1; i < 8; i++) {
    const x = (w * i) / 8;
    ctx.beginPath();
    ctx.moveTo(x, 0);
    ctx.lineTo(x, h);
    ctx.stroke();
  }
}

function draw2D(
  ctx: CanvasRenderingContext2D,
  bins: Float32Array,
  accent: string,
  w: number,
  h: number,
) {
  ctx.setTransform(1, 0, 0, 1, 0, 0);
  ctx.clearRect(0, 0, w, h);
  ctx.fillStyle = "#121212";
  ctx.fillRect(0, 0, w, h);
  drawGrid(ctx, w, h);

  const n = bins.length;
  ctx.strokeStyle = accent;
  ctx.lineWidth = Math.max(1.5, w / 400);
  ctx.beginPath();
  for (let i = 0; i < n; i++) {
    const t = i / (n - 1);
    // Logarithmic x
    const x = (Math.pow(n, t) - 1) / (n - 1) * (w - 2) + 1;
    const y = h - bins[i] * (h - 4) - 2;
    if (i === 0) ctx.moveTo(x, y);
    else ctx.lineTo(x, y);
  }
  ctx.stroke();
}

function drawWebGL(
  gl: WebGLRenderingContext,
  prog: WebGLProgram,
  buf: WebGLBuffer,
  bins: Float32Array,
  accent: string,
  w: number,
  h: number,
) {
  gl.viewport(0, 0, w, h);
  gl.clearColor(0.07, 0.07, 0.07, 1);
  gl.clear(gl.COLOR_BUFFER_BIT);

  const n = bins.length;
  const verts = new Float32Array(n * 2);
  for (let i = 0; i < n; i++) {
    const t = i / (n - 1);
    const xNorm = (Math.pow(n, t) - 1) / (n - 1);
    verts[i * 2] = xNorm * 2 - 1;
    verts[i * 2 + 1] = bins[i] * 2 - 1;
  }

  gl.useProgram(prog);
  gl.bindBuffer(gl.ARRAY_BUFFER, buf);
  gl.bufferData(gl.ARRAY_BUFFER, verts, gl.DYNAMIC_DRAW);
  const loc = gl.getAttribLocation(prog, "a");
  gl.enableVertexAttribArray(loc);
  gl.vertexAttribPointer(loc, 2, gl.FLOAT, false, 0, 0);
  const rgb = hexToRgb(accent);
  const u = gl.getUniformLocation(prog, "uColor");
  gl.uniform3f(u, rgb[0], rgb[1], rgb[2]);
  gl.lineWidth(2);
  gl.drawArrays(gl.LINE_STRIP, 0, n);
}

function hexToRgb(hex: string): [number, number, number] {
  const h = hex.replace("#", "");
  const full = h.length === 3 ? h.split("").map((c) => c + c).join("") : h;
  const n = parseInt(full.slice(0, 6), 16);
  return [((n >> 16) & 255) / 255, ((n >> 8) & 255) / 255, (n & 255) / 255];
}

export default SpectrumCanvas;
