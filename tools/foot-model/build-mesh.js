#!/usr/bin/env node
/*
  Rebuilds the 3D foot mesh that is embedded in web/index.html.

  The foot shape lives in web/index.html itself (castSDF and the FP / TOES tables in the
  CORE section). Change it there, then run:

      node tools/foot-model/build-mesh.js

  The script meshes the shape and rewrites the `const MESH={...}` line in web/index.html.
  Needs only Node.js (no packages).
*/
const fs = require('fs'), path = require('path');
const file = path.join(__dirname, '..', '..', 'web', 'index.html');
const html = fs.readFileSync(file, 'utf8');
const core = html.slice(html.indexOf('/*CORE-START*/'), html.indexOf('/*CORE-END*/'));
const { castSDF, surfaceNets } = new Function(core + '\nreturn {castSDF, surfaceNets};')();

const STEP = Number(process.argv[2]) || 0.26;          // grid size in cm; smaller = finer, bigger file
const t0 = Date.now();
const m = surfaceNets(castSDF, [-7.2, -0.6, -7.6], [7.2, 34.9, 22.2], STEP);
const nv = m.P.length / 3;
if (nv > 65535) throw new Error(`Too many vertices (${nv}). Use a bigger step, e.g. 0.3`);

const P = Int16Array.from(m.P, v => Math.round(v * 500));
const N = Int8Array.from(m.N, v => Math.round(v * 127));
const I = Uint16Array.from(m.I);
const b64 = a => Buffer.from(a.buffer, a.byteOffset, a.byteLength).toString('base64');
const line = `const MESH={p:"${b64(P)}",n:"${b64(N)}",i:"${b64(I)}"};`;

const start = html.indexOf('const MESH={'), end = html.indexOf('};', start) + 2;
if (start < 0) throw new Error('No "const MESH={" line found in web/index.html');
fs.writeFileSync(file, html.slice(0, start) + line + html.slice(end));
console.log(`Mesh: ${nv} vertices, ${I.length / 3} triangles, ${(line.length / 1024).toFixed(0)} KB, ${Date.now() - t0} ms -> web/index.html`);
