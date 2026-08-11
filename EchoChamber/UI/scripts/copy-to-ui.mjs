import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

const root = path.dirname(path.dirname(fileURLToPath(import.meta.url)));
const dist = path.join(root, "dist");
const out = path.join(path.dirname(root), "ui");
fs.mkdirSync(out, { recursive: true });
for (const name of fs.readdirSync(dist)) {
  fs.cpSync(path.join(dist, name), path.join(out, name), { recursive: true });
}
console.log("[UI] copied Vite dist → ../ui for WebBrowserComponent BinaryData");
