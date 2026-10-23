// tests/slot-list.test.ts — framework/src/slot-list.ts: rows measured on
// demand under a per-frame budget (visible first, the rest while the list
// stands still), estimates replaced without moving the row on screen, and a
// slot pool that paints skeletons when a frame's budget is spent.

import { beforeEach, describe, expect, test } from "bun:test";
import { createRoot, createSignal } from "solid-js";
import { __advanceClock, resetClock } from "../framework/src/clock.ts";
import { resetFrameHooks, runFrameHooks } from "../framework/src/frame.ts";
import { createMeasuredList, createSlotPool, type FrameBudget, type MeasuredRow } from "../framework/src/slot-list.ts";

if (Bun.resolveSync("solid-js", import.meta.dir).endsWith("server.js")) throw new Error("Slot list tests require --conditions=browser");

/** A budget of `count` pieces of work per frame. */
function countBudget(count: number): FrameBudget {
  let frame = -1;
  let used = 0;
  let clock = 0;
  return {
    left() {
      if (frame !== clock) {
        frame = clock;
        used = 0;
      }
      return used < count;
    },
    busy: false,
    idle() {
      return !this.busy && this.left();
    },
    owe() {},
    spend(work) {
      used += 1;
      return work();
    },
    // The test advances this budget's frame with the host's.
    get tick() {
      return () => (clock += 1);
    },
  } as FrameBudget & { tick: () => void; busy: boolean };
}

function frame(budget: FrameBudget): void {
  (budget as unknown as { tick: () => void }).tick();
  __advanceClock();
  runFrameHooks(0);
}

interface Item {
  id: number;
  lines: number;
}

const items = (count: number): Item[] => Array.from({ length: count }, (_, id) => ({ id, lines: 1 + (id % 3) }));

beforeEach(() => {
  resetClock();
  resetFrameHooks();
});

describe("createMeasuredList", () => {
  test("measures the rows in view first, then the rest while the list stands still", () => {
    createRoot((dispose) => {
      const budget = countBudget(2);
      const [offset] = createSignal(0);
      const measured: number[] = [];
      const all = items(40);
      const list = createMeasuredList({
        items: () => all,
        measure: (item: Item) => (measured.push(item.id), { h: 20 * item.lines }),
        estimate: () => 20,
        offset,
        view: 100,
        overscan: 0,
        budget,
      });
      // Created during setup: the first two rows are measured at once.
      expect(measured).toEqual([0, 1]);
      expect(list.rows().slice(0, 3).map((row) => row.pending)).toEqual([false, false, true]);
      for (let n = 0; n < 4; n++) frame(budget);
      expect(list.rows().filter((row) => row.y < 100).every((row) => !row.pending)).toBe(true);
      expect(list.complete()).toBe(false);
      for (let n = 0; n < 40; n++) frame(budget);
      expect(list.complete()).toBe(true);
      expect(list.height()).toBe(items(40).reduce((sum, item) => sum + 20 * item.lines, 0));
      expect(new Set(measured).size).toBe(40);
      dispose();
    });
  });

  test("keeps the row at the offset in place when rows above it are measured", () => {
    createRoot((dispose) => {
      const budget = countBudget(1);
      const [offset, setOffset] = createSignal(400);
      const rebases: number[] = [];
      const all = items(60);
      const list = createMeasuredList({
        items: () => all,
        measure: (item: Item) => ({ h: 20 * item.lines }),
        estimate: () => 20,
        offset,
        view: 100,
        overscan: 0,
        budget,
        rebase: (delta) => {
          rebases.push(delta);
          setOffset(offset() + delta);
        },
      });
      const anchorOf = () => list.rows().find((row) => row.y + row.h > offset())!;
      for (let n = 0; n < 6; n++) frame(budget);
      const anchor = anchorOf();
      const screenY = anchor.y - offset();
      for (let n = 0; n < 200; n++) frame(budget);
      expect(list.complete()).toBe(true);
      const after = list.rows().find((row) => row.item === anchor.item)!;
      expect(after.y - offset()).toBe(screenY);
      expect(rebases.length).toBeGreaterThan(0);
      dispose();
    });
  });

  test("a version change keeps the old measurement drawn until the new one is made", () => {
    createRoot((dispose) => {
      const budget = countBudget(8);
      const [face, setFace] = createSignal(0);
      const all = items(3);
      const list = createMeasuredList({
        items: () => all,
        measure: (item: Item) => ({ h: 20 * item.lines + face() }),
        estimate: () => 20,
        version: () => face(),
        offset: () => 0,
        view: 200,
        overscan: 0,
        budget,
      });
      expect(list.rows().map((row) => row.h)).toEqual([20, 40, 60]);
      setFace(5);
      expect(list.rows().every((row) => !row.pending)).toBe(true);
      frame(budget);
      expect(list.rows().map((row) => row.h)).toEqual([25, 45, 65]);
      // A stale row in view waits for a frame with no rows owed anywhere.
      setFace(9);
      (budget as unknown as { busy: boolean }).busy = true;
      frame(budget);
      expect(list.rows().map((row) => row.h)).toEqual([25, 45, 65]);
      (budget as unknown as { busy: boolean }).busy = false;
      frame(budget);
      expect(list.rows().map((row) => row.h)).toEqual([29, 49, 69]);
      dispose();
    });
  });

  test("rows added above the reader keep the row at the offset in place", () => {
    createRoot((dispose) => {
      const budget = countBudget(100);
      const later = items(10);
      const [list, setList] = createSignal<readonly Item[]>(later);
      const [offset, setOffset] = createSignal(100);
      const measured = createMeasuredList({
        items: list,
        key: (item: Item) => item.id,
        measure: (item: Item) => ({ h: 20 * item.lines }),
        estimate: () => 20,
        offset,
        view: 100,
        overscan: 0,
        budget,
        rebase: (delta) => setOffset(offset() + delta),
      });
      for (let n = 0; n < 20; n++) frame(budget);
      const anchor = measured.rows().find((row) => row.y + row.h > offset())!;
      const screenY = anchor.y - offset();
      const earlier = Array.from({ length: 5 }, (_, n) => ({ id: 100 + n, lines: 2 }));
      setList([...earlier, ...later]);
      for (let n = 0; n < 20; n++) frame(budget);
      const after = measured.rows().find((row) => row.item.id === anchor.item.id)!;
      expect(after.y - offset()).toBe(screenY);
      expect(after.y).toBeGreaterThan(anchor.y);
      dispose();
    });
  });

  test("unchanged rows keep their objects across layouts", () => {
    createRoot((dispose) => {
      const budget = countBudget(100);
      const [list, setList] = createSignal(items(5));
      const measured = createMeasuredList({ items: list, measure: (item: Item) => ({ h: 10 * item.lines }), estimate: () => 10, offset: () => 0, view: 200, overscan: 0, budget });
      const before = measured.rows();
      setList([...list(), { id: 5, lines: 1 }]);
      frame(budget);
      const after = measured.rows();
      expect(after).toHaveLength(6);
      for (let index = 0; index < 5; index++) expect(after[index]).toBe(before[index]);
      dispose();
    });
  });
});

