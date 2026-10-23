// Long lists on slow handhelds: a fixed pool of row slots, rows measured on
// demand, and one per-frame work budget for both. A row whose content is not
// ready shows its skeleton for a frame or more instead of holding the frame:
// scrolling stays at the frame rate and the rows fill in behind it.
//
//   const list = createMeasuredList({ items, measure, estimate, offset,
//     view: VIEW_H, overscan: 80, rebase });
//   const pool = createSlotPool(list.rows, offset, { view: VIEW_H, overscan: 80, minRowH: 40 });
//   pool.attach(slot, (row, skeleton) => (skeleton ? showSkeleton(row) : paint(row)));
//
// Measuring (wrapping a post, laying out a comment) and painting a slot (a
// dozen text writes) each cost QuickJS milliseconds on an Old 3DS. Both run
// under frameBudget(): the first piece of work in a frame always runs, and
// more run while the frame's list work stays under FRAME_BUDGET_MS. Visible
// rows go first, then the overscan, then the rows ahead of the motion; the
// rest of the list is measured in frames where the list stands still.
//
// A row that is not measured is placed at its estimated height. When its
// measured height replaces the estimate above the row the reader is looking
// at, `rebase` moves the scroll offset by the difference (Scroller.rebase),
// so what is on screen stays where it is.

import { createSignal, type Accessor } from "solid-js";
import { virtualFrame } from "./clock.ts";
import { onFrame } from "./frame.ts";

// ---------------------------------------------------------------------------
// the frame budget
// ---------------------------------------------------------------------------

/** Milliseconds of list work (measuring, painting) per frame on the shared
 *  budget. */
export const FRAME_BUDGET_MS = 4;

export interface FrameBudget {
  /** Work may start: none ran yet this frame, or what ran stayed under the
   *  budget. */
  left(): boolean;
  /** Work no reader waits for (rows away from every view) may start: the
   *  budget is left, and no list left rows in its view undone last frame. */
  idle(): boolean;
  /** A list leaves rows in its view undone this frame. */
  owe(): void;
  /** Run `work`, charging its time to this frame. */
  spend<T>(work: () => T): T;
}

/** A budget of `ms` per frame, reset when the virtual frame advances. A list
 *  hidden under another page keeps measuring its rest while it stands
 *  still; the owed rows of the page on top go first. */
export function createFrameBudget(ms: number): FrameBudget {
  let frame = Number.NaN;
  let used = 0;
  let owed = false;
  let owedBefore = false;
  const roll = () => {
    const now = virtualFrame();
    if (now !== frame) {
      owedBefore = now === frame + 1 && owed;
      frame = now;
      used = 0;
      owed = false;
    }
  };
  return {
    left() {
      roll();
      return used < ms;
    },
    idle() {
      roll();
      return used < ms && !owedBefore && !owed;
    },
    owe() {
      roll();
      owed = true;
    },
    spend(work) {
      roll();
      const start = Date.now();
      try {
        return work();
      } finally {
        used += Date.now() - start;
      }
    },
  };
}

let shared: FrameBudget | null = null;

/** The budget every list shares unless it is given its own: two lists on
 *  screen split one frame's allowance. */
export function frameBudget(): FrameBudget {
  return (shared ??= createFrameBudget(FRAME_BUDGET_MS));
}

// ---------------------------------------------------------------------------
// the slot pool
// ---------------------------------------------------------------------------

export interface PlacedRow {
  /** Top in content px. Rows are in order of y. */
  y: number;
  h: number;
  /** The content is not ready: the slot shows the row's skeleton. */
  pending?: boolean;
  /** What the slot draws besides the row's place. A row whose content is
   *  the one the slot last painted in full is only moved (the painter's
   *  `moved`), which costs nothing against the budget. */
  content?: unknown;
}

export interface SlotOptions {
  /** Visible content height. */
  view: number;
  /** Content px kept filled above and below the visible band. */
  overscan: number;
  /** The shortest row; sizes the pool so every band row has a slot. */
  minRowH: number;
  budget?: FrameBudget;
}

/** Paint `row` into a slot. With `skeleton`, only move the slot to the
 *  row and show its placeholder: the row is pending, or this frame's budget
 *  is spent and the full paint comes in a later frame. With `moved`, the
 *  slot shows this row's content already: only its place changed. */
export type SlotPainter<R> = (row: R | null, skeleton: boolean, moved: boolean) => void;

export interface SlotPool<R> {
  /** Slot indices to mount, one component each. */
  slots: readonly number[];
  /** A mounted slot's painter: called with its current row now and with
   *  every different row (or null) after. */
  attach(slot: number, paint: SlotPainter<R>): void;
}

