// Runs inside EasyEDA Pro (via the jlceda MCP pcb_execute_code tool).
// Replaces placed footprints with LCSC system-library devices, keeping position, nets and routing.
// MODE = 'plan' only measures fits; MODE = 'apply' commits the best fit for each part.
const MODE = 'plan';
const PARTS = {"J1": "C165948", "U2": "C84681", "D1": "C8598", "R4": "C23186", "R5": "C23186", "C1": "C19702", "C2": "C14663", "U3": "C6186", "C10": "C19702", "C3": "C14663", "Q2": "C20526", "Q3": "C20526", "R7": "C25804", "C5": "C14663", "C4": "C19702", "R1": "C22859", "Q1": "C10487", "R2": "C21190", "R3": "C25804", "U1": "C82899", "C6": "C19702", "C7": "C14663", "R8": "C25804", "C9": "C15849", "SW1": "C318884", "R9": "C25804", "SW2": "C318884", "FPC1": "C6364659"}; // { ref: lcscId }
const TOL = 12; // mil: max pad-centre distance accepted without re-routing

const devCache = {};
async function device(lcsc) {
  if (!devCache[lcsc]) {
    const r = await eda.lib_Device.search(lcsc);
    devCache[lcsc] = (r || []).find(d => d.supplierId === lcsc) || null;
  }
  return devCache[lcsc];
}
async function pins(id) {
  return (await eda.pcb_PrimitiveComponent.getAllPinsByPrimitiveId(id)).map(p => ({ p, num: p.padNumber, x: p.x, y: p.y, net: p.net, hole: !!p.hole }));
}
function nearest(list, x, y) {
  let best = null, bd = 1e18;
  for (const o of list) { const d = Math.hypot(o.x - x, o.y - y); if (d < bd) { bd = d; best = o; } }
  return [best, bd];
}
// translation that maps new pads onto old pads: start with the numbered-pad median, refine by nearest matching
function fit(oldPins, newPins) {
  const byNum = {};
  for (const o of oldPins) (byNum[o.num] = byNum[o.num] || []).push(o);
  let dx = [], dy = [];
  for (const n of newPins) if (byNum[n.num]) { const [o] = nearest(byNum[n.num], n.x, n.y); dx.push(o.x - n.x); dy.push(o.y - n.y); }
  const med = a => { const s = [...a].sort((p, q) => p - q); return s.length ? s[s.length >> 1] : 0; };
  let tx = med(dx), ty = med(dy);
  for (let it = 0; it < 3; it++) {
    dx = []; dy = [];
    for (const n of newPins) { const [o, d] = nearest(oldPins, n.x + tx, n.y + ty); if (d < 40) { dx.push(o.x - n.x); dy.push(o.y - n.y); } }
    if (!dx.length) break;
    tx = med(dx); ty = med(dy);
  }
  let err = 0, numOk = 0, matched = 0;
  const used = new Set();
  for (const n of newPins) {
    const [o, d] = nearest(oldPins, n.x + tx, n.y + ty);
    if (d < 40) { matched++; used.add(o); err = Math.max(err, d); if (o.num === n.num) numOk++; }
  }
  const unmatchedOld = oldPins.filter(o => !used.has(o) && o.net).length;
  return { tx, ty, err, numOk, matched, unmatchedOld };
}

const comps = await eda.pcb_PrimitiveComponent.getAll();
const report = [];
for (const [ref, lcsc] of Object.entries(PARTS)) {
  const old = comps.find(c => c.designator === ref);
  const dev = await device(lcsc);
  if (!old || !dev) { report.push({ ref, lcsc, error: !old ? 'no component' : 'no device' }); continue; }
  const oldPins = await pins(old.primitiveId);
  const trials = [];
  for (const rot of [0, 90, 180, 270]) {
    const c = await eda.pcb_PrimitiveComponent.create({ libraryUuid: dev.libraryUuid, uuid: dev.uuid }, old.layer, old.x, old.y, (old.rotation + rot) % 360, false);
    const f = fit(oldPins, await pins(c.primitiveId));
    trials.push({ rot: (old.rotation + rot) % 360, id: c.primitiveId, ...f });
  }
  // fewest unmatched connected old pads, then best numbering agreement, then smallest error
  trials.sort((a, b) => a.unmatchedOld - b.unmatchedOld || b.numOk - a.numOk || a.err - b.err);
  const best = trials[0];
  for (const t of trials) if (MODE !== 'apply' || t !== best) await eda.pcb_PrimitiveComponent.delete(t.id);
  const row = { ref, lcsc, name: dev.name, fp: dev.footprintName, rot: best.rot, tx: +best.tx.toFixed(1), ty: +best.ty.toFixed(1),
                err: +best.err.toFixed(1), numOk: best.numOk, newPads: null, oldPads: oldPins.length, unmatchedOld: best.unmatchedOld };
  if (MODE === 'apply') {
    if (best.err > TOL || best.unmatchedOld) { await eda.pcb_PrimitiveComponent.delete(best.id); row.skipped = true; report.push(row); continue; }
    await eda.pcb_PrimitiveComponent.modify(best.id, { x: old.x + best.tx, y: old.y + best.ty });
    const np = await pins(best.id);
    for (const n of np) {
      const [o, d] = nearest(oldPins, n.x, n.y);
      if (o && d < 40 && o.net) { n.p.setState_Net(o.net); await n.p.done(); }
    }
    await eda.pcb_PrimitiveComponent.delete(old.primitiveId);
    await eda.pcb_PrimitiveComponent.modify(best.id, { designator: ref });
    row.newPads = np.length;
    row.applied = true;
  }
  report.push(row);
}
return report;
