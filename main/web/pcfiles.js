/* DOS files on the web page: files dropped loose, in folders or in a ZIP file
 * are put together on a FAT disk image in the browser, which is uploaded like
 * any other image. A floppy if they fit on one (the smallest of 360 KB,
 * 720 KB, 1.2 MB, 1.44 MB and 2.88 MB that does, FAT12 as DOS's FORMAT makes
 * it), else a hard disk (.hdd: one FAT16 partition, a quarter more room than
 * the files take and 1 MB besides, cylinders of 16 heads x 63 sectors).
 *
 * Names become 8.3 as on the board's folder disks (components/ibmpc/folder.c)
 * and as Windows makes them: upper case, spaces gone, characters DOS doesn't
 * take as "_", and if that changed the name or it's too long, its first six
 * characters and ~1 (~2, ...).
 *
 * The board serves this file as /machine.js; the page loads it when
 * /api/info says "script": true, and it registers itself with
 * registerPacker(). It also runs under node (module.exports) for tests.
 */
(function () {
"use strict";

const put16 = (b, p, v) => { b[p] = v & 255; b[p + 1] = (v >> 8) & 255; };
const put32 = (b, p, v) => { put16(b, p, v & 0xffff); put16(b, p + 2, v >>> 16); };
const SECTOR = 512;

/* ---- Names ---- */

const dosChar = c => /[A-Z0-9!#$%&'()\-@^_`{}~]/.test(c);

/* The 8.3 name (11 characters, padded) for a name: n = 0 as it is (null if
 * it can't be), else with ~n */
function shortName(name, n) {
  let dot = name.lastIndexOf(".");
  if (dot <= 0) dot = -1;
  const baseIn = dot < 0 ? name : name.slice(0, dot), extIn = dot < 0 ? "" : name.slice(dot + 1);
  let base = "", ext = "", lossy = false;
  for (const ch of baseIn) {
    if (ch === " " || ch === ".") { lossy = true; continue; }
    const c = ch.toUpperCase();
    if (dosChar(c) && c.length === 1) base += c; else { base += "_"; lossy = true; }
  }
  for (const ch of extIn) {
    if (ch === " ") { lossy = true; continue; }
    if (ext.length === 3) { lossy = true; break; }
    const c = ch.toUpperCase();
    if (dosChar(c) && c.length === 1) ext += c; else { ext += "_"; lossy = true; }
  }
  if (base.length > 8 || !base.length) lossy = true;
  if (n === 0 && lossy) return null;
  if (n > 0) {
    const tail = "~" + n;
    base = base.slice(0, 8 - tail.length) + tail;
  }
  return base.padEnd(8) + ext.padEnd(3);
}

function dosTime(date) {
  const d = date instanceof Date && !isNaN(date) ? date : new Date();
  if (d.getFullYear() < 1980) return { time: 0, date: (1 << 5) | 1 };
  const y = Math.min(d.getFullYear(), 2107) - 1980;
  return { time: (d.getHours() << 11) | (d.getMinutes() << 5) | (d.getSeconds() >> 1),
           date: (y << 9) | ((d.getMonth() + 1) << 5) | d.getDate() };
}

/* ---- The files as a tree of folders ---- */

function tree(files) {
  const root = { dir: true, children: [], byKey: new Map() };
  for (const f of files) {
    const parts = f.path.split("/").filter(s => s && s !== "." && s !== "..");
    if (!parts.length) continue;
    let d = root;
    for (let i = 0; i < parts.length; i++) {
      const last = i === parts.length - 1, key = parts[i].toLowerCase();
      let c = d.byKey.get(key);
      if (c && (last || !c.dir)) { c = null; break; }        /* (a file and a folder of one name) */
      if (!c) {
        c = last ? { dir: false, name: parts[i], data: f.data, date: f.date }
                 : { dir: true, name: parts[i], children: [], byKey: new Map(), date: f.date };
        d.byKey.set(key, c);
        d.children.push(c);
      }
      d = c;
    }
  }
  const name83 = d => {
    const taken = new Set();
    for (const c of d.children) {
      let s = shortName(c.name, 0);
      for (let n = 1; !s || taken.has(s); n++) s = shortName(c.name, n);
      taken.add(s);
      c.short = s;
      if (c.dir) name83(c);
    }
  };
  name83(root);
  return root;
}

function each(d, fn) {
  for (const c of d.children) {
    fn(c);
    if (c.dir) each(c, fn);
  }
}

/* ---- Formats ---- */

const FLOPPIES = [
  { bytes: 368640, spt: 9, spc: 2, root: 112, media: 0xfd },
  { bytes: 737280, spt: 9, spc: 2, root: 112, media: 0xf9 },
  { bytes: 1228800, spt: 15, spc: 1, root: 224, media: 0xf9 },
  { bytes: 1474560, spt: 18, spc: 1, root: 224, media: 0xf0 },
  { bytes: 2949120, spt: 36, spc: 2, root: 240, media: 0xf0 },
];
const CYLINDER = 16 * 63;

/* A format's sectors per FAT for the clusters left */
function fatSectors(fs) {
  const root = fs.root * 32 / SECTOR;
  let fat = 1;
  for (let i = 0; i < 8; i++) {
    const clusters = Math.floor((fs.sectors - 1 - 2 * fat - root) / fs.spc) + 2;
    const need = Math.ceil((fs.fat16 ? clusters * 2 : Math.ceil(clusters * 3 / 2)) / SECTOR);
    if (need <= fat) break;
    fat = need;
  }
  return fat;
}

function layout(fs) {
  fs.fat = fatSectors(fs);
  fs.rootSector = 1 + 2 * fs.fat;
  fs.dataSector = fs.rootSector + fs.root * 32 / SECTOR;
  fs.clusters = Math.floor((fs.sectors - fs.dataSector) / fs.spc);
  return fs;
}

/* Clusters the folders (but the root) and files take */
function need(root, clusterBytes) {
  let n = 0;
  each(root, c => { n += Math.ceil((c.dir ? (2 + c.children.length) * 32 : c.data.length) / clusterBytes); });
  return n;
}

function chooseFormat(root) {
  for (const f of FLOPPIES) {
    const fs = layout({ sectors: f.bytes / SECTOR, spt: f.spt, heads: 2, spc: f.spc, root: f.root, media: f.media,
                        hidden: 0, fat16: false, drive: 0, bytes: f.bytes });
    if (root.children.length < fs.root && need(root, fs.spc * SECTOR) <= fs.clusters) return fs;
  }
  /* a hard disk: room to spare, at most 65,000 clusters (from 4,200: FAT16) */
  let bytes = 0;
  each(root, c => { bytes += c.dir ? 0 : c.data.length; });
  for (let spc = 1; spc <= 64; spc *= 2) {
    const cb = spc * SECTOR;
    const data = need(root, cb) * cb * 1.25 + 1024 * 1024;
    const clusters = Math.max(4200, Math.ceil(data / cb));
    if (clusters > 65000) continue;
    const fat = Math.ceil((clusters + 2) * 2 / SECTOR);
    const cyls = Math.ceil((63 + 1 + 2 * fat + 32 + clusters * spc) / CYLINDER);
    if (cyls > 1024) break;
    let fs = layout({ sectors: cyls * CYLINDER - 63, spt: 63, heads: 16, spc, root: 512, media: 0xf8,
                      hidden: 63, fat16: true, drive: 0x80, bytes: cyls * CYLINDER * SECTOR, cyls });
    if (fs.clusters > 65000) {
      /* (the partition ends with the 65,000th cluster: DOS counts them from its size) */
      fs.sectors = fs.dataSector + 65000 * spc;
      fs = layout(fs);
    }
    if (root.children.length < fs.root && need(root, cb) <= fs.clusters) return fs;
  }
  throw new Error("too much for a disk image (" + Math.round(bytes / 1048576) + " MB)");
}

/* ---- The image ---- */

const BOOT_CODE = [   /* "not a system disk", a key, INT 19h again (as disk.c's) */
  0xfa, 0x31, 0xc0, 0x8e, 0xd0, 0xbc, 0x00, 0x7c, 0xfb, 0x8e, 0xd8, 0xbe, 0x5d, 0x7c,
  0xac, 0x08, 0xc0, 0x74, 0x06, 0xb4, 0x0e, 0xcd, 0x10, 0xeb, 0xf5,
  0x30, 0xe4, 0xcd, 0x16, 0xcd, 0x19,
];
const MBR_CODE = [    /* the active partition's boot sector to 7C00 (as disk.c's) */
  0xfa, 0x31, 0xc0, 0x8e, 0xd0, 0xbc, 0x00, 0x7c, 0x8e, 0xd8, 0x8e, 0xc0, 0xfb,
  0xbe, 0x00, 0x7c, 0xbf, 0x00, 0x06, 0xb9, 0x00, 0x01, 0xfc, 0xf3, 0xa5,
  0xea, 0x1e, 0x06, 0x00, 0x00,
  0xbe, 0xbe, 0x07, 0xb9, 0x04, 0x00,
  0x80, 0x3c, 0x80, 0x74, 0x0b, 0x83, 0xc6, 0x10, 0xe2, 0xf6,
  0xcd, 0x18, 0xeb, 0xfe, 0x90, 0x90,
  0x8a, 0x74, 0x01, 0x8b, 0x4c, 0x02, 0xb2, 0x80, 0xbb, 0x00, 0x7c, 0xb8, 0x01, 0x02, 0xcd, 0x13,
  0x72, 0xe8, 0x81, 0x3e, 0xfe, 0x7d, 0x55, 0xaa, 0x75, 0xe0,
  0xea, 0x00, 0x7c, 0x00, 0x00,
];
const ascii = (b, p, s) => { for (let i = 0; i < s.length; i++) b[p + i] = s.charCodeAt(i) & 255; };

function bootSector(b, p, fs, label) {
  b.set([0xeb, 0x3c, 0x90], p);
  ascii(b, p + 3, "MSDOS5.0");
  put16(b, p + 11, SECTOR);
  b[p + 13] = fs.spc;
  put16(b, p + 14, 1);
  b[p + 16] = 2;
  put16(b, p + 17, fs.root);
  put16(b, p + 19, fs.sectors < 65536 ? fs.sectors : 0);
  b[p + 21] = fs.media;
  put16(b, p + 22, fs.fat);
  put16(b, p + 24, fs.spt);
  put16(b, p + 26, fs.heads);
  put32(b, p + 28, fs.hidden);
  put32(b, p + 32, fs.sectors < 65536 ? 0 : fs.sectors);
  b[p + 36] = fs.drive;
  b[p + 38] = 0x29;
  put32(b, p + 39, (Math.random() * 0x100000000) >>> 0);
  ascii(b, p + 43, label);
  ascii(b, p + 54, fs.fat16 ? "FAT16   " : "FAT12   ");
  b.set(BOOT_CODE, p + 0x3e);
  ascii(b, p + 0x5d, "Not a system disk: press a key to try again.\r\n");
  b[p + 510] = 0x55;
  b[p + 511] = 0xaa;
}

function mbr(b, fs) {
  b.set(MBR_CODE, 0);
  const e = 0x1be, end = 63 + fs.sectors - 1;      /* its last sector: cylinder, head, sector */
  let c = Math.floor(end / CYLINDER), h = Math.floor(end / 63) % 16, s = end % 63 + 1;
  if (c > 1023) { c = 1023; h = 15; s = 63; }
  b[e] = 0x80;
  b[e + 1] = 1;
  b[e + 2] = 1;
  b[e + 4] = fs.sectors < 65536 ? 0x04 : 0x06;
  b[e + 5] = h;
  b[e + 6] = s | ((c >> 8) << 6);
  b[e + 7] = c & 255;
  put32(b, e + 8, 63);
  put32(b, e + 12, fs.sectors);
  b[510] = 0x55;
  b[511] = 0xaa;
}

function entry(b, p, name, attr, date, first, size) {
  ascii(b, p, name);
  if (b[p] === 0xe5) b[p] = 0x05;
  b[p + 11] = attr;
  const t = dosTime(date);
  put16(b, p + 22, t.time);
  put16(b, p + 24, t.date);
  put16(b, p + 26, first);
  put32(b, p + 28, size);
}

function labelOf(name) {
  let s = "";
  for (const ch of name.toUpperCase()) {
    if (ch === ".") continue;
    s += dosChar(ch) || ch === " " ? ch : "_";
  }
  return (s.slice(0, 11) || "NO NAME").padEnd(11);
}

function build(name, root, fs) {
  const img = new Uint8Array(fs.bytes);
  const base = fs.hidden * SECTOR, cb = fs.spc * SECTOR;
  if (fs.hidden) mbr(img, fs);
  const label = labelOf(name);
  bootSector(img, base, fs, label);
  /* clusters, in order */
  let next = 2;
  each(root, c => {
    const n = Math.ceil((c.dir ? (2 + c.children.length) * 32 : c.data.length) / cb);
    c.first = n ? next : 0;
    c.count = n;
    next += n;
  });
  const fat = new Uint8Array(fs.fat * SECTOR);
  const setFat = (c, v) => {
    if (fs.fat16) { put16(fat, c * 2, v); return; }
    const p = Math.floor(c * 3 / 2);
    if (c & 1) { fat[p] = (fat[p] & 0x0f) | ((v << 4) & 0xf0); fat[p + 1] = v >> 4; }
    else { fat[p] = v & 255; fat[p + 1] = (fat[p + 1] & 0xf0) | ((v >> 8) & 0x0f); }
  };
  const end = fs.fat16 ? 0xffff : 0xfff;
  setFat(0, (fs.fat16 ? 0xff00 : 0xf00) | fs.media);
  setFat(1, end);
  const at = c => base + (fs.dataSector + (c - 2) * fs.spc) * SECTOR;
  const entries = (d, p) => {
    for (const c of d.children) {
      entry(img, p, c.short, c.dir ? 0x10 : 0x20, c.date, c.first, c.dir ? 0 : c.data.length);
      p += 32;
    }
  };
  each(root, c => {
    for (let k = 0; k < c.count; k++) setFat(c.first + k, k + 1 < c.count ? c.first + k + 1 : end);
    if (!c.dir) {
      img.set(c.data, at(c.first));
      return;
    }
    const p = at(c.first);
    entry(img, p, ".          ", 0x10, c.date, c.first, 0);
    entry(img, p + 32, "..         ", 0x10, c.date, c.parent ? c.parent.first : 0, 0);
    entries(c, p + 64);
  });
  for (let i = 0; i < 2; i++) img.set(fat, base + (1 + i * fs.fat) * SECTOR);
  const rootAt = base + fs.rootSector * SECTOR;
  entry(img, rootAt, label, 0x08, new Date(), 0, 0);
  entries(root, rootAt + 32);
  return img;
}

/* A safe file name for the board: "<name>.img" or ".hdd" */
function imageName(name, hd) {
  const s = name.replace(/[\x00-\x1f/\\:*?"<>|]/g, "_").replace(/^\.+/, "").slice(0, 80) || "FILES";
  return s + (hd ? ".hdd" : ".img");
}

function pack(name, files) {
  const root = tree(files);
  const parents = d => { for (const c of d.children) if (c.dir) { c.parent = d === root ? null : d; parents(c); } };
  parents(root);
  if (!root.children.length) throw new Error("no files");
  const fs = chooseFormat(root);
  return [{ name: imageName(name, fs.hidden !== 0), data: build(name, root, fs) }];
}

const packer = { description: "DOS files and folders", pack: (name, files) => pack(name, files) };

if (typeof window !== "undefined" && window.registerPacker) window.registerPacker(packer);
if (typeof module !== "undefined") module.exports = { ...packer, shortName, tree };
})();