/** Slot k shows the band row whose index is k modulo the pool size, so a row
 *  entering the band repaints one slot and the others keep theirs
 *  (UITableView cell reuse). The band is placed once per frame, outside the
 *  reactive graph: a memo on the scroll offset would rerun on every scrolled
 *  frame. */
export function createSlotPool<R extends PlacedRow>(
  rows: () => readonly R[],
  offset: () => number,
  options: SlotOptions,
): SlotPool<R> {
  const budget = options.budget ?? frameBudget();
  const size = Math.ceil((options.view + options.overscan * 2) / options.minRowH) + 1;
  const shown: (R | null)[] = new Array(size).fill(null);
  /** The slot shows its row's skeleton and owes it a full paint. */
  const owed: boolean[] = new Array(size).fill(false);
  /** The content of the slot's last full paint. */
  const drawn: unknown[] = new Array(size).fill(undefined);
  const painters: (SlotPainter<R> | undefined)[] = new Array(size);
  let placedRows: readonly R[] | null = null;
  let placedFirst = -1;
  let placedEnd = -1;
  let owing = 0;

  const paint = (slot: number, row: R | null) => {
    const painter = painters[slot];
    if (row && !row.pending && !owed[slot] && row.content !== undefined && row.content === drawn[slot]) {
      painter?.(row, false, true);
      return;
    }
    const full = row !== null && !row.pending && budget.left();
    if (owed[slot]) owing -= 1;
    owed[slot] = row !== null && !row.pending && !full;
    if (owed[slot]) owing += 1;
    drawn[slot] = full && painter ? row!.content : undefined;
    if (!painter) return;
    if (full) budget.spend(() => painter(row, false, false));
    else painter(row, row !== null, false);
  };

  /** Settle what earlier frames owed, the rows in view first. */
  const payOwed = () => {
    const top = offset();
    const bottom = top + options.view;
    for (const visible of [true, false]) {
      for (let slot = 0; slot < size && owing > 0; slot++) {
        const row = shown[slot];
        if (!owed[slot] || !row) continue;
        if ((row.y + row.h > top && row.y < bottom) !== visible) continue;
        if (!budget.left()) return;
        owed[slot] = false;
        owing -= 1;
        const painter = painters[slot];
        drawn[slot] = painter ? row.content : undefined;
        if (painter) budget.spend(() => painter(row, false, false));
      }
    }
  };

  const place = () => {
    const list = rows();
    const top = offset() - options.overscan;
    const bottom = offset() + options.view + options.overscan;
    let first = 0;
    let hi = list.length;
    while (first < hi) {
      const mid = (first + hi) >> 1;
      const row = list[mid]!;
      if (row.y + row.h > top) hi = mid;
      else first = mid + 1;
    }
    let end = first;
    while (end < list.length && end - first < size && list[end]!.y < bottom) end++;
    const sameList = list === placedRows;
    if (!sameList || first !== placedFirst || end !== placedEnd) {
      placedRows = list;
      placedFirst = first;
      placedEnd = end;
      for (let slot = 0; slot < size; slot++) {
        const index = first + ((slot - (first % size) + size) % size);
        const row = index < end ? list[index]! : null;
        if (row === shown[slot]) continue;
        // A row that left the band stays painted where it is, past the
        // overscan and out of sight, until a row entering the band takes its
        // slot: one repaint per row instead of a clear and a repaint.
        if (row === null && sameList) continue;
        shown[slot] = row;
        paint(slot, row);
      }
    }
    if (owing > 0) payOwed();
    if (owing > 0) budget.owe();
  };
  place();
  onFrame(place);
  return {
    slots: Array.from({ length: size }, (_, slot) => slot),
    attach(slot, painter) {
      painters[slot] = painter;
      const row = shown[slot] ?? null;
      if (owed[slot]) {
        owed[slot] = false;
        owing -= 1;
      }
      paint(slot, row);
    },
  };
}

// ---------------------------------------------------------------------------
// the measured list
// ---------------------------------------------------------------------------

export interface MeasuredRow<T, V> extends PlacedRow {
  readonly item: T;
  readonly index: number;
  /** The measurement: a row is only moved while it keeps it. */
  readonly content?: V;
  /** The measurement, or null while the row is pending at its estimated
   *  height. A stale measurement (its version changed) stays until the new
   *  one is made. */
  readonly value: V | null;
}