describe("createSlotPool", () => {
  test("paints skeletons when the frame's budget is spent, and the owed rows in later frames", () => {
    createRoot((dispose) => {
      const budget = countBudget(1);
      const rows = Array.from({ length: 10 }, (_, index) => ({ y: index * 30, h: 30, id: index }));
      const [offset, setOffset] = createSignal(0);
      const pool = createSlotPool(() => rows, offset, { view: 90, overscan: 0, minRowH: 30, budget });
      const painted: string[] = [];
      for (const slot of pool.slots) pool.attach(slot, (row, skeleton) => painted.push(`${row?.id ?? "-"}${skeleton ? "s" : ""}`));
      // One full paint per frame; the other rows in the band show skeletons.
      expect(painted.filter((paint) => !paint.endsWith("s") && paint !== "-")).toHaveLength(1);
      painted.length = 0;
      // The two skeletons in view are owed full paints, one per frame.
      frame(budget);
      frame(budget);
      frame(budget);
      expect(painted).toEqual(["1", "2"]);
      painted.length = 0;
      setOffset(60);
      frame(budget);
      expect(painted.some((paint) => paint.endsWith("s"))).toBe(true);
      dispose();
    });
  });

  test("a row with the content the slot drew is only moved", () => {
    createRoot((dispose) => {
      const budget = countBudget(4);
      const content = { text: "a" };
      const [rows, setRows] = createSignal([{ y: 0, h: 30, content }]);
      const pool = createSlotPool(rows, () => 0, { view: 90, overscan: 0, minRowH: 30, budget });
      const painted: string[] = [];
      pool.attach(0, (row, skeleton, moved) => painted.push(`${row?.y}${skeleton ? "s" : ""}${moved ? "m" : ""}`));
      setRows([{ y: 10, h: 30, content }]);
      frame(budget);
      setRows([{ y: 20, h: 30, content: { text: "b" } }]);
      frame(budget);
      expect(painted).toEqual(["0", "10m", "20"]);
      dispose();
    });
  });

  test("a pending row is painted as its skeleton and fully once it is ready", () => {
    createRoot((dispose) => {
      const budget = countBudget(4);
      const [rows, setRows] = createSignal<readonly MeasuredRow<number, { h: number }>[]>([{ item: 0, index: 0, y: 0, h: 30, value: null, pending: true }]);
      const pool = createSlotPool(rows, () => 0, { view: 90, overscan: 0, minRowH: 30, budget });
      const painted: string[] = [];
      pool.attach(0, (row, skeleton) => painted.push(`${row?.item ?? "-"}${skeleton ? "s" : ""}`));
      expect(painted).toEqual(["0s"]);
      setRows([{ item: 0, index: 0, y: 0, h: 30, value: { h: 30 }, pending: false }]);
      frame(budget);
      expect(painted).toEqual(["0s", "0"]);
      dispose();
    });
  });
});