export interface MeasuredListOptions<T, V extends { h: number }> {
  items: () => readonly T[];
  /** The expensive part: the row's layout, at least its height. */
  measure: (item: T, index: number) => V;
  /** A cheap height for a row not measured yet. */
  estimate: (item: T, index: number) => number;
  /** A row is measured again when this changes (compared with ===); the old
   *  measurement is drawn meanwhile. */
  version?: (item: T, index: number) => unknown;
  /** What a measurement is kept by. Default: the item itself. */
  key?: (item: T, index: number) => unknown;
  offset: () => number;
  view: number;
  overscan: number;
  /** Content px above the first row. */
  top?: () => number;
  /** Move the scroll offset by `delta` px without moving the content on
   *  screen (Scroller.rebase): rows above the reader changed height. */
  rebase?: (delta: number) => void;
  budget?: FrameBudget;
}

export interface MeasuredList<T, V> {
  rows: Accessor<readonly MeasuredRow<T, V>[]>;
  /** The content height, estimates included. */
  height: Accessor<number>;
  /** The last scan of the whole list found every row measured for its
   *  current version. */
  complete: Accessor<boolean>;
  /** Scan the whole list again for rows whose version changed (the list
   *  stops scanning once a full pass finds nothing to do). */
  recheck(): void;
  /** Forget every measurement (the width or the fonts changed). */
  invalidate(): void;
}

/** Frames the offset holds still before the rest of the list is measured. */
const STILL_FRAMES = 6;
/** Rows a still frame checks for a missing or stale measurement. */
const SCAN_PER_FRAME = 32;
/** Frames measurements made out of view may wait to be published together. */
const PUBLISH_EVERY = 8;

interface Measurement<V> {
  value: V;
  version: unknown;
}

export function createMeasuredList<T, V extends { h: number }>(options: MeasuredListOptions<T, V>): MeasuredList<T, V> {
  const budget = options.budget ?? frameBudget();
  const keyOf = options.key ?? ((item: T) => item as unknown);
  const versionOf = options.version ?? (() => undefined);
  let measured = new Map<unknown, Measurement<V>>();
  const [rows, setRows] = createSignal<readonly MeasuredRow<T, V>[]>([]);
  const [height, setHeight] = createSignal(0);
  const [complete, setComplete] = createSignal(true);

  let items: readonly T[] = [];
  let top = Number.NaN;
  let laid: readonly MeasuredRow<T, V>[] = [];
  /** The range of rows measured since the last layout. */
  let dirtyFrom = Number.POSITIVE_INFINITY;
  let dirtyTo = -1;
  let lastOffset = Number.NaN;
  let still = 0;
  let scan = 0;
  /** Rows checked since the background scan last found work. */
  let quiet = 0;
  let unpublished = 0;

  const needs = (item: T, index: number): boolean => {
    const entry = measured.get(keyOf(item, index));
    return !entry || entry.version !== versionOf(item, index);
  };

  /** Place the rows from the measurements and publish them if any moved.
   *  `full` after the items, the top or every measurement changed: rows are
   *  matched by key. Otherwise the items are the same and the rows before
   *  the first measured one are kept as they are, and so are the rows after
   *  the last measured one once they fall where they were. */
  const layout = (full: boolean) => {
    top = options.top?.() ?? 0;
    const from = full ? 0 : Math.min(dirtyFrom, laid.length);
    const next: MeasuredRow<T, V>[] = full ? [] : laid.slice(0, from);
    let y = from > 0 ? laid[from - 1]!.y + laid[from - 1]!.h : top;
    let byKey: Map<unknown, MeasuredRow<T, V>> | null = null;
    if (full) {
      byKey = new Map();
      for (const row of laid) byKey.set(keyOf(row.item, row.index), row);
    }
    let changed = items.length !== laid.length;
    for (let index = from; index < items.length; index++) {
      const item = items[index]!;
      const key = keyOf(item, index);
      const before = byKey ? byKey.get(key) : laid[index];
      if (!byKey && index > dirtyTo && before && before.y === y) {
        for (let rest = index; rest < laid.length; rest++) next.push(laid[rest]!);
        const last = laid[laid.length - 1]!;
        y = last.y + last.h;
        break;
      }
      const entry = measured.get(key);
      const value = entry ? entry.value : null;
      const h = value ? value.h : options.estimate(item, index);
      const row =
        before && before.item === item && before.index === index && before.y === y && before.h === h && before.value === value
          ? before
          : { item, index, y, h, value, pending: value === null, content: value ?? undefined };
      if (laid[index] !== row) changed = true;
      next.push(row);
      y += h;
    }
    dirtyFrom = Number.POSITIVE_INFINITY;
    dirtyTo = -1;
    if (changed) {
      laid = next;
      setRows(next);
    }
    setHeight(y);
    unpublished = 0;
  };

  const rescan = () => {
    quiet = 0;
    setComplete(false);
  };

  /** Lay out again, keeping the row at the offset where it was on screen:
   *  rows above it that changed height (or were added) move the offset. */
  const relayout = (at: number, full: boolean) => {
    const index = rowAt(at);
    const anchor = laid[index];
    layout(full);
    if (!anchor || !options.rebase) return;
    let moved: MeasuredRow<T, V> | undefined;
    if (!full) moved = laid[index];
    else {
      const key = keyOf(anchor.item, anchor.index);
      moved = laid.find((row) => keyOf(row.item, row.index) === key);
    }
    if (moved && moved.y !== anchor.y) options.rebase(moved.y - anchor.y);
  };

  /** The index of the row at content y (the last row past the end). */
  const rowAt = (y: number): number => {
    let lo = 0;
    let hi = laid.length;
    while (lo < hi) {
      const mid = (lo + hi) >> 1;
      const row = laid[mid]!;
      if (row.y + row.h > y) hi = mid;
      else lo = mid + 1;
    }
    return lo;
  };

  /** Measure `index` if it needs it and the budget allows; false when the
   *  budget is spent. Only a row near the view with no measurement at all
   *  is urgent: a stale one is drawn from its old measurement meanwhile,
   *  and is measured again in an idle budget like the rows away from view. */
  let touchedNear = false;
  const visit = (index: number, near: boolean): boolean => {
    if (index < 0 || index >= items.length) return true;
    const item = items[index]!;
    if (!needs(item, index)) return true;
    const urgent = near && !measured.has(keyOf(item, index));
    if (urgent ? !budget.left() : !budget.idle()) {
      if (urgent) budget.owe();
      // A stale row near the view waits; the scan goes on to the rows there
      // with no measurement.
      return near && !urgent;
    }
    const value = budget.spend(() => options.measure(item, index));
    measured.set(keyOf(item, index), { value, version: versionOf(item, index) });
    if (index < dirtyFrom) dirtyFrom = index;
    if (index > dirtyTo) dirtyTo = index;
    if (near) touchedNear = true;
    unpublished += 1;
    return true;
  };

  /** Frames that measured rows out of view without publishing them. */
  let waiting = 0;
  const step = () => {
    const at = options.offset();
    const list = options.items();
    if (list !== items) {
      items = list;
      // Keep only the measurements of rows still in the list.
      const kept = new Map<unknown, Measurement<V>>();
      for (let index = 0; index < items.length; index++) {
        const key = keyOf(items[index]!, index);
        const entry = measured.get(key);
        if (entry) kept.set(key, entry);
      }
      measured = kept;
      rescan();
      relayout(at, true);
    } else if ((options.top?.() ?? 0) !== top) {
      relayout(at, true);
    }
    const moving = at !== lastOffset;
    const direction = at > lastOffset ? 1 : -1;
    still = moving ? 0 : still + 1;
    lastOffset = at;

    touchedNear = false;
    const firstVisible = rowAt(at);
    const endVisible = rowAt(at + options.view) + 1;
    const firstNear = rowAt(at - options.overscan);
    const endNear = rowAt(at + options.view + options.overscan) + 1;
    let open = true;
    for (let index = firstVisible; open && index < endVisible; index++) open = visit(index, true);
    for (let index = endVisible; open && index < endNear; index++) open = visit(index, true);
    for (let index = firstVisible - 1; open && index >= firstNear; index--) open = visit(index, true);
    if (open && moving) {
      // A view's worth ahead of the motion.
      const from = direction > 0 ? endNear : firstNear - 1;
      const to = direction > 0 ? rowAt(at + 2 * options.view + options.overscan) + 1 : rowAt(at - options.view - options.overscan) - 1;
      for (let index = from; open && (direction > 0 ? index < to : index > to); index += direction) open = visit(index, false);
    }
    if (open && still >= STILL_FRAMES && quiet < items.length) {
      for (let checked = 0; open && checked < SCAN_PER_FRAME && quiet < items.length; checked++) {
        if (scan >= items.length) scan = 0;
        const before = unpublished;
        open = visit(scan, false);
        if (!open) break;
        quiet = unpublished === before ? quiet + 1 : 0;
        scan += 1;
      }
      if (quiet >= items.length) setComplete(true);
    }

    // Rows measured in view publish at once; rows measured ahead of the
    // motion on the next moving frame, before they come into view; rows
    // measured while the list stands still every PUBLISH_EVERY frames.
    if (unpublished > 0) waiting += 1;
    if (unpublished > 0 && (touchedNear || moving || waiting >= PUBLISH_EVERY)) {
      waiting = 0;
      relayout(at, false);
    }
  };

  step();
  onFrame(step);
  return {
    rows,
    height,
    complete,
    recheck: rescan,
    invalidate() {
      measured = new Map();
      rescan();
      relayout(options.offset(), true);
    },
  };
}
